#pragma once
#include <string>
#include <string_view>
namespace loopz::windows {
// Hash both owner and durable mix ID: different Windows accounts never share a bus.
std::wstring currentOwnerSid();
std::string driverMixKey(std::wstring_view ownerSid, std::string_view mixId);
// Separate hash domain: no valid mix ID can alias the account's internal sink.
std::string driverAppRoutingSinkKey(std::wstring_view ownerSid);
std::wstring driverInstance(std::string_view key);
} // namespace loopz::windows
