#include "Samples.h"
#include <signal.h>
#include <sys/stat.h>

/*
 * xorgate cloud viewer-recorder.
 *
 * Runs in the cloud (a Fargate task) as a KVS WebRTC VIEWER: it joins the device's
 * existing signaling channel as one more viewer (the master serves multiple viewers,
 * so the device + the browser's live P2P viewing are untouched), receives the H.264
 * video, and — via GstRecorder.c's receiveGstreamerAudioVideo — remuxes it to a
 * single MP4. On a clean stop (SIGTERM from ECS StopTask, or SIGINT) the pipeline is
 * finalized and the MP4 is uploaded to S3.
 *
 * Config via env (set by the backend's RunTask + the container entrypoint):
 *   KVS_CHANNEL_NAME   signaling channel to view (= "<deviceId>-<streamKey>")
 *   AWS_DEFAULT_REGION / AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN
 *                      task-role creds, exported by entrypoint.sh from the ECS
 *                      container credentials endpoint (NOT the device IoT cert)
 *   RECORD_DIR         local dir for the MP4 (default /tmp)
 *   RECORDING_ID       file stem -> <RECORD_DIR>/<RECORDING_ID>.mp4
 *   S3_BUCKET, S3_KEY  upload destination (skipped if unset)
 *
 * Modeled on the upstream samples/p2p/kvsWebRTCClientViewerGstSample.c, trimmed to
 * video-only with no data channel, plus SIGTERM handling and the S3 upload.
 */

extern PSampleConfiguration gSampleConfiguration;

