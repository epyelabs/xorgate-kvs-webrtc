# Xorgate KVS WebRTC

A standalone WebRTC **master** (camera sender) for AWS Kinesis Video Streams, built on the
[`amazon-kinesis-video-streams-webrtc-sdk-c`](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c)
SDK.

The AWS SDK is vendored **unmodified** as a git submodule under `third_party/` so it stays
upstream-tracked. All of our own code lives in `src/` (a customized copy of the SDK's master
GStreamer sample), so AWS updates never conflict with it.

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
  main.c              entry point (was kvsWebRTCClientMasterGstSample.c)
  Common.c            signaling / peer-connection orchestration
  GstMedia.c          GStreamer sender + KVS_GST_PIPELINE support
  GstMedia.h Samples.h
third_party/
  amazon-kinesis-video-streams-webrtc-sdk-c/   unmodified AWS SDK (submodule, pinned to v1.19.1)
CMakeLists.txt        builds the app, links against the built SDK
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

mkdir -p build && cd build && cmake .. && make
```

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
