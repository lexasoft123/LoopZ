#pragma once
#include <windows.h>
#include <loopz/device_intent_database.hpp>
#include <string>
#include <string_view>

namespace loopz::windows {
// Control-thread only. Fixed local pipe, verified installed SCM server, bounded
// request/reply and cancellation. Throws on failure; never retries a sent request.
std::string callDeviceBroker(std::string_view request, HANDLE cancel);
// Reads a consistent owner-scoped desired-state snapshot. Restarts boundedly if
// edits or tombstone pruning invalidate pagination; never infers deletion intent.
DeviceIntentSnapshot readDeviceBrokerSnapshot(HANDLE cancel);
} // namespace loopz::windows
