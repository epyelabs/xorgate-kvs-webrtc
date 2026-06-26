# Xorgate KVS WebRTC

WebRTC clients for AWS Kinesis Video Streams, built on the
[`amazon-kinesis-video-streams-webrtc-sdk-c`](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c)
SDK.

The AWS SDK is vendored **unmodified** as a git submodule under `third_party/` so it stays
upstream-tracked. All of our own code lives in `src/` (customized copies of the SDK's
GStreamer samples), so AWS updates never conflict with it.

## Build targets

This project builds **two** executables from the same `src/` + vendored SDK:

| Target | Role | Where it runs | Entry + media file |
|--------|------|---------------|--------------------|
| `xorgate-kvs-webrtc` | **master** (camera sender) | on the device (RPi, aarch64), one process per camera, spawned by the device agent | `src/main.c` + `src/GstMedia.c` |
| `xorgate-kvs-recorder` | **viewer-recorder** (records a stream to MP4) | in the cloud (AWS Fargate, arm64), one task per recording, launched by the backend | `src/recorder.c` + `src/GstRecorder.c` |

Both share `src/Common.c` (signaling / peer-connection glue) and the same KVS SDK + GStreamer.
The master *sends* frames (encoder → `appsink` → `writeFrame`); the recorder *receives* them
(`transceiverOnFrame` → `appsrc` → `h264parse` → `mp4mux` → `filesink`) and uploads the finished
MP4 to S3. A change to shared code (`Common.c`, or an SDK bump) means **rebuild both**: the
device master (below) and the recorder image (see [Cloud viewer-recorder](#cloud-viewer-recorder-xorgate-kvs-recorder)).

## What's different from the stock sample

### Configurable GStreamer pipeline

The GStreamer sender pipeline is configurable at runtime via the `KVS_GST_PIPELINE`
environment variable, which points at a **file** containing a `gst-launch` pipeline string:

- `KVS_GST_PIPELINE` set -> read that file and use its contents as the pipeline.
- unset, or the file is missing / empty / unreadable -> warn and fall back to the in-code
  default pipeline (`DEFAULT_GST_PIPELINE` in [`src/GstMedia.c`](src/GstMedia.c)).

The pipeline must end in `appsink ... name=appsink-video`, and any software encoder should keep
`name=sampleVideoEncoder` so TWCC bitrate adaptation works. See [`pipeline.example`](pipeline.example).

### Runtime credential selection

The stock sample chooses between static AWS keys and AWS IoT Core certificate credentials at **compile
time** via `-DIOT_CORE_ENABLE_CREDENTIALS`. This app decides at **runtime** instead, so a single binary
does both:

- If `AWS_IOT_CORE_CREDENTIAL_ENDPOINT` is set, it uses IoT cert-based credentials (and also requires
  `AWS_IOT_CORE_CERT`, `AWS_IOT_CORE_PRIVATE_KEY`, `AWS_IOT_CORE_ROLE_ALIAS`, `AWS_IOT_CORE_THING_NAME`).
- Otherwise it uses static credentials (`AWS_ACCESS_KEY_ID` + `AWS_SECRET_ACCESS_KEY`).

#### Channel name resolution

The channel name is the first command-line argument in both modes (`./xorgate-kvs-webrtc my-channel-name`).
In IoT mode there are additional fallbacks. The full precedence is:

```
AWS_IOT_CORE_CERTIFICATE_ID   (IoT mode only; if set, overrides everything below)
  > argv[1]                   (the command-line argument)
    > AWS_IOT_CORE_THING_NAME (IoT mode env fallback when no argument is given)
      > ScaryTestChannel      (built-in sample default)
```

So to name the channel explicitly, pass `argv[1]` and leave `AWS_IOT_CORE_CERTIFICATE_ID` unset (it is
optional, not one of the required IoT vars). Set it only if you want the channel named after the cert ID.

## Layout

```
src/                  our app (copied + customized SDK sample glue)
  main.c              MASTER entry point (was kvsWebRTCClientMasterGstSample.c)
  GstMedia.c          GStreamer sender + KVS_GST_PIPELINE support (master)
  recorder.c          RECORDER entry point (cloud viewer; was the viewer sample)
  GstRecorder.c       GStreamer receiver -> mp4mux -> filesink (recorder)
  Common.c            signaling / peer-connection orchestration (shared by both)
  GstMedia.h Samples.h
third_party/
  amazon-kinesis-video-streams-webrtc-sdk-c/   unmodified AWS SDK (submodule, pinned to v1.19.1)
CMakeLists.txt        builds BOTH targets, links against the built SDK
Dockerfile            builds the recorder container image (Fargate)
entrypoint.sh         recorder container entrypoint (task-role creds + CA cert + exec)
pipeline.example      sample KVS_GST_PIPELINE file (videotestsrc, no camera needed)
```

## Build (On Remote Device i.e RaspberrPi)

Requires GStreamer dev packages and a toolchain (Linux / Raspberry Pi). The SDK is built once,
then the app links against it.

```bash
# 1. Clone this repo
git clone https://github.com/epyelabs/xorgate-kvs-webrtc.git

# 2. Fetch the SDK submodule (only needed after a fresh clone)
cd xorgate-kvs-webrtc
git submodule update --init --progress

# 3. Install required libraries

# Install guide from amazon-kinesis-video-streams-webrtc-sdk-c
sudo apt-get install cmake m4 pkg-config libssl-dev libcurl4-openssl-dev liblog4cplus-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base-apps gstreamer1.0-plugins-bad gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-tools

# Additional library needed for latest RPi OS
sudo apt install gstreamer1.0-libcamera

# 4. Build the SDK once (samples off; it fetches its own deps into open-source/)
cd third_party/amazon-kinesis-video-streams-webrtc-sdk-c
mkdir -p build && cd build && cmake -DBUILD_SAMPLE=OFF .. && make
cd ../../..

# 5. Build the app

# `make` builds both targets; on the device you only need the master:
mkdir -p build && cd build && cmake .. && make xorgate-kvs-webrtc
```

The recorder target (`make xorgate-kvs-recorder`) is for the cloud and is normally built via
the [`Dockerfile`](Dockerfile), not on the device — see [Cloud viewer-recorder](#cloud-viewer-recorder-xorgate-kvs-recorder).

If the SDK's libraries aren't under `third_party/.../build`, pass
`-DKVS_BUILD_DIR=<dir containing libkvsWebrtcClient.*>` to step 3.

## Sample content of GST Pipe (KVS_GST_PIPELINE)

```
libcamerasrc ! video/x-raw,format=I420,width=1280,height=720,framerate=15/1 !
x264enc bframes=0 speed-preset=ultrafast bitrate=1000 byte-stream=TRUE tune=zerolatency threads=2 !
video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline !
appsink sync=TRUE emit-signals=TRUE name=appsink-video
```

## Run

A CA cert path is required in **both** credential modes (TLS to AWS, and the credential provider in IoT
mode). Set it and the region from the repo root:

```bash
export AWS_KVS_CACERT_PATH="$PWD/third_party/amazon-kinesis-video-streams-webrtc-sdk-c/certs/cert.pem"
export AWS_DEFAULT_REGION=us-west-2     # match your channel's region
```

Then pick a credential mode (see [Runtime credential selection](#runtime-credential-selection)) and run
from the build dir:

```bash
cd build

# --- Static credentials ---
export AWS_ACCESS_KEY_ID=...
export AWS_SECRET_ACCESS_KEY=...
KVS_GST_PIPELINE=../pipeline.example ./xorgate-kvs-webrtc my-channel-name

# --- IoT Core credentials (channel name defaults to the thing name when no arg is given) ---
export AWS_IOT_CORE_CREDENTIAL_ENDPOINT=...
export AWS_IOT_CORE_CERT=/path/to/certificate.pem
export AWS_IOT_CORE_PRIVATE_KEY=/path/to/private.key
export AWS_IOT_CORE_ROLE_ALIAS=...
export AWS_IOT_CORE_THING_NAME=...
KVS_GST_PIPELINE=../pipeline.example ./xorgate-kvs-webrtc
```

Without `KVS_GST_PIPELINE`, the in-code default pipeline is used. Verify a session by opening the same
channel in the AWS console's WebRTC test viewer.

## Cloud viewer-recorder (`xorgate-kvs-recorder`)

The recorder is the cloud-side counterpart of the device master. Instead of running on the
device, it runs as an **AWS Fargate task** that joins a device's existing KVS signaling channel
as **one more VIEWER** (the master already serves multiple viewers, so the device and the
browser's live view are unaffected), receives the H.264, remuxes it to a single MP4 with no
re-encode, and uploads it to S3 on stop. It is driven by the backend
(`xorgate-core-service`): an API/Lambda calls ECS `RunTask`/`StopTask` (see that repo's
`lib/stacks/recorder-stack.ts` and `src/services/recordings.ts`).

How it differs from the master:

- **Viewer role**, not master: `src/recorder.c` sends the SDP offer and records what comes back
  (`src/GstRecorder.c` builds `appsrc ! h264parse ! mp4mux faststart ! filesink`, video-only).
- **Credentials are static AWS keys from the Fargate task role**, not the device IoT cert.
  `entrypoint.sh` fetches the task-role creds from the ECS container-credentials endpoint and
  exports `AWS_ACCESS_KEY_ID/SECRET/SESSION_TOKEN`, because the KVS C SDK only reads static env
  creds. Leave `AWS_IOT_CORE_CREDENTIAL_ENDPOINT` unset so the SDK takes the static path.
- **TLS CA**: same gotcha as the device (see [`../xorgate-device-agent/media/README.md`](../xorgate-device-agent/media/README.md)).
  It uses the SDK's bundled `certs/cert.pem` (baked into the image at `/opt/kvs/cert.pem`), not
  the system bundle, or KVS fails with `unable to get local issuer certificate (X509_V_ERR=20)`.
- **Graceful stop**: on `SIGTERM` (from ECS `StopTask`) it sends EOS so `mp4mux` writes the moov
  atom (a seekable, playable MP4), then uploads via the AWS CLI. The task `stopTimeout` (120s)
  gives it time to finalize + upload before SIGKILL.

Config is passed as container env (set by the backend's `RunTask`): `KVS_CHANNEL_NAME`
(`<deviceId>-<streamKey>`), `AWS_REGION`, `RECORDING_ID`, `DEVICE_ID`, `STREAM_KEY`,
`S3_BUCKET`, `S3_KEY`.

### Deploy a recorder code change

The recorder image lives in ECR (dev: `xorgate-recorder-dev`, prod: `xorgate-recorder`) and the
Fargate task always pulls the `:latest` tag at run time. So to ship a change to `recorder.c` /
`GstRecorder.c` / `Common.c` / the `Dockerfile`, just **rebuild and push the image** — no CDK
redeploy is needed unless you changed the task definition or infra.

```bash
# from xorgate/device/xorgate-kvs-webrtc (submodules checked out, Docker running)
# dev ECR repo (from the recorder-stack "RecorderRepoUri" output):
#   111910761410.dkr.ecr.us-east-1.amazonaws.com/xorgate-recorder-dev

# 1. build for the Fargate task arch (recorder-stack uses ARM64 / Graviton)
docker build --platform linux/arm64 \
  -t 111910761410.dkr.ecr.us-east-1.amazonaws.com/xorgate-recorder-dev:latest --load .

# 2. log in to ECR and push
aws ecr get-login-password --region us-east-1 \
  | docker login --username AWS --password-stdin 111910761410.dkr.ecr.us-east-1.amazonaws.com
docker push 111910761410.dkr.ecr.us-east-1.amazonaws.com/xorgate-recorder-dev:latest
```

The next recording (manual or auto) launches a fresh task on the new image; in-flight
recordings keep the old image until they stop. The multi-stage `Dockerfile` compiles the
vendored SDK from scratch, so the first build is slow and later builds reuse the cached SDK
layer. Build for `linux/arm64` to match Graviton Fargate (a native build on an arm64 Mac).

If you change the **task definition or infra** (CPU/mem, container env, IAM, VPC, CPU arch),
redeploy the CDK stacks from `xorgate-core-service` instead:

```bash
cd <xorgate-core-service>
npx cdk deploy --context stage=dev XorgateCoreRecorderStackDev
# also deploy XorgateCoreApiStackDev if its recorder env / IAM changed
```

> Changing the task CPU architecture replaces the task definition, whose ARN was previously a
> cross-stack export. The api-stack now references the task **family** (a stable string) instead,
> so future task-def changes don't hit the "cannot update an export in use" deadlock.

### Test the recorder against a live channel

With a device streaming as master (so there's media to view), launch a one-off task and watch
it record to S3:

```bash
aws ecs run-task --region us-east-1 --cluster xorgate-recorder-dev \
  --task-definition xorgate-recorder-dev --launch-type FARGATE \
  --network-configuration 'awsvpcConfiguration={subnets=[<public-subnet>],securityGroups=[<sg>],assignPublicIp=ENABLED}' \
  --overrides '{"containerOverrides":[{"name":"recorder","environment":[
    {"name":"AWS_REGION","value":"us-east-1"},
    {"name":"KVS_CHANNEL_NAME","value":"<deviceId>-cam0"},
    {"name":"RECORDING_ID","value":"<uuid>"},
    {"name":"S3_BUCKET","value":"xorgate-core-dev-<acct>-us-east-1"},
    {"name":"S3_KEY","value":"media/v1/<deviceId>/cam0/<uuid>.mp4"}]}]}'
# ... then `aws ecs stop-task` to finalize; the MP4 lands at the S3_KEY above.
```

Logs stream to the `/xorgate/recorder-dev` CloudWatch log group.

## Updating the SDK from upstream AWS

```bash
cd third_party/amazon-kinesis-video-streams-webrtc-sdk-c
git fetch origin
git checkout <newer-tag>        # e.g. v1.20.0   (or: git checkout master && git pull)
cd ../..
git add third_party/amazon-kinesis-video-streams-webrtc-sdk-c
git commit -m "Bump KVS SDK to <newer-tag>"
```

Then rebuild the SDK (build step 2) and the app (build step 3). Our `src/` is untouched by the bump.
