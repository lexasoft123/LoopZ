#pragma once

namespace loopz::macos {
// Opt-in output-only fixture. Both endpoints must be verified LoopZ virtual
// devices. A bounded stdin protocol controls the second output within one PID.
int multiOutputSource(int argc, char** argv);
} // namespace loopz::macos
