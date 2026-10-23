#include "../native/platform/windows/driver/adapter_configuration.hpp"
#include "../native/platform/windows/render_only_package.hpp"
#include <cassert>
#include <climits>
#include <initializer_list>

using namespace loopz_driver;
int main() {
    for (const auto version : {L"0.4.0.18", L"0.4.0.19", L"0.4.0.20", L"0.4.0.21", L"0.4.0.22",
                               L"0.4.0.23", L"0.4.0.24", L"0.4.0.25"})
        assert(loopz::windows::supportsRenderOnlyPackage(version));
    for (const auto version : {L"", L"0.4.0.17", L"0.4.0.26", L"0.4.0.210", L"0.4.0.21-extra"})
        assert(!loopz::windows::supportsRenderOnlyPackage(version));
    assert(!loopz::windows::supportsRenderOnlyPackage(std::wstring_view(L"0.4.0.21\0other", 14)));
    static_assert(sizeof(AudioFormatRecord) == 16);
    AdapterConfiguration configuration;
    for (const auto rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
        for (unsigned channels = 2; channels <= 64; channels += 2) {
            assert(decodeAdapterConfiguration({1, 0, {rate, channels}}, configuration));
            assert(configuration.exposesMicrophone());
            assert(configuration.format.sampleRate == rate &&
                   configuration.format.channels == channels);
        }
    const auto sink = appRoutingSinkRecord();
    assert(!sink.valid()); // Legacy microphone-only readers must reject it.
    assert(decodeAdapterConfiguration(sink, configuration));
    assert(!configuration.exposesMicrophone());
    assert(configuration.format.sampleRate == 48000 && configuration.format.channels == 2);
    for (const auto version : {0u, 1u, 2u, 3u, UINT_MAX})
        for (const auto tag : {0u, 1u, routingSinkRecordTag, UINT_MAX}) {
            const AudioFormatRecord record{version, tag, {48000, 2}};
            const bool expected =
                (version == 1 && tag == 0) ||
                (version == routingSinkRecordVersion && tag == routingSinkRecordTag);
            const auto prior = configuration;
            assert(decodeAdapterConfiguration(record, configuration) == expected);
            if (!expected) {
                assert(configuration.mode == prior.mode);
                assert(configuration.format.sampleRate == prior.format.sampleRate);
                assert(configuration.format.channels == prior.format.channels);
            }
        }
    for (const auto rate : {0u, 44100u, 48000u, 88200u, 96000u, 176400u, 192000u, UINT_MAX})
        for (const auto channels : {0u, 1u, 2u, 3u, 4u, 64u, 66u, UINT_MAX}) {
            auto record = sink;
            record.format = {rate, channels};
            assert(decodeAdapterConfiguration(record, configuration) ==
                   (rate == 48000 && channels == 2));
        }
    assert(decodeAdapterConfiguration({}, configuration));
    assert(configuration.exposesMicrophone()); // Existing/default roots retain both endpoints.
}
