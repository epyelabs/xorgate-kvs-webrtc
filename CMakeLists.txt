cmake_minimum_required(VERSION 3.6.3)
project(xorgate-kvs-webrtc C)

# ---------------------------------------------------------------------------
# AWS KVS WebRTC SDK location.
#
# By default we consume the unmodified upstream SDK that is vendored as a git
# submodule under third_party/. It must be BUILT first (see readme.md). Point
# -DKVS_SDK_DIR=... at a prebuilt checkout elsewhere if you don't use the
# submodule, and -DKVS_BUILD_DIR=... if the SDK build output isn't in <sdk>/build.
# ---------------------------------------------------------------------------
set(KVS_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/amazon-kinesis-video-streams-webrtc-sdk-c"
    CACHE PATH "Path to a built amazon-kinesis-video-streams-webrtc-sdk-c checkout")
set(KVS_BUILD_DIR   "${KVS_SDK_DIR}/build"       CACHE PATH "SDK build dir containing libkvsWebrtcClient")
set(KVS_OPENSRC_DIR "${KVS_SDK_DIR}/open-source" CACHE PATH "SDK bundled-dependency prefix")

if(NOT EXISTS "${KVS_SDK_DIR}/src/include")
  message(FATAL_ERROR
    "KVS SDK not found at ${KVS_SDK_DIR}. Run 'git submodule update --init' and build the SDK first "
    "(see readme.md), or pass -DKVS_SDK_DIR=<path-to-built-sdk>.")
endif()

# GStreamer (the media pipeline)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST     REQUIRED gstreamer-1.0)
pkg_check_modules(GLIB2   REQUIRED glib-2.0)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GOBJ2   REQUIRED gobject-2.0)

# Shared include + link config applied to every target below.
set(XORGATE_KVS_INCLUDE_DIRS
  src
  ${KVS_SDK_DIR}/src/include
  ${KVS_OPENSRC_DIR}/include
  ${GST_INCLUDE_DIRS} ${GLIB2_INCLUDE_DIRS} ${GST_APP_INCLUDE_DIRS} ${GOBJ2_INCLUDE_DIRS})

set(XORGATE_KVS_LINK_DIRS
  ${KVS_BUILD_DIR}
  ${KVS_OPENSRC_DIR}/lib
  ${KVS_OPENSRC_DIR}/lib64
  ${GST_LIBRARY_DIRS} ${GLIB2_LIBRARY_DIRS} ${GST_APP_LIBRARY_DIRS} ${GOBJ2_LIBRARY_DIRS})

set(XORGATE_KVS_LIBS
  kvsWebrtcClient kvsWebrtcSignalingClient
  kvsCommonLws kvspicUtils websockets
  # If the linker reports undefined references, uncomment the SDK's private deps:
  # kvspicState srtp2 usrsctp ssl crypto
  ${GST_LIBRARIES} ${GLIB2_LIBRARIES} ${GST_APP_LIBRARIES} ${GOBJ2_LIBRARIES}
  pthread m dl)

set(XORGATE_KVS_RPATH "${KVS_BUILD_DIR};${KVS_OPENSRC_DIR}/lib;${KVS_OPENSRC_DIR}/lib64")

# --- Device master: streams the camera(s) to KVS as MASTER ---
add_executable(xorgate-kvs-webrtc
  src/main.c
  src/Common.c
  src/GstMedia.c)

# Must match the feature flags the SDK was built with. ENABLE_DATA_CHANNEL is ON by
# default in the SDK, and main.c / Common.c guard onDataChannel behind it.
target_compile_definitions(xorgate-kvs-webrtc PRIVATE ENABLE_DATA_CHANNEL)
target_include_directories(xorgate-kvs-webrtc PRIVATE ${XORGATE_KVS_INCLUDE_DIRS})
target_link_directories(xorgate-kvs-webrtc PRIVATE ${XORGATE_KVS_LINK_DIRS})
target_link_libraries(xorgate-kvs-webrtc ${XORGATE_KVS_LIBS})

# Let the binary find the SDK shared libs at runtime without LD_LIBRARY_PATH.
set_target_properties(xorgate-kvs-webrtc PROPERTIES BUILD_RPATH "${XORGATE_KVS_RPATH}")

# --- Cloud viewer-recorder: joins a channel as VIEWER, records H.264 -> MP4 ---
# Built into the Fargate recorder image (see Dockerfile). Shares the SDK sample glue
# (Common.c) but uses recorder.c as entry + GstRecorder.c for the record pipeline.
add_executable(xorgate-kvs-recorder
  src/recorder.c
  src/Common.c
  src/GstRecorder.c)

target_compile_definitions(xorgate-kvs-recorder PRIVATE ENABLE_DATA_CHANNEL)
target_include_directories(xorgate-kvs-recorder PRIVATE ${XORGATE_KVS_INCLUDE_DIRS})
target_link_directories(xorgate-kvs-recorder PRIVATE ${XORGATE_KVS_LINK_DIRS})
target_link_libraries(xorgate-kvs-recorder ${XORGATE_KVS_LIBS})
set_target_properties(xorgate-kvs-recorder PROPERTIES BUILD_RPATH "${XORGATE_KVS_RPATH}")
