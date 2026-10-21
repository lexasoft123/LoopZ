#pragma once

namespace loopz::macos {
// Explicit diagnostics for verified LoopZ virtual endpoints only. This command
// never requests permission, opens physical IO or changes a shared device rate.
int routingIOCheck(int argc, char** argv);
} // namespace loopz::macos
