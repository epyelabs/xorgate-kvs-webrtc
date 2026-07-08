#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/app.h>
#include <stdlib.h>

/*
 * Cloud viewer-recorder receive pipeline.
 *
 * This is the VIEWER-side counterpart of GstMedia.c's master sender. It implements
 * `receiveGstreamerAudioVideo` (declared in Samples.h, wired in via
 * pSampleConfiguration->receiveAudioVideoSource by recorder.c): every incoming
 * H.264 frame from the device master is pushed into an appsrc and remuxed — without
 * re-encoding — into a single MP4 written to <RECORD_DIR>/<RECORDING_ID>.mp4.
 *
 * Adapted from the upstream samples/p2p/GstAudioVideoReceiver.c, trimmed to
 * video-only and pointed at mp4mux+filesink. On streaming-session shutdown we send
 * EOS so mp4mux writes the moov atom and the file is a valid, seekable MP4; recorder.c
 * then uploads it to S3.
 */

static UINT64 presentationTsIncrement = 0;
static volatile BOOL recorderEos = FALSE;

// Capture heartbeat state (kvs-webrtc-lts Phase 4 / §4.5). Driven off the frame
// callback (serialized per stream, so no lock needed): frames arriving is the ONLY
// honest proof the media plane is alive, so we publish only while they do.
static UINT64 gCaptureFrameCount = 0;   // frames received this recording
static UINT64 gLastHeartbeatTime = 0;   // GETTIME() of last publish; 0 = none yet
static UINT64 gHeartbeatIntervalTime = 0; // cadence in 100ns units; 0 until parsed

// Heartbeat cadence from RECORDER_HEARTBEAT_MS (default 12s), parsed once.
static UINT64 heartbeatIntervalTime(VOID)
{
    if (gHeartbeatIntervalTime == 0) {
        PCHAR pMs = GETENV("RECORDER_HEARTBEAT_MS");
        UINT64 ms = (pMs != NULL && pMs[0] != '\0') ? (UINT64) strtoull(pMs, NULL, 10) : 0;
        if (ms == 0) ms = 12000;
        gHeartbeatIntervalTime = ms * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    }
    return gHeartbeatIntervalTime;
}

// Fire-and-forget: publish a "capturing" heartbeat to IoT Core. The backend's
// recording-capture rule stamps capture_started_at on the FIRST message (UI flips
// "Starting…" -> "REC") and bumps capture_heartbeat_at on EVERY message — the
// media-plane liveness the §4.5 offline-stop corroboration + the orphan reconciler
// check against CAPTURE_STALE_MS. Because we only call this while frames actually
// arrive, a stalled media plane -> heartbeat stops -> backend can tell.
//
// Backgrounded with a trailing `&` so it never blocks the media-receive thread,
// mirroring the `system()` upload idiom in recorder.c. The AWS CLI picks up the
// task-role creds + region from entrypoint.sh's env exports (same as the s3 upload).
// A fileb:// payload is raw bytes in aws-cli v1/v2, sidestepping v2's
// --cli-binary-format requirement.
static VOID publishCaptureHeartbeat(UINT64 frames)
{
    PCHAR pRecordingId = GETENV("RECORDING_ID");
    PCHAR pEndpoint = GETENV("IOT_DATA_ENDPOINT");
    CHAR endpointArg[256];
    CHAR payload[256];
    CHAR cmd[2048];

    if (pRecordingId == NULL) {
        DLOGW("[recorder] RECORDING_ID unset; skipping capture heartbeat");
        return;
    }

    // The bundled aws-cli v1 defaults iot-data to the deprecated non-ATS endpoint
    // (data.iot.<region>), whose cert is no longer trusted; entrypoint.sh resolves the
    // account ATS data endpoint into IOT_DATA_ENDPOINT so we can target it explicitly.
    endpointArg[0] = '\0';
    if (pEndpoint != NULL && pEndpoint[0] != '\0') {
        SNPRINTF(endpointArg, SIZEOF(endpointArg), "--endpoint-url 'https://%s' ", pEndpoint);
    }

    // JSON with real double-quotes (safe inside the shell single-quoted printf below;
    // it contains no single-quotes). ts is informational (backend uses server now()).
    SNPRINTF(payload, SIZEOF(payload),
             "{\"state\":\"capturing\",\"frames\":%llu,\"ts\":%llu}",
             (unsigned long long) frames,
             (unsigned long long) (GETTIME() / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));

    // Backgrounded subshell so it never blocks the media-receive thread; only logs on
    // failure so a ~12s cadence doesn't spam CloudWatch.
    SNPRINTF(cmd, SIZEOF(cmd),
             "( printf '%%s' '%s' > /tmp/%s.cap.json; "
             "if aws iot-data publish --topic 'xorgate/recordings/%s/status' "
             "%s--payload fileb:///tmp/%s.cap.json >/tmp/%s.cap.out 2>&1; "
             "then :; "
             "else echo '[recorder] capture heartbeat FAILED'; cat /tmp/%s.cap.out; fi ) &",
             payload, pRecordingId, pRecordingId, endpointArg, pRecordingId, pRecordingId, pRecordingId);
    if (system(cmd) != 0) {
        DLOGW("[recorder] capture heartbeat dispatch returned non-zero");
    }
}

