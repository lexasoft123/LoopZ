#pragma once

namespace loopz::macos {
// Input-only, virtual-device-only signal observation. Uses the Engine's
// existing permission status and never requests a new privacy grant.
int virtualInputCheck(int argc, char** argv);
} // namespace loopz::macos
