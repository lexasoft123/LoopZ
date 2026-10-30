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
if(LOOPZ_BUILD_BENCHMARKS)
  add_executable(loopz-routing-stress-benchmark ../tools/routing_benchmark.cpp)
  target_link_libraries(loopz-routing-stress-benchmark PRIVATE LoopZ::Core)
  loopz_strict_target(loopz-routing-stress-benchmark)
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
