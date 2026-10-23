#pragma once
#include "app_routing_store.hpp"
#include <functional>
namespace loopz::windows {
enum class AppRoutingStorageIntent { RecoverOnly, InitializeForRequest };
// Serialized control owner only. An existing journal is always opened without
// initialization. New history requires an explicit saved request, a wholly new
// storage directory and proven absence of this account's internal PnP root.
// RootProbe is a native dependency-injection seam for isolated tests, never an
// editor/request field. Production uses the real account-root observation.
std::unique_ptr<AppRoutingStore> openAppRoutingStore(const std::filesystem::path&,
                                                     AppRoutingStorageIntent,
                                                     std::function<bool()> rootProbe = {});
} // namespace loopz::windows