// Publish a heartbeat on the first frame (capture-started) and then at most once per
// heartbeat interval while frames keep flowing. Called from the frame callback.
static VOID maybeHeartbeat(VOID)
{
    UINT64 now = GETTIME();
    gCaptureFrameCount++;
    if (gLastHeartbeatTime == 0 || (now - gLastHeartbeatTime) >= heartbeatIntervalTime()) {
        if (gLastHeartbeatTime == 0) {
            DLOGI("[recorder] first frame — signaling capture started");
        }
        gLastHeartbeatTime = now;
        publishCaptureHeartbeat(gCaptureFrameCount);
    }
}

// Transceiver callback: push each received H.264 access unit into appsrc-video.
static VOID onGstVideoFrameReady(UINT64 customData, PFrame pFrame)
{
    GstFlowReturn ret;
    GstBuffer* buffer;
    GstElement* appsrcVideo = (GstElement*) customData;

    if (appsrcVideo == NULL || pFrame == NULL || recorderEos) {
        return;
    }

    buffer = gst_buffer_new_allocate(NULL, pFrame->size, NULL);
    if (buffer == NULL) {
        DLOGE("[recorder] buffer allocation failed");
        return;
    }

    GST_BUFFER_DTS(buffer) = presentationTsIncrement;
    GST_BUFFER_PTS(buffer) = presentationTsIncrement;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, DEFAULT_FPS_VALUE);
    presentationTsIncrement += gst_util_uint64_scale(1, GST_SECOND, DEFAULT_FPS_VALUE);

    if (gst_buffer_fill(buffer, 0, pFrame->frameData, pFrame->size) != pFrame->size) {
        DLOGE("[recorder] buffer fill incomplete");
        gst_buffer_unref(buffer);
        return;
    }
    g_signal_emit_by_name(appsrcVideo, "push-buffer", buffer, &ret);
    if (ret != GST_FLOW_OK) {
        DLOGE("[recorder] push-buffer error: %s", gst_flow_get_name(ret));
    }
    gst_buffer_unref(buffer);

    // A frame arrived -> the media plane is alive. Heartbeat on the first frame
    // (capture-started) and periodically thereafter (the transceiver callback is
    // serialized per stream, so the counters need no lock).
    maybeHeartbeat();
}

// Streaming-session shutdown -> EOS so mp4mux finalizes a playable MP4.
static VOID onRecorderSessionShutdown(UINT64 customData, PSampleStreamingSession pSampleStreamingSession)
{
    (void) pSampleStreamingSession;
    GstElement* pipeline = (GstElement*) customData;
    recorderEos = TRUE;
    if (pipeline != NULL) {
        gst_element_send_event(pipeline, gst_event_new_eos());
    }
}

