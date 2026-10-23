#pragma once
namespace loopz::windows {
// Control-thread preparation before opening audio. Returns true only when the
// adapter has verified CPU/OS availability and installed the optimized kernel.
bool prepareAudioKernels() noexcept;
} // namespace loopz::windows
