add_library(loopz_core STATIC
  application_processes.cpp
  graph.cpp
  capture_buffer.cpp
  audio_delivery_timing.cpp
  capture_mix.cpp
  routing.cpp
  route_document.cpp
  route_edits.cpp
  edit_history.cpp
  recovery.cpp
  audio_scene.cpp
  input_timing.cpp
  application_timing.cpp
  audio_history.cpp
  sample_clock.cpp
  output_clock.cpp
  audio_resampler.cpp
  cached_fir_kernels.cpp
  causal_resampler.cpp
  sinc_kernel.cpp
  minimum_phase_kernel.cpp
  source_registry.cpp
  audio_meter.cpp
  monitor.cpp
  direct_monitor.cpp
  render_schedule.cpp)
add_library(LoopZ::Core ALIAS loopz_core)
target_include_directories(loopz_core PUBLIC "${PROJECT_SOURCE_DIR}/native/include")
target_link_libraries(loopz_core PUBLIC SingZ::zdsp_runtime)
loopz_strict_target(loopz_core)
add_library(loopz_document_json STATIC document_json.cpp)
target_include_directories(loopz_document_json SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/vendor")
target_link_libraries(loopz_document_json PUBLIC LoopZ::Core)
loopz_strict_target(loopz_document_json)
# The broker request contract is portable so its untrusted-input validation is
# exercised under sanitizers without installing a Windows service or driver.
add_library(loopz_device_intent STATIC device_intent.cpp device_intent_plan.cpp)
target_link_libraries(loopz_device_intent PUBLIC loopz_document_json)
loopz_strict_target(loopz_device_intent)
add_library(loopz_sqlite STATIC EXCLUDE_FROM_ALL "${PROJECT_SOURCE_DIR}/vendor/sqlite/sqlite3.c")
target_include_directories(loopz_sqlite SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/vendor/sqlite")
target_compile_definitions(loopz_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0)
if(UNIX)
  target_link_libraries(loopz_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
add_library(loopz_device_database STATIC EXCLUDE_FROM_ALL device_intent_database.cpp)
target_link_libraries(loopz_device_database PUBLIC loopz_device_intent loopz_sqlite)
loopz_strict_target(loopz_device_database)
add_library(loopz_app_routing_journal STATIC EXCLUDE_FROM_ALL app_routing_journal.cpp app_routing_lease.cpp)
target_link_libraries(loopz_app_routing_journal PUBLIC LoopZ::Core loopz_sqlite)
loopz_strict_target(loopz_app_routing_journal)

if(LOOPZ_BUILD_BENCHMARKS)
  add_executable(loopz-routing-benchmark ../tools/routing_benchmark.cpp)
  target_link_libraries(loopz-routing-benchmark PRIVATE LoopZ::Core)
  loopz_strict_target(loopz-routing-benchmark)
  add_executable(loopz-input-history-benchmark ../tools/input_history_benchmark.cpp)
  target_link_libraries(loopz-input-history-benchmark PRIVATE LoopZ::Core)
  loopz_strict_target(loopz-input-history-benchmark)
  add_executable(loopz-causal-output-benchmark ../tools/causal_output_benchmark.cpp)
  target_link_libraries(loopz-causal-output-benchmark PRIVATE LoopZ::Core)
  loopz_strict_target(loopz-causal-output-benchmark)
  add_executable(loopz-audio-scene-benchmark ../tools/audio_scene_benchmark.cpp)
  target_link_libraries(loopz-audio-scene-benchmark PRIVATE LoopZ::Core)
  loopz_strict_target(loopz-audio-scene-benchmark)
endif()
