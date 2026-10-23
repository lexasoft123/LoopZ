// LoopZ contributors. MIT license. Integer-only, little-endian audio boundaries.
#pragma once
#include "sample_bits.hpp"

namespace loopz_driver {
inline unsigned readSampleBits(const unsigned char* source, unsigned bytes) noexcept {
    unsigned bits = 0;
    for (unsigned i = 0; i < bytes; ++i)
        bits |= static_cast<unsigned>(source[i]) << (i * 8);
    return bits;
}
inline void writeSampleBits(unsigned char* destination, unsigned bytes, unsigned bits) noexcept {
    for (unsigned i = 0; i < bytes; ++i)
        destination[i] = static_cast<unsigned char>(bits >> (i * 8));
}
// Exact in-place conversion is supported; partially overlapping buffers are not.
// Validate before touching either buffer. Sizes are bytes, count is samples.
// Callers own serialization and nonpaged storage. No allocation or FP operations.
inline bool convertPackedSamples(const unsigned char* source, unsigned sourceBytes,
                                 unsigned sourceSize, unsigned char* destination,
                                 unsigned destinationBytes, unsigned destinationSize,
                                 unsigned count) noexcept {
    if ((sourceBytes != 2 && sourceBytes != 4) ||
        (destinationBytes != 2 && destinationBytes != 4) || count > sourceSize / sourceBytes ||
        count > destinationSize / destinationBytes || (count && (!source || !destination)))
        return false;
    // The float transport already uses the public float encoding. Keep the
    // trust-boundary finite check, but make its width explicit so a hardened
    // kernel build does not decode/store every sample through byte-count loops.
    // Byte expressions allow unaligned buffers without aliasing or FP operations.
    if (sourceBytes == 4 && destinationBytes == 4) {
        for (unsigned i = 0; i < count; ++i) {
            const auto* input = source + i * 4;
            const unsigned bits =
                static_cast<unsigned>(input[0]) | (static_cast<unsigned>(input[1]) << 8) |
                (static_cast<unsigned>(input[2]) << 16) | (static_cast<unsigned>(input[3]) << 24);
            const auto finite = finiteFloatBits(bits);
            auto* output = destination + i * 4;
            output[0] = static_cast<unsigned char>(finite);
            output[1] = static_cast<unsigned char>(finite >> 8);
            output[2] = static_cast<unsigned char>(finite >> 16);
            output[3] = static_cast<unsigned char>(finite >> 24);
        }
        return true;
    }
    if (sourceBytes == 2 && destinationBytes == 4) {
        const bool backwards = source == destination;
        for (unsigned n = 0; n < count; ++n) {
            const unsigned i = backwards ? count - n - 1 : n;
            const auto* input = source + i * 2;
            const unsigned raw =
                static_cast<unsigned>(input[0]) | (static_cast<unsigned>(input[1]) << 8);
            const int sample = raw < 32768 ? static_cast<int>(raw) : static_cast<int>(raw) - 65536;
            const auto bits = pcm16ToFloatBits(static_cast<short>(sample));
            auto* output = destination + i * 4;
            output[0] = static_cast<unsigned char>(bits);
            output[1] = static_cast<unsigned char>(bits >> 8);
            output[2] = static_cast<unsigned char>(bits >> 16);
            output[3] = static_cast<unsigned char>(bits >> 24);
        }
        return true;
    }
    if (sourceBytes == 4 && destinationBytes == 2) {
        for (unsigned i = 0; i < count; ++i) {
            const auto* input = source + i * 4;
            const unsigned bits =
                static_cast<unsigned>(input[0]) | (static_cast<unsigned>(input[1]) << 8) |
                (static_cast<unsigned>(input[2]) << 16) | (static_cast<unsigned>(input[3]) << 24);
            const auto sample = static_cast<unsigned short>(floatBitsToPcm16(bits));
            auto* output = destination + i * 2;
            output[0] = static_cast<unsigned char>(sample);
            output[1] = static_cast<unsigned char>(sample >> 8);
        }
        return true;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto low = source[i * 2], high = source[i * 2 + 1];
        destination[i * 2] = low;
        destination[i * 2 + 1] = high;
    }
    return true;
}
} // namespace loopz_driver
