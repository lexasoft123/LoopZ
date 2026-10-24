#pragma once
#include <loopz/macos/configuration_controller.hpp>

namespace loopz::macos {
inline bool installationValidationCanStop(bool requested, const RestorationStatus& state) noexcept {
    return requested && state.phase == RestorationPhase::Restored && !state.error &&
           state.desiredGeneration == state.activeGeneration;
}
} // namespace loopz::macos
