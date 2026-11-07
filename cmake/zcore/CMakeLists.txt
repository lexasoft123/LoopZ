# One immutable zCore source checkout owns every device and DSP target.
set(ZCORE_SUBMODULE_VERIFIER "${PROJECT_SOURCE_DIR}/scripts/verify-zcore-submodule.py")
set(_loopz_zcore_arguments --root "${PROJECT_SOURCE_DIR}" --path "vendor/zcore" --print-revision)
if(TARGET zCore::device)
  if(NOT LOOPZ_SHARED_ZCORE_ROOT)
    message(FATAL_ERROR "Existing zCore targets require explicit LOOPZ_SHARED_ZCORE_ROOT")
  endif()
  file(REAL_PATH "${LOOPZ_SHARED_ZCORE_ROOT}" ZCORE_ROOT)
  foreach(_target IN ITEMS zCore::device SingZ::zdsp_runtime)
    if(NOT TARGET "${_target}")
      message(FATAL_ERROR "The shared zCore source must supply ${_target}")
    endif()
    get_target_property(_source "${_target}" SOURCE_DIR)
    if(NOT _source)
      message(FATAL_ERROR "Shared zCore must use source-built targets")
    endif()
    file(REAL_PATH "${_source}" _source)
    if(NOT _source STREQUAL ZCORE_ROOT)
      message(FATAL_ERROR "Shared ${_target} SOURCE_DIR does not match LOOPZ_SHARED_ZCORE_ROOT")
    endif()
  endforeach()
  list(APPEND _loopz_zcore_arguments --external-source "${ZCORE_ROOT}")
  set(_loopz_reuse_zcore TRUE)
else()
  if(LOOPZ_SHARED_ZCORE_ROOT)
    message(FATAL_ERROR "Add the shared zCore source before LoopZ when LOOPZ_SHARED_ZCORE_ROOT is set")
  endif()
  set(ZCORE_ROOT "${PROJECT_SOURCE_DIR}/vendor/zcore")
  set(_loopz_reuse_zcore FALSE)
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${ZCORE_SUBMODULE_VERIFIER}" ${_loopz_zcore_arguments}
  OUTPUT_VARIABLE ZCORE_VERIFIED_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_VARIABLE ZCORE_VERIFY_ERROR RESULT_VARIABLE ZCORE_VERIFY_RESULT)
if(NOT ZCORE_VERIFY_RESULT EQUAL 0)
  message(FATAL_ERROR "zCore submodule verification failed: ${ZCORE_VERIFY_ERROR}")
endif()
message(STATUS "Using pinned zCore source ${ZCORE_VERIFIED_REVISION} at ${ZCORE_ROOT}")
if(NOT _loopz_reuse_zcore)
  set(ZCORE_GRAPH_RUNNER_SOURCE "${ZCORE_ROOT}/zdsp/src/runtime/graph_runner.cpp"
    CACHE FILEPATH "zCore graph runner source" FORCE)
  set(ZCORE_BUILTIN_NODES_SOURCE "${ZCORE_ROOT}/zdsp/src/runtime/builtin_nodes.cpp"
    CACHE FILEPATH "zCore builtin nodes source" FORCE)
  set(ZCORE_BUILD_MEDIA OFF CACHE BOOL "" FORCE)
  set(ZCORE_BUILD_ANALYSIS OFF CACHE BOOL "" FORCE)
  set(ZCORE_BUILD_TESTS "${LOOPZ_BUILD_TESTS}" CACHE BOOL "" FORCE)
  set(ZCORE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${ZCORE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/zcore")
endif()
# Inspect the constructed target, not the overlay cache variables: the parent
# may reset those variables after target creation without changing its sources.
get_target_property(_loopz_dsp_source_dir SingZ::zdsp_runtime SOURCE_DIR)
get_target_property(_loopz_dsp_sources SingZ::zdsp_runtime SOURCES)
string(REPLACE ";" "\n" _loopz_dsp_source_lines "${_loopz_dsp_sources}")
set(_loopz_dsp_configure_manifest "${CMAKE_CURRENT_BINARY_DIR}/loopz-dsp-configure-sources.txt")
file(WRITE "${_loopz_dsp_configure_manifest}" "${_loopz_dsp_source_dir}\n${_loopz_dsp_source_lines}\n")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${ZCORE_SUBMODULE_VERIFIER}"
  ${_loopz_zcore_arguments} --dsp-source-manifest "${_loopz_dsp_configure_manifest}"
  ERROR_VARIABLE _loopz_dsp_error RESULT_VARIABLE _loopz_dsp_result OUTPUT_QUIET)
if(NOT _loopz_dsp_result EQUAL 0)
  message(FATAL_ERROR "zCore actual DSP source verification failed: ${_loopz_dsp_error}")
endif()
# Generate from final target properties after the parent finishes configuring.
# The build guard therefore also catches target_sources()/set_property() calls
# made after add_subdirectory(LoopZ), including configuration-specific sources.
set(_loopz_dsp_final_manifest "${CMAKE_CURRENT_BINARY_DIR}/loopz-dsp-final-sources-$<CONFIG>.txt")
file(GENERATE OUTPUT "${_loopz_dsp_final_manifest}"
  CONTENT "$<TARGET_PROPERTY:SingZ::zdsp_runtime,SOURCE_DIR>\n$<JOIN:$<TARGET_GENEX_EVAL:SingZ::zdsp_runtime,$<TARGET_PROPERTY:SingZ::zdsp_runtime,SOURCES>>,\n>\n")
list(APPEND _loopz_zcore_arguments --dsp-source-manifest "${_loopz_dsp_final_manifest}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/.gitmodules")
add_custom_target(loopz_verify_zcore ALL
  COMMAND "${Python3_EXECUTABLE}" "${ZCORE_SUBMODULE_VERIFIER}" ${_loopz_zcore_arguments}
  VERBATIM COMMENT "Verifying pinned zCore source")
get_property(_zcore_targets DIRECTORY "${ZCORE_ROOT}" PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _zcore_targets)
  get_target_property(_kind "${_target}" TYPE)
  if(NOT _kind STREQUAL "INTERFACE_LIBRARY")
    add_dependencies("${_target}" loopz_verify_zcore)
  endif()
endforeach()
