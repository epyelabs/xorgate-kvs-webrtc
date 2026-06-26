# Cloud viewer-recorder image (Fargate, linux/amd64).
#
# Builds the KVS WebRTC C SDK + the `xorgate-kvs-recorder` target, then assembles a
# slim runtime with GStreamer (appsrc/h264parse/mp4mux/filesink) + the AWS CLI for
# the S3 upload. The recorder joins a device's KVS channel as a VIEWER and remuxes
# the incoming H.264 to MP4 (see src/recorder.c, src/GstRecorder.c).
#
# Build (from xorgate/device/xorgate-kvs-webrtc, with submodules checked out).
# Match the Fargate task arch (recorder-stack.ts uses ARM64 / Graviton):
#   docker build --platform linux/arm64 -t xorgate-recorder .
# Then tag + push to the ECR repo created by recorder-stack.ts.

# ---- Build stage ----
FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ca-certificates \
    libssl-dev libcurl4-openssl-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# 1) Build the vendored KVS WebRTC SDK (libs only). This also builds its bundled
#    open-source deps under third_party/.../open-source.
RUN cd third_party/amazon-kinesis-video-streams-webrtc-sdk-c \
    && mkdir -p build && cd build \
    && cmake -DBUILD_SAMPLE=OFF .. \
    && make -j"$(nproc)"

# 2) Build the recorder (and master) targets against the freshly built SDK.
RUN mkdir -p build && cd build \
    && cmake .. \
    && make -j"$(nproc)" xorgate-kvs-recorder

# Collect the SDK shared libs the binary needs at runtime, plus the SDK's CA bundle
# (the KVS SDK verifies AWS endpoints against this, via AWS_KVS_CACERT_PATH).
RUN mkdir -p /out/lib \
    && cp -a third_party/amazon-kinesis-video-streams-webrtc-sdk-c/build/*.so* /out/lib/ 2>/dev/null || true \
    && cp -a third_party/amazon-kinesis-video-streams-webrtc-sdk-c/open-source/lib/*.so* /out/lib/ 2>/dev/null || true \
    && cp -a third_party/amazon-kinesis-video-streams-webrtc-sdk-c/open-source/lib64/*.so* /out/lib/ 2>/dev/null || true \
    && cp third_party/amazon-kinesis-video-streams-webrtc-sdk-c/certs/cert.pem /out/cert.pem \
    && cp build/xorgate-kvs-recorder /out/

# ---- Runtime stage ----
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl jq awscli \
    libssl3 libcurl4 \
    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/lib/ /opt/kvs/lib/
COPY --from=build /out/cert.pem /opt/kvs/cert.pem
COPY --from=build /out/xorgate-kvs-recorder /usr/local/bin/xorgate-kvs-recorder
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/xorgate-kvs-recorder

ENV RECORD_DIR=/tmp
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
