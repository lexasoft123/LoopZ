# Narrow LoopZ composition. Upstream source bytes remain unchanged and hashed.
set(SINGZ_ROOT "${PROJECT_SOURCE_DIR}/vendor/singz")
include("${SINGZ_ROOT}/cmake/SingZRealtimeTarget.cmake")
add_library(singz_native_build_options INTERFACE)
add_library(SingZ::native_build_options ALIAS singz_native_build_options)
add_library(zcore_base INTERFACE)
add_library(SingZ::zcore_base ALIAS zcore_base)
target_compile_features(zcore_base INTERFACE cxx_std_20)
target_include_directories(zcore_base INTERFACE "${SINGZ_ROOT}/zcore/include")
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
set_target_properties(zdsp_runtime PROPERTIES POSITION_INDEPENDENT_CODE YES
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

if(LOOPZ_BUILD_MACOS_BACKEND)
  include("${CMAKE_CURRENT_LIST_DIR}/macos.cmake")
endif()
