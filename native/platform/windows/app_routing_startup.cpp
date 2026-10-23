#include "app_routing_startup.hpp"
#include "app_routing_sink_lifetime.hpp"
#include "driver_identity.hpp"
#include <stdexcept>
namespace loopz::windows {
std::unique_ptr<AppRoutingStore> openAppRoutingStore(const std::filesystem::path& root,
                                                     AppRoutingStorageIntent intent,
                                                     std::function<bool()> rootProbe) {
    if (intent != AppRoutingStorageIntent::RecoverOnly &&
        intent != AppRoutingStorageIntent::InitializeForRequest)
        throw std::invalid_argument("Invalid app routing storage intent");
    if (std::filesystem::exists(root / L"routing.sqlite3"))
        return std::make_unique<AppRoutingStore>(root, false);
    if (intent == AppRoutingStorageIntent::RecoverOnly)
        return {};
    // An existing empty folder can be lost history, not evidence of first use.
    if (std::filesystem::exists(root))
        throw std::runtime_error("App routing recovery history is missing from an existing folder; "
                                 "preserve it before repairing setup");
    const bool rootPresent = rootProbe ? rootProbe() : appRoutingSinkRootPresent(currentOwnerSid());
    if (rootPresent)
        throw std::runtime_error("The internal app endpoint exists without recovery history; "
                                 "preserve it before repairing setup");
    return std::make_unique<AppRoutingStore>(root, true);
}
} // namespace loopz::windows
