# Narrow LoopZ composition. Upstream source bytes remain unchanged and hashed.
set(SINGZ_ROOT "${PROJECT_SOURCE_DIR}/vendor/singz")
include("${SINGZ_ROOT}/cmake/SingZRealtimeTarget.cmake")
add_library(singz_native_build_options INTERFACE)
add_library(SingZ::native_build_options ALIAS singz_native_build_options)
add_library(zcore_base INTERFACE)
add_library(SingZ::zcore_base ALIAS zcore_base)
target_compile_features(zcore_base INTERFACE cxx_std_20)
target_include_directories(zcore_base INTERFACE "${SINGZ_ROOT}/zcore/include")
set(ZCORE_RT_SOURCES
  "${SINGZ_ROOT}/zcore/src/audio/audio_input_convert.cpp"
  "${SINGZ_ROOT}/zcore/src/audio/audio_input_timestamp.cpp"
  "${SINGZ_ROOT}/zcore/src/audio/audio_input_transport_callback.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input_callback.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input_callback_gate.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_callback.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_fifo_hot.cpp"
  "${SINGZ_ROOT}/zcore/src/audio/audio_input_transport_internal.h"
  "${SINGZ_ROOT}/zcore/src/device/audio_input_callback.h"
  "${SINGZ_ROOT}/zcore/include/zcore/audio/audio_input_convert.h"
  "${SINGZ_ROOT}/zcore/include/zcore/audio/audio_input_producer.h"
  "${SINGZ_ROOT}/zcore/include/zcore/audio/audio_input_timestamp.h"
  "${SINGZ_ROOT}/zcore/include/zcore/device/audio_input_callback_gate.h"
  "${SINGZ_ROOT}/zcore/include/zcore/device/audio_host_callback.h"
  "${SINGZ_ROOT}/zcore/include/zcore/device/audio_host_render.h"
)
add_library(zcore_device_callback STATIC ${ZCORE_RT_SOURCES})
target_link_libraries(zcore_device_callback PUBLIC zcore_base)
singz_configure_realtime_target(zcore_device_callback)
set_target_properties(zcore_device_callback PROPERTIES POSITION_INDEPENDENT_CODE YES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
add_library(zcore_device STATIC
  "${SINGZ_ROOT}/zcore/src/audio/audio_input_transport.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input_wake.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_fifo.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_unsupported.cpp"
  "${SINGZ_ROOT}/zcore/platform/macos/audio_host_macos.cpp"
  "${SINGZ_ROOT}/zcore/platform/macos/audio_input_macos.cpp"
  "${SINGZ_ROOT}/zcore/platform/macos/audio_input_wake_macos.cpp"
  "${SINGZ_ROOT}/zcore/platform/ios/audio_input_ios_session.cpp")
add_library(SingZ::zcore_device ALIAS zcore_device)
target_link_libraries(zcore_device PUBLIC zcore_device_callback Threads::Threads
  "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
target_compile_options(zcore_device PRIVATE -fblocks)
add_library(zdsp_api INTERFACE)
add_library(SingZ::zdsp_api ALIAS zdsp_api)
target_include_directories(zdsp_api INTERFACE "${SINGZ_ROOT}/zdsp/include")
target_link_libraries(zdsp_api INTERFACE zcore_base)
set(ZDSP_SOURCES src/api/contracts.cpp src/runtime/realtime_arena.cpp
  src/runtime/builtin_nodes.cpp src/runtime/decoded_buffer_source.cpp
  src/runtime/streaming_window_source.cpp src/runtime/scheduled_cue_source.cpp
  src/runtime/scheduled_gain.cpp src/runtime/graph_compiler.cpp src/runtime/graph_runner.cpp)
list(TRANSFORM ZDSP_SOURCES PREPEND "${SINGZ_ROOT}/zdsp/")
add_library(zdsp_runtime STATIC ${ZDSP_SOURCES})
add_library(SingZ::zdsp_runtime ALIAS zdsp_runtime)
target_link_libraries(zdsp_runtime PUBLIC zdsp_api)
singz_configure_realtime_target(zdsp_runtime)
set_target_properties(zcore_device zdsp_runtime PROPERTIES POSITION_INDEPENDENT_CODE YES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
# Preserve upstream RT source checking for the exact compiled runtime and headers.
file(GLOB ZDSP_HEADERS "${SINGZ_ROOT}/zdsp/include/zdsp/*.h" "${SINGZ_ROOT}/zdsp/src/runtime/*.h")
set(MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/realtime.cmake")
file(WRITE "${MANIFEST}" "set(SINGZ_ZDSP_RT_FILES\n")
foreach(SOURCE IN LISTS ZDSP_SOURCES ZDSP_HEADERS)
  file(APPEND "${MANIFEST}" " [==[${SOURCE}]==]\n")
endforeach()
file(APPEND "${MANIFEST}" ")\n")
add_custom_target(loopz-check-zdsp ALL COMMAND "${CMAKE_COMMAND}"
  "-DSINGZ_ZDSP_RT_MANIFEST=${MANIFEST}" "-DSINGZ_ZDSP_RT_PROFILE=monitor"
  -P "${SINGZ_ROOT}/cmake/CheckZdspRealtime.cmake")
add_dependencies(zdsp_runtime loopz-check-zdsp)

set(ZCORE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/zcore-realtime.cmake")
file(WRITE "${ZCORE_MANIFEST}" "set(SINGZ_ZCORE_RT_FILES\n")
foreach(SOURCE IN LISTS ZCORE_RT_SOURCES)
  file(APPEND "${ZCORE_MANIFEST}" " [==[${SOURCE}]==]\n")
endforeach()
file(APPEND "${ZCORE_MANIFEST}" ")\n")
add_custom_target(loopz-check-zcore ALL COMMAND "${CMAKE_COMMAND}"
  "-DSINGZ_ZCORE_RT_MANIFEST=${ZCORE_MANIFEST}"
  -P "${SINGZ_ROOT}/cmake/CheckZcoreRealtime.cmake"
  COMMAND "${CMAKE_COMMAND}" "-DSINGZ_ZCORE_ROOT=${SINGZ_ROOT}/zcore"
  -P "${SINGZ_ROOT}/cmake/CheckZcoreBoundaries.cmake")
add_dependencies(zcore_device_callback loopz-check-zcore)
