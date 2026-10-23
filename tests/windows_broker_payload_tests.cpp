#include "broker_payload.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    using namespace loopz::windows;
    if (argc == 1) {
        bool rejected{};
        try {
            embeddedBrokerPayload();
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("Missing resources were accepted");
        std::cout << "PASS: missing embedded payload rejected\n";
        return 0;
    }
    if (argc != 2)
        throw std::runtime_error("Expected independently computed broker digest");
    const auto payload = embeddedBrokerPayload();
    if (payload.sha256 != argv[1] || payload.bytes.size() < 2 ||
        payload.bytes[0] != std::byte{'M'} || payload.bytes[1] != std::byte{'Z'})
        throw std::runtime_error("Embedded broker differs from the built service");
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize independent payload hash");
    struct Algorithm {
        BCRYPT_ALG_HANDLE value;
        ~Algorithm() {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    } close{algorithm};
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(algorithm, nullptr, 0,
                   reinterpret_cast<PUCHAR>(const_cast<std::byte*>(payload.bytes.data())),
                   static_cast<ULONG>(payload.bytes.size()), hash.data(),
                   static_cast<ULONG>(hash.size())) < 0)
        throw std::runtime_error("Cannot hash embedded broker");
    std::string digest;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : hash) {
        digest += hex[byte >> 4];
        digest += hex[byte & 15];
    }
    if (digest != payload.sha256)
        throw std::runtime_error("Embedded payload checksum mismatch");
    std::cout
        << "PASS: current-EXE resource bytes and digest match the independently hashed service\n";
}