PVOID receiveGstreamerAudioVideo(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *pipeline = NULL, *appsrcVideo = NULL;
    GstBus* bus = NULL;
    GstMessage* msg = NULL;
    GError* error = NULL;
    GstCaps* videocaps = NULL;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    CHAR pipelineDescription[1024];
    CHAR recordPath[768];
    PCHAR pRecordDir, pRecordingId;
    BOOL isH265 = FALSE;

    CHK_ERR(pSampleStreamingSession != NULL, STATUS_NULL_ARG, "[recorder] streaming session is NULL");
    CHK_ERR(gst_init_check(NULL, NULL, &error), STATUS_INTERNAL_ERROR, "[recorder] GStreamer init failed");

    pRecordDir = GETENV("RECORD_DIR");
    if (pRecordDir == NULL) pRecordDir = (PCHAR) "/tmp";
    pRecordingId = GETENV("RECORDING_ID");
    if (pRecordingId == NULL) pRecordingId = (PCHAR) "recording";
    SNPRINTF(recordPath, SIZEOF(recordPath), "%s/%s.mp4", pRecordDir, pRecordingId);

    isH265 = (pSampleStreamingSession->pVideoRtcRtpTransceiver != NULL &&
              pSampleStreamingSession->pVideoRtcRtpTransceiver->receiver.track.codec == RTC_CODEC_H265);

    // Video-only: depay is already done by the SDK (frames arrive as access units),
    // so we just parse + mux + write. faststart=true relocates the moov atom to the
    // front on EOS for progressive/web playback.
    SNPRINTF(pipelineDescription, SIZEOF(pipelineDescription),
             "appsrc name=appsrc-video ! queue ! %s ! queue ! mp4mux faststart=true name=mux ! filesink location=%s",
             isH265 ? "h265parse" : "h264parse", recordPath);

    DLOGI("[recorder] recording to %s (codec=%s)", recordPath, isH265 ? "h265" : "h264");

    pipeline = gst_parse_launch(pipelineDescription, &error);
    CHK_ERR(pipeline != NULL, STATUS_INTERNAL_ERROR, "[recorder] pipeline is NULL");

    appsrcVideo = gst_bin_get_by_name(GST_BIN(pipeline), "appsrc-video");
    CHK_ERR(appsrcVideo != NULL, STATUS_INTERNAL_ERROR, "[recorder] cannot find appsrc-video");

    if (isH265) {
        videocaps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", NULL);
    } else {
        videocaps = gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", NULL);
    }
    g_object_set(G_OBJECT(appsrcVideo), "caps", videocaps, "format", GST_FORMAT_TIME, "is-live", TRUE, NULL);
    gst_caps_unref(videocaps);
    videocaps = NULL;

    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver, (UINT64) appsrcVideo, onGstVideoFrameReady));
    CHK_STATUS(streamingSessionOnShutdown(pSampleStreamingSession, (UINT64) pipeline, onRecorderSessionShutdown));

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Block until EOS (clean stop -> finalized MP4) or error.
    bus = gst_element_get_bus(pipeline);
    CHK_ERR(bus != NULL, STATUS_INTERNAL_ERROR, "[recorder] bus is NULL");
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    if (msg != NULL) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg, &error, NULL);
                DLOGE("[recorder] pipeline error: %s", error != NULL ? error->message : "(unknown)");
                break;
            case GST_MESSAGE_EOS:
                DLOGI("[recorder] EOS — MP4 finalized at %s", recordPath);
                break;
            default:
                break;
        }
    }

CleanUp:
    if (error != NULL) {
        DLOGE("[recorder] %s", error->message);
        g_clear_error(&error);
    }
    if (msg != NULL) gst_message_unref(msg);
    if (bus != NULL) gst_object_unref(bus);
    if (pipeline != NULL) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    if (appsrcVideo != NULL) gst_object_unref(appsrcVideo);

    return (PVOID) (ULONG_PTR) retStatus;
}
