#include "driver_identity.hpp"
#include <iostream>
#include <stdexcept>
template <class F> void rejects(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("Invalid device identity input accepted");
}
int main() try {
    using namespace loopz::windows;
    const auto owner = currentOwnerSid();
    const auto key = driverMixKey(owner, "mix-stable-id");
    if (key.size() != 64 || key != driverMixKey(owner, "mix-stable-id") ||
        key == driverMixKey(owner, "different-mix") ||
        key == driverMixKey(L"S-1-5-21-1-2-3-999", "mix-stable-id") ||
        driverInstance(key) != driverInstance(driverMixKey(owner, "mix-stable-id")))
        throw std::runtime_error("Mix endpoint identity is unstable or not owner isolated");
    const auto sink = driverAppRoutingSinkKey(owner);
    if (sink.size() != 64 || sink != driverAppRoutingSinkKey(owner) ||
        sink == driverAppRoutingSinkKey(L"S-1-5-21-1-2-3-999") || sink == key ||
        sink == driverMixKey(owner, "LoopZ.AppRoutingSink.v1") ||
        sink == driverMixKey(owner, "app-routing-sink"))
        throw std::runtime_error("Routing sink identity is not separate and owner isolated");
    if (driverMixKey(L"S-1-5-21-1-2-3-999", "mix-stable-id") !=
            "c72255c13709c100696d5215bcc1b5a249dc6d3bc05ca4dd3f7a9caea37ec7bf" ||
        driverAppRoutingSinkKey(L"S-1-5-21-1-2-3-999") !=
            "7dfeba57e5754a7708229a8b778e4f8c1e9f0d83fa487a940f716417eb72b19e")
        throw std::runtime_error("Legacy or sink hash differs from independent SHA-256 vectors");
    rejects([&] {
        driverMixKey(owner, "");
    });
    rejects([&] {
        driverMixKey(owner, std::string("\0LoopZ.AppRoutingSink.v1", 23));
    });
    rejects([&] {
        driverMixKey(owner, std::string(4097, 'a'));
    });
    rejects([&] {
        driverMixKey(L"", "mix");
    });
    rejects([&] {
        driverAppRoutingSinkKey(std::wstring(L"S-1-5-18\0suffix", 16));
    });
    rejects([&] {
        driverAppRoutingSinkKey(std::wstring(4097, L'a'));
    });
    bool rejected = false;
    try {
        (void)driverInstance("..\\unrelated");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("Invalid device identity accepted");
    std::cout << "Stable legacy mix and domain-separated routing sink identities passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}
