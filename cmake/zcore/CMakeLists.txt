# zCore is an immutable, independently versioned source dependency.
# Product capture/routing policy stays in LoopZ.
set(ZCORE_ROOT "${PROJECT_SOURCE_DIR}/vendor/zcore")
set(ZCORE_SUBMODULE_VERIFIER "${PROJECT_SOURCE_DIR}/scripts/verify-zcore-submodule.py")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${ZCORE_SUBMODULE_VERIFIER}" --root "${PROJECT_SOURCE_DIR}"
  --path "vendor/zcore" --print-revision
  OUTPUT_VARIABLE ZCORE_VERIFIED_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_VARIABLE ZCORE_VERIFY_ERROR
  RESULT_VARIABLE ZCORE_VERIFY_RESULT)
if(NOT ZCORE_VERIFY_RESULT EQUAL 0)
  message(FATAL_ERROR "zCore submodule verification failed: ${ZCORE_VERIFY_ERROR}")
endif()
message(STATUS "Using zCore submodule ${ZCORE_VERIFIED_REVISION}")

# zCore owns these optimized sources. Force canonical paths so any transitional
# source override in a reused build cache cannot select a local overlay.
set(ZCORE_GRAPH_RUNNER_SOURCE "${ZCORE_ROOT}/zdsp/src/runtime/graph_runner.cpp"
  CACHE FILEPATH "zCore graph runner source" FORCE)
set(ZCORE_BUILTIN_NODES_SOURCE "${ZCORE_ROOT}/zdsp/src/runtime/builtin_nodes.cpp"
  CACHE FILEPATH "zCore builtin nodes source" FORCE)
set(ZCORE_BUILD_MEDIA OFF CACHE BOOL "" FORCE)
set(ZCORE_BUILD_ANALYSIS OFF CACHE BOOL "" FORCE)
set(ZCORE_BUILD_TESTS "${BUILD_TESTING}" CACHE BOOL "" FORCE)
set(ZCORE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory("${ZCORE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/zcore")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/.gitmodules")

add_custom_target(loopz_verify_zcore ALL
  COMMAND "${Python3_EXECUTABLE}" "${ZCORE_SUBMODULE_VERIFIER}"
    --root "${PROJECT_SOURCE_DIR}" --path "vendor/zcore" --print-revision
  VERBATIM COMMENT "Verifying pinned zCore submodule")
get_property(_zcore_targets DIRECTORY "${ZCORE_ROOT}" PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _zcore_targets)
  get_target_property(_kind "${_target}" TYPE)
  if(NOT _kind STREQUAL "INTERFACE_LIBRARY")
    add_dependencies("${_target}" loopz_verify_zcore)
  endif()
endforeach()
