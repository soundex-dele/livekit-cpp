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

# A constant ALR bitrate must not retain an obsolete congestion cause in the
# native probe controller. Keep source and prebuilt-library checks separate.
foreach(LK_GOOGCC_FILE IN ITEMS goog_cc_network_control.cc goog_cc_network_control.h)
  file(READ "${WEBRTC_ROOT}/modules/congestion_controller/goog_cc/${LK_GOOGCC_FILE}" LK_GOOGCC_SOURCE)
  string(REPLACE "\r\n" "\n" LK_GOOGCC_SOURCE "${LK_GOOGCC_SOURCE}")
  string(SHA256 LK_GOOGCC_SHA256 "${LK_GOOGCC_SOURCE}")
  if(LK_GOOGCC_FILE STREQUAL "goog_cc_network_control.cc")
    set(LK_GOOGCC_EXPECTED "dd18e88c022594f9841fef63f917b56d1f976b6b698af8c63bb57a38fbe4cb77")
  else()
    set(LK_GOOGCC_EXPECTED "1858ca00fcb5bb0572ee27961791721fbcad448a250be59b7bd7fcdfb0183363")
  endif()
  if(NOT LK_GOOGCC_SHA256 STREQUAL LK_GOOGCC_EXPECTED)
    message(FATAL_ERROR
      "Unsupported/unpatched WebRTC ${LK_GOOGCC_FILE} (${LK_GOOGCC_SHA256}). "
      "Apply patches/webrtc-m137-probe-cause-refresh.patch, rebuild any "
      "WEBRTC_PREBUILT_DIR library, and run native-send. See patches/README.md.")
  endif()
endforeach()
unset(LK_GOOGCC_FILE)
unset(LK_GOOGCC_SOURCE)
unset(LK_GOOGCC_SHA256)
unset(LK_GOOGCC_EXPECTED)
