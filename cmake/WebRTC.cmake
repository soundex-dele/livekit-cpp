if(NOT EXISTS "${WEBRTC_ROOT}/api/peer_connection_interface.h")
  message(FATAL_ERROR "Set WEBRTC_ROOT to the native WebRTC checkout")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/CheckWebRTCPatches.cmake")
if(APPLE AND WEBRTC_PREBUILT_DIR)
  set(WEBRTC_SOURCE_DIR "${WEBRTC_ROOT}")
  include("${CMAKE_CURRENT_LIST_DIR}/WebRTCPrebuiltMacOS.cmake")
  return()
endif()
if(WEBRTC_PREBUILT_DIR)
  if(NOT WIN32)
    message(FATAL_ERROR "Prebuilt import currently supports Windows Release; use source build on Linux")
  endif()
  # All objects here are from the selected WebRTC CMake build, never a system SDK.
  file(GLOB WEBRTC_ABSL_LIBS "${WEBRTC_PREBUILT_DIR}/lib/Release/absl_*.lib")
  set(WEBRTC_STATIC_LIBS
    "${WEBRTC_PREBUILT_DIR}/Release/webrtc.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/webrtc_pffft.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/webrtc_rnnoise.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/aom.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/srtp.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/yuv.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/opus.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/crc32c.lib"
    "${WEBRTC_PREBUILT_DIR}/lib/Release/jsoncpp.lib")
  find_package(unofficial-libvpx CONFIG REQUIRED)
  find_library(DAV1D_RELEASE NAMES dav1d REQUIRED PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
  add_library(livekit_webrtc INTERFACE)
  add_library(WebRTC::webrtc ALIAS livekit_webrtc)
  target_include_directories(livekit_webrtc INTERFACE "${WEBRTC_ROOT}" "${WEBRTC_ROOT}/third_party/abseil-cpp" "${WEBRTC_ROOT}/third_party/libyuv/include")
  target_compile_definitions(livekit_webrtc INTERFACE WEBRTC_WIN WEBRTC_ALLOW_DEPRECATED_NAMESPACES NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(livekit_webrtc INTERFACE ${WEBRTC_STATIC_LIBS} ${WEBRTC_ABSL_LIBS} ${WEBRTC_JSON_LIBS}
    unofficial::libvpx::libvpx ${DAV1D_RELEASE} OpenSSL::SSL OpenSSL::Crypto
    ws2_32 winmm iphlpapi secur32 crypt32 bcrypt dmoguids wmcodecdspuuid amstrmid msdmo ole32 oleaut32 strmiids user32 advapi32 shell32)
else()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The pinned native manifest consumes the vcpkg target name. Supply that
    # target using the distribution's pkg-config package on Linux.
    set(unofficial-libvpx_DIR "${CMAKE_CURRENT_LIST_DIR}/unofficial-libvpx")
  endif()
  set(WEBRTC_BUILD_DEMO OFF CACHE BOOL "" FORCE)
  if(WIN32 AND VCPKG_INSTALLED_DIR)
    file(GLOB LK_NASM_TOOLS "${VCPKG_INSTALLED_DIR}/../downloads/tools/nasm/*/nasm.exe")
    foreach(tool IN LISTS LK_NASM_TOOLS)
      get_filename_component(tool_dir "${tool}" DIRECTORY)
      list(APPEND CMAKE_PROGRAM_PATH "${tool_dir}")
    endforeach()
  endif()
  if(WIN32)
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
      get_filename_component(LK_GIT_BIN "${GIT_EXECUTABLE}" DIRECTORY)
      find_program(PERL_EXECUTABLE perl HINTS "${LK_GIT_BIN}/../usr/bin")
    endif()
  endif()
  if(WIN32 AND VCPKG_INSTALLED_DIR)
    # The local checkout's generic find_library can accidentally select Debug
    # dav1d for Release. Pin the matching runtime explicitly in our build only.
    set(DAV1D_LIBRARY_RELEASE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/dav1d.lib" CACHE FILEPATH "" FORCE)
    set(DAV1D_LIBRARY_DEBUG "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib/dav1d.lib" CACHE FILEPATH "" FORCE)
  endif()
  set(WEBRTC_BUILD_VIDEO_CAPTURE OFF CACHE BOOL "" FORCE)
  add_subdirectory("${WEBRTC_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/webrtc" EXCLUDE_FROM_ALL)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # PFFFT's C99 source uses glibc math constants hidden by strict C mode.
    target_compile_definitions(webrtc_pffft PRIVATE _DEFAULT_SOURCE)
    # The native manifest exports this C feature macro to child directories.
    # AOM's NASM config defines the same name with `equ`; -D replaces its label.
    # Restrict only that inherited definition, preserving AOM's ASM/PIC options.
    function(lk_scope_aom_c_definitions directory)
      get_property(definitions DIRECTORY "${directory}" PROPERTY COMPILE_DEFINITIONS)
      list(TRANSFORM definitions REPLACE "^HAVE_UNISTD_H$"
        "$<$<NOT:$<COMPILE_LANGUAGE:ASM_NASM>>:HAVE_UNISTD_H>")
      set_property(DIRECTORY "${directory}" PROPERTY COMPILE_DEFINITIONS "${definitions}")
      get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
      foreach(child IN LISTS children)
        lk_scope_aom_c_definitions("${child}")
      endforeach()
    endfunction()
    lk_scope_aom_c_definitions("${WEBRTC_ROOT}/third_party/libaom/source/libaom")
    include(CheckCXXSourceCompiles)
    function(lk_check_dav1d_api)
      set(CMAKE_REQUIRED_INCLUDES "${DAV1D_INCLUDE_DIR}")
      unset(LK_DAV1D_HAS_THREADS_API CACHE)
      check_cxx_source_compiles("#include <dav1d/dav1d.h>\nint main() { Dav1dSettings s{}; s.n_threads = DAV1D_MAX_THREADS; return 0; }" LK_DAV1D_HAS_THREADS_API)
      if(NOT LK_DAV1D_HAS_THREADS_API)
        message(FATAL_ERROR "dav1d headers are too old for this WebRTC checkout. Run scripts/build-linux-deps.sh, then configure with -U'DAV1D_*' -DCMAKE_PREFIX_PATH=<installed prefix>.")
      endif()
    endfunction()
    lk_check_dav1d_api()
  endif()
  # Older monolithic checkouts still need their capture sources removed.
  if(NOT TARGET webrtc_api_objects)
    get_target_property(LK_WEBRTC_SOURCES webrtc SOURCES)
    list(FILTER LK_WEBRTC_SOURCES EXCLUDE REGEX "/modules/video_capture/")
    set_property(TARGET webrtc PROPERTY SOURCES "${LK_WEBRTC_SOURCES}")
  endif()
endif()
