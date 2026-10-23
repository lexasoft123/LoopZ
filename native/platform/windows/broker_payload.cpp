#include "broker_payload.hpp"
#include <windows.h>
#include <stdexcept>

namespace loopz::windows {
namespace {
std::span<const std::byte> resource(WORD id) {
    const auto module = GetModuleHandleW(nullptr);
    const auto entry = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!entry)
        throw std::runtime_error("Maintenance package is missing its embedded setup service");
    const auto count = SizeofResource(module, entry);
    const auto loaded = LoadResource(module, entry);
    const auto* data = static_cast<const std::byte*>(LockResource(loaded));
    if (!data || !count)
        throw std::runtime_error("Cannot read embedded setup service package");
    return {data, count};
}
} // namespace
EmbeddedBrokerPayload embeddedBrokerPayload() {
    const auto bytes = resource(51001);
    if (bytes.size() > 64 * 1024 * 1024)
        throw std::runtime_error("Embedded setup service exceeds the package limit");
    const auto metadata = resource(51002);
    if (metadata.size() != 64)
        throw std::runtime_error("Invalid embedded setup service digest");
    std::string digest(reinterpret_cast<const char*>(metadata.data()), metadata.size());
    if (digest.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid embedded setup service digest");
    return {bytes, std::move(digest)};
}
} // namespace loopz::windows
