# Finite native pacer TTLs require this fix on the pinned M137 checkout.
# Check normalized source, so Windows CRLF and Linux LF have the same identity.
# The native-send CTest additionally detects stale, unpatched prebuilt binaries.
file(READ "${WEBRTC_ROOT}/modules/pacing/prioritized_packet_queue.cc" LK_PACER_SOURCE)
string(REPLACE "\r\n" "\n" LK_PACER_SOURCE "${LK_PACER_SOURCE}")
string(SHA256 LK_PACER_SHA256 "${LK_PACER_SOURCE}")
if(NOT LK_PACER_SHA256 STREQUAL "ad610ee3bfca53ce02eb53d6cc08c46595606eac20687b84699b1d9c72788124")
  message(FATAL_ERROR
    "Unsupported/unpatched WebRTC pacer (${LK_PACER_SHA256}). See patches/README.md. "
    "Apply patches/webrtc-m137-pacer-pause-ttl.patch to the pinned WEBRTC_ROOT and "
    "rebuild any WEBRTC_PREBUILT_DIR library; do not disable native packet TTLs.")
endif()
unset(LK_PACER_SOURCE)
unset(LK_PACER_SHA256)
