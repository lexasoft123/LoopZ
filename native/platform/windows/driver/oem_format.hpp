// LoopZ contributors. MIT license.
#pragma once
#include "audio_format.hpp"
namespace loopz_driver {
// PKEY_AudioEngine_OEMFormat in a driver interface's EP\0 key uses the
// documented serialized VT_BLOB representation, not a raw WAVEFORMATEX.
// https://learn.microsoft.com/windows-hardware/drivers/audio/pkey-audioengine-oemformat
struct OemFormatProperty {
    unsigned char data[48]{};
    void word(unsigned offset, unsigned value) noexcept {
        data[offset] = static_cast<unsigned char>(value & 255);
        data[offset + 1] = static_cast<unsigned char>((value >> 8) & 255);
    }
    void dword(unsigned offset, unsigned value) noexcept {
        word(offset, value);
        word(offset + 2, value >> 16);
    }
    bool build(AudioFormat format, unsigned sampleBytes = 2) noexcept {
        for (auto& byte : data)
            byte = 0;
        if (!format.valid() || (sampleBytes != 2 && sampleBytes != 4))
            return false;
        dword(0, 65); // serialized VT_BLOB
        dword(4, 40); // WAVEFORMATEXTENSIBLE bytes
        word(8, 0xfffe);
        word(10, format.channels);
        dword(12, format.sampleRate);
        dword(16, format.sampleRate * (format.channels * sampleBytes));
        word(20, (format.channels * sampleBytes));
        word(22, sampleBytes * 8);
        word(24, 22);
        word(26, sampleBytes * 8);
        dword(28, format.channels == 2 ? 3 : 0); // stereo or ordinal/direct-out
        // KSDATAFORMAT_SUBTYPE_PCM, GUID fields serialized little-endian.
        data[32] = sampleBytes == 4 ? 3 : 1;
        data[38] = 0x10;
        data[40] = 0x80;
        data[43] = 0xaa;
        data[45] = 0x38;
        data[46] = 0x9b;
        data[47] = 0x71;
        return true;
    }
};
static_assert(sizeof(OemFormatProperty) == 48);
} // namespace loopz_driver
