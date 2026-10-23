# Reproducible LoopZ overlay for the pinned SingZ graph runner.
# The archive stays byte-for-byte intact. Both the input and the generated
# translation unit are pinned; an SDK update must explicitly review this overlay.
function(loopz_singz_containment_source result)
  set(input "${SINGZ_ROOT}/zdsp/src/runtime/graph_runner.cpp")
  file(SHA256 "${input}" input_sha)
  if(NOT input_sha STREQUAL "97d90afdc0dd733f7e187cdbb6d3f049cbcab60873f0a5e0756793f06a315e16")
    message(FATAL_ERROR "SingZ graph containment overlay does not match this SDK")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
  file(READ "${input}" source)

  string(REPLACE "#include <cmath>" [==[#include <cmath>
#include <cstdint>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif]==] source "${source}")

  string(REPLACE [==[    ++*invalidSamples;
  return 0.0f;
}

]==] [==[    ++*invalidSamples;
  return 0.0f;
}

// Contain one plane with a local non-finite reduction. The number of bad
// samples in a plane cannot exceed its uint32 frame count; fold it into the
// graph's saturating total only after processing the complete plane.
void copyContained(const float* source, float* destination, uint32_t frames,
                   uint32_t* invalidSamples) noexcept {
  uint32_t invalid = 0;
  uint32_t frame = 0;
#if defined(__SSE2__) || defined(_M_X64)
  // Partial overlap retains the upstream forward-copy semantics. Ordinary
  // graph buffers are disjoint or exactly in-place; both can use full vectors.
  const auto from = reinterpret_cast<uintptr_t>(source);
  const auto to = reinterpret_cast<uintptr_t>(destination);
  const auto distance = from < to ? to - from : from - to;
  const bool partialOverlap = from != to && distance / sizeof(float) < frames;
  if (!partialOverlap) {
    const auto magnitude = _mm_set1_epi32(0x7fffffff);
    const auto infinity = _mm_set1_epi32(0x7f800000);
    for (; frames - frame >= 4; frame += 4) {
      const auto raw = _mm_loadu_ps(source + frame);
      const auto finite = _mm_cmpgt_epi32(
          infinity, _mm_and_si128(_mm_castps_si128(raw), magnitude));
      _mm_storeu_ps(destination + frame, _mm_and_ps(raw, _mm_castsi128_ps(finite)));
      const auto mask = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(finite)));
      invalid += 4U - ((mask & 1U) + ((mask >> 1) & 1U) +
                      ((mask >> 2) & 1U) + ((mask >> 3) & 1U));
    }
  }
#endif
  for (; frame < frames; ++frame) {
    const float value = source[frame];
    const bool finite = std::isfinite(value);
    destination[frame] = finite ? value : 0.0f;
    invalid += static_cast<uint32_t>(!finite);
  }
  if (invalid != 0 && invalidSamples != nullptr) {
    const uint32_t remaining = UINT32_MAX - *invalidSamples;
    *invalidSamples += invalid < remaining ? invalid : remaining;
  }
}

]==] source "${source}")

  string(REPLACE [==[    destination.capture = inputs[bus].capture;
    for (uint32_t channel = 0; channel < destination.channelCount; ++channel)
      for (uint32_t frame = 0; frame < context.frames.value; ++frame)
        destination.channels[channel][frame] =
            contained(inputs[bus].channels[channel][frame], &invalidSamples);
  }
  for (uint32_t index = 0; index < graph->nodeCount; ++index) {
]==] [==[    destination.capture = inputs[bus].capture;
    for (uint32_t channel = 0; channel < destination.channelCount; ++channel)
      copyContained(inputs[bus].channels[channel], destination.channels[channel],
                    context.frames.value, &invalidSamples);
  }
  for (uint32_t index = 0; index < graph->nodeCount; ++index) {
]==] source "${source}")

  string(REPLACE [==[        RuntimeBuffer& destination = graph->buffers[node.outputs[0].buffer];
        for (uint32_t channel = 0; channel < node.outputs[0].channels; ++channel)
          for (uint32_t frame = 0; frame < context.frames.value; ++frame)
            destination.channels[channel][frame] = contained(
                source.channels[channel][frame], &invalidSamples);
      }
      continue;
]==] [==[        RuntimeBuffer& destination = graph->buffers[node.outputs[0].buffer];
        for (uint32_t channel = 0; channel < node.outputs[0].channels; ++channel)
          copyContained(source.channels[channel], destination.channels[channel],
                        context.frames.value, &invalidSamples);
      }
      continue;
]==] source "${source}")

  string(REPLACE [==[    for (uint32_t bus = 0; bus < node.outputCount; ++bus) {
      for (uint32_t channel = 0; channel < outputViews[bus].channelCount; ++channel)
        for (uint32_t frame = 0; frame < context.frames.value; ++frame)
          outputViews[bus].channels[channel][frame] = contained(
              outputViews[bus].channels[channel][frame], &invalidSamples);
    }
  }
]==] [==[    for (uint32_t bus = 0; bus < node.outputCount; ++bus) {
      for (uint32_t channel = 0; channel < outputViews[bus].channelCount; ++channel)
        copyContained(outputViews[bus].channels[channel],
                      outputViews[bus].channels[channel], context.frames.value,
                      &invalidSamples);
    }
  }
]==] source "${source}")

  string(REPLACE [==[    RuntimeBuffer& source = graph->buffers[graph->outputs[bus].buffer];
    for (uint32_t channel = 0; channel < source.channelCount; ++channel)
      for (uint32_t frame = 0; frame < context.frames.value; ++frame)
        outputs[bus].channels[channel][frame] =
            contained(source.channels[channel][frame], &invalidSamples);
  }
  if (invalidSamples != 0 && diagnostics != nullptr)
]==] [==[    RuntimeBuffer& source = graph->buffers[graph->outputs[bus].buffer];
    for (uint32_t channel = 0; channel < source.channelCount; ++channel)
      copyContained(source.channels[channel], outputs[bus].channels[channel],
                    context.frames.value, &invalidSamples);
  }
  if (invalidSamples != 0 && diagnostics != nullptr)
]==] source "${source}")

  string(SHA256 output_sha "${source}")
  if(NOT output_sha STREQUAL "91a1ca4bcc7c8fbaeac5cec11bcc5a65675bdeab12974fcd790c5ee93f55cfae")
    message(FATAL_ERROR "SingZ graph containment overlay produced unexpected source")
  endif()
  set(output "${CMAKE_CURRENT_BINARY_DIR}/contained-runtime/graph_runner.cpp")
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