// Upload the finalized MP4 to S3 via the AWS CLI (which picks up the task role from
// the ECS container credentials endpoint automatically). Skips an empty/missing file
// so a recording that never connected doesn't land a 0-byte object.
static VOID uploadRecording(VOID)
{
    PCHAR pRecordDir = GETENV("RECORD_DIR");
    PCHAR pRecordingId = GETENV("RECORDING_ID");
    PCHAR pBucket = GETENV("S3_BUCKET");
    PCHAR pKey = GETENV("S3_KEY");
    CHAR recordPath[768];
    CHAR cmd[2048];
    struct stat st;
    INT32 rc;

    if (pRecordDir == NULL) pRecordDir = (PCHAR) "/tmp";
    if (pRecordingId == NULL) pRecordingId = (PCHAR) "recording";
    SNPRINTF(recordPath, SIZEOF(recordPath), "%s/%s.mp4", pRecordDir, pRecordingId);

    if (stat(recordPath, &st) != 0 || st.st_size == 0) {
        DLOGW("[recorder] no recording to upload at %s (never connected?)", recordPath);
        return;
    }
    if (pBucket == NULL || pKey == NULL) {
        DLOGW("[recorder] S3_BUCKET/S3_KEY unset; leaving recording at %s", recordPath);
        return;
    }

    SNPRINTF(cmd, SIZEOF(cmd), "aws s3 cp '%s' 's3://%s/%s' --content-type video/mp4 --only-show-errors", recordPath, pBucket, pKey);
    DLOGI("[recorder] uploading %lld bytes -> s3://%s/%s", (long long) st.st_size, pBucket, pKey);
    rc = system(cmd);
    if (rc != 0) {
        DLOGE("[recorder] S3 upload failed (exit %d)", rc);
    } else {
        DLOGI("[recorder] upload complete");
    }
}

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit offerSessionDescriptionInit;
    UINT32 buffLen = 0;
    SignalingMessage message = {0};
    PSampleConfiguration pSampleConfiguration = NULL;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    BOOL locked = FALSE;
    PCHAR pChannelName;
    CHAR clientId[256];

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();

    // ECS StopTask sends SIGTERM (then SIGKILL after stopTimeout); SIGINT for local
    // runs. Both route to the SDK's handler, which flips the interrupt flag so the
    // block loop below exits and cleanup finalizes + uploads the MP4.
    signal(SIGINT, sigintHandler);
    signal(SIGTERM, sigintHandler);

    pChannelName = argc > 1 ? argv[1] : GETENV("KVS_CHANNEL_NAME");
    CHK_ERR(pChannelName != NULL, STATUS_INVALID_OPERATION, "[recorder] KVS_CHANNEL_NAME (or argv[1]) must be set");

    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_VIEWER, TRUE, TRUE, logLevel, &pSampleConfiguration));
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
    pSampleConfiguration->receiveAudioVideoSource = receiveGstreamerAudioVideo;
    pSampleConfiguration->audioCodec = RTC_CODEC_OPUS;
    pSampleConfiguration->videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    // No data channel for the recorder (onDataChannel stays NULL).

    CHK_STATUS(initKvsWebRtc());
    DLOGI("[recorder] KVS WebRTC initialized; viewing channel %s", pChannelName);

    SPRINTF(clientId, "%s_%u", SAMPLE_VIEWER_CLIENT_ID, RAND() % MAX_UINT32);
    CHK_STATUS(initSignaling(pSampleConfiguration, clientId));
    DLOGI("[recorder] signaling connection established");

    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;
    CHK_STATUS(createSampleStreamingSession(pSampleConfiguration, NULL, FALSE, &pSampleStreamingSession));
    pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount++] = pSampleStreamingSession;
    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = FALSE;

    MEMSET(&offerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));
    offerSessionDescriptionInit.useTrickleIce = pSampleStreamingSession->remoteCanTrickleIce;
    CHK_STATUS(setLocalDescription(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));

    if (!pSampleConfiguration->trickleIce) {
        DLOGI("[recorder] non-trickle ICE; waiting for candidate gathering");
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = TRUE;
        while (!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->candidateGatheringDone)) {
            CHK_WARN(!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag), STATUS_OPERATION_TIMED_OUT,
                     "[recorder] terminated before candidate gathering completed");
            CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock, 5 * HUNDREDS_OF_NANOS_IN_A_SECOND);
        }
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = FALSE;
    }

    CHK_STATUS(createOffer(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));
    CHK_STATUS(serializeSessionDescriptionInit(&offerSessionDescriptionInit, NULL, &buffLen));
    CHK_ERR(buffLen < SIZEOF(message.payload), STATUS_INVALID_OPERATION, "[recorder] offer too large");
    CHK_STATUS(serializeSessionDescriptionInit(&offerSessionDescriptionInit, message.payload, &buffLen));

    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_OFFER;
    STRCPY(message.peerClientId, SAMPLE_MASTER_CLIENT_ID);
    message.payloadLen = (buffLen / SIZEOF(CHAR)) - 1;
    message.correlationId[0] = '\0';
    CHK_STATUS(signalingClientSendMessageSync(pSampleConfiguration->signalingClientHandle, &message));
    DLOGI("[recorder] offer sent; recording until stopped");

    // Block until SIGTERM/SIGINT (interrupt) or the session ends.
    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->interrupted) && !ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag)) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

CleanUp:
    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[recorder] terminated with status 0x%08x", retStatus);
    }
    DLOGI("[recorder] cleaning up...");

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (pSampleConfiguration != NULL) {
        if (pSampleConfiguration->enableFileLogging) {
            freeFileLogger();
        }
        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[recorder] freeSignalingClient(): 0x%08x", retStatus);
        }
        // freeSampleConfiguration -> freeSampleStreamingSession -> shutdown callback
        // (EOS) -> the receive thread finalizes the MP4 (moov atom) and is joined, so
        // the file on disk is complete by the time this returns.
        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[recorder] freeSampleConfiguration(): 0x%08x", retStatus);
        }
    }

    // File is finalized — upload it.
    uploadRecording();

    DLOGI("[recorder] done");
    RESET_INSTRUMENTED_ALLOCATORS();
    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}
