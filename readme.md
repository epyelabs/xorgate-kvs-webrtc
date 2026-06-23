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

- `KVS_GST_PIPELINE` set  -> read that file and use its contents as the pipeline.
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

## Build

Requires GStreamer dev packages and a toolchain (Linux / Raspberry Pi). The SDK is built once,
then the app links against it.

```bash
# 1. Fetch the SDK submodule (only needed after a fresh clone)
git submodule update --init

# 2. Build the SDK once (samples off; it fetches its own deps into open-source/)
cd third_party/amazon-kinesis-video-streams-webrtc-sdk-c
mkdir -p build && cd build && cmake -DBUILD_SAMPLE=OFF .. && make -j"$(nproc)"
cd ../../..

# 3. Build the app
mkdir -p build && cd build && cmake .. && make -j"$(nproc)"
```

If the SDK's libraries aren't under `third_party/.../build`, pass
`-DKVS_BUILD_DIR=<dir containing libkvsWebrtcClient.*>` to step 3.

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
