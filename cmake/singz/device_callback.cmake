# Shared callback leaves, compiled with the upstream realtime policy.
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
