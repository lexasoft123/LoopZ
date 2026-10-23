# Pinned, reproducible steady-state gain/limiter traversal overlay.
# Preserve the vendored archive and all automation/containment semantics.
function(loopz_singz_planar_source result)
  set(input "${SINGZ_ROOT}/zdsp/src/runtime/builtin_nodes.cpp")
  file(SHA256 "${input}" input_sha)
  if(NOT input_sha STREQUAL "49ad35f85c808d6834949fe20c1694d0bc5db38a555778984d4f18e1792139d0")
    message(FATAL_ERROR "SingZ planar overlay does not match this SDK")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
  file(READ "${input}" source)
  string(REPLACE "#include <bit>" "#include <bit>\n#include <limits>" source "${source}")
  string(REPLACE [==[float finiteSample(float value, BuiltinState* state) noexcept {
  if (std::isfinite(value)) return value;]==] [==[float finiteSample(float value, BuiltinState* state) noexcept {
  // Classify the IEEE encoding without a per-sample CRT call on Windows.
  // Every finite encoding, including signed zero and subnormals, stays intact.
  static_assert(sizeof(float) == sizeof(uint32_t) &&
                std::numeric_limits<float>::is_iec559);
  if ((std::bit_cast<uint32_t>(value) & 0x7f800000U) != 0x7f800000U) return value;]==]
    source "${source}")
  string(REPLACE [==[void processGain(BuiltinState* state, const ProcessContext* context,
                 const ConstAudioBusView* inputs,
                 const MutableAudioBusView& output) noexcept {
  uint32_t eventIndex = 0;]==] [==[void processGain(BuiltinState* state, const ProcessContext* context,
                 const ConstAudioBusView* inputs,
                 const MutableAudioBusView& output) noexcept {
  // With no pending automation, traverse contiguous planes. Automated and
  // in-flight ramps retain the upstream sample-accurate implementation below.
  if (context->parameterCount == 0 && state->rampRemaining == 0) {
    const float value = state->currentValue;
    for (uint32_t channel = 0; channel < output.channelCount; ++channel) {
      const float* source = inputs[0].channels[channel];
      float* destination = output.channels[channel];
      for (uint32_t frame = 0; frame < context->frames.value; ++frame)
        destination[frame] = finiteSample(source[frame], state) * value;
    }
    return;
  }
  uint32_t eventIndex = 0;]==] source "${source}")
  string(REPLACE [==[void processLimiter(BuiltinState* state, const ProcessContext* context,
                    const ConstAudioBusView* inputs,
                    const MutableAudioBusView& output) noexcept {
  uint32_t eventIndex = 0;]==] [==[void processLimiter(BuiltinState* state, const ProcessContext* context,
                    const ConstAudioBusView* inputs,
                    const MutableAudioBusView& output) noexcept {
  // With no pending automation, traverse contiguous planes. Automated and
  // in-flight ramps retain the upstream sample-accurate implementation below.
  if (context->parameterCount == 0 && state->rampRemaining == 0) {
    const float value = state->currentValue;
    for (uint32_t channel = 0; channel < output.channelCount; ++channel) {
      const float* source = inputs[0].channels[channel];
      float* destination = output.channels[channel];
      for (uint32_t frame = 0; frame < output.frames.value; ++frame) {
        float sample = finiteSample(source[frame], state);
        if (sample > value) sample = value;
        if (sample < -value) sample = -value;
        destination[frame] = sample;
      }
    }
    return;
  }
  uint32_t eventIndex = 0;]==] source "${source}")
  string(SHA256 output_sha "${source}")
  if(NOT output_sha STREQUAL "b32d465b277ad4544856373510456143f5f9e5d1e39cd0b6573a06b28b5ab7ea")
    message(FATAL_ERROR "SingZ planar overlay produced unexpected source")
  endif()
  set(output "${CMAKE_CURRENT_BINARY_DIR}/contained-runtime/builtin_nodes.cpp")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/contained-runtime")
  set(existing "")
  if(EXISTS "${output}")
    file(READ "${output}" existing)
  endif()
  if(NOT existing STREQUAL source)
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY NEWLINE_STYLE UNIX)
  endif()
  set(${result} "${output}" PARENT_SCOPE)
endfunction()
