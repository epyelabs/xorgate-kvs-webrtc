# Xorgate KVS WebRTC

A standalone WebRTC **master** (camera sender) for AWS Kinesis Video Streams, built on the
[`amazon-kinesis-video-streams-webrtc-sdk-c`](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c)
SDK.

The AWS SDK is vendored **unmodified** as a git submodule under `third_party/` so it stays
upstream-tracked. All of our own code lives in `src/` (a customized copy of the SDK's master
GStreamer sample), so AWS updates never conflict with it.

## What's different from the stock sample

The GStreamer sender pipeline is configurable at runtime via the `KVS_GST_PIPELINE`
environment variable, which points at a **file** containing a `gst-launch` pipeline string:

- `KVS_GST_PIPELINE` set -> read that file and use its contents as the pipeline.
- unset, or the file is missing / empty / unreadable -> warn and fall back to the in-code
  default pipeline (`DEFAULT_GST_PIPELINE` in [`src/GstMedia.c`](src/GstMedia.c)).

The pipeline must end in `appsink ... name=appsink-video`, and any software encoder should keep
`name=sampleVideoEncoder` so TWCC bitrate adaptation works. See [`pipeline.example`](pipeline.example).

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

Provide AWS credentials in the environment (or IoT certs, per the SDK docs), then:

```bash
cd build

# In-code default pipeline:
./xorgate-kvs-webrtc some-channel-id

# File-driven pipeline:
KVS_GST_PIPELINE=../pipeline.example ./xorgate-kvs-webrtc some-channel-id
```

Verify a session by opening the same channel in the AWS console's WebRTC test viewer.

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
