#pragma once
#include <loopz/audio_history.hpp>
#include <loopz/document_json.hpp>
namespace loopz {
inline JSON energyJSON(const zcore::routing::AudioEnergySnapshot& value) {
    return {{"available", value.available},
            {"skippedPublications", std::to_string(value.skippedPublications)},
            {"rms", value.rms},
            {"highBandRms", value.highBandRms},
            {"highBandHz", 8000},
            {"highBandFilter", "block-local-two-pole-Butterworth"},
            {"blocks", std::to_string(value.blocks)},
            {"frames", value.frames}};
}
inline JSON historyDiagnosticsJSON(const AudioHistoryReadSnapshot& value) {
    const auto& resampler = value.resampler;
    JSON paths = JSON::object();
    constexpr std::array names{"direct",      "phaseTable", "cachedExact",
                               "convolution", "tiledExact", "invalid"};
    for (size_t index = 0; index < names.size(); ++index)
        paths[names[index]] = std::to_string(resampler.blocks[index]);
    return {{"enabled", value.enabled},
            {"publishedEnd", std::to_string(value.publishedEnd)},
            {"minimumRequested", std::to_string(value.minimumRequested)},
            {"maximumRequested", std::to_string(value.maximumRequested)},
            {"readMinusWriteFrames", std::to_string(value.readMinusWriteFrames)},
            {"unpublishedSampleReads", std::to_string(value.unpublishedSampleReads)},
            {"expiredSampleReads", std::to_string(value.expiredSampleReads)},
            {"racingSampleReads", std::to_string(value.racingSampleReads)},
            {"absentSampleReads", std::to_string(value.absentSampleReads)},
            {"inputEnergy", energyJSON(value.inputEnergy)},
            {"outputEnergy", energyJSON(value.outputEnergy)},
            {"resampler",
             {{"lastPathMask", resampler.lastPathMask},
              {"blocks", paths},
              {"tableMisses", std::to_string(resampler.tableMisses)},
              {"cutoff", resampler.cutoff},
              {"sourceRate", resampler.sourceRate},
              {"outputRate", resampler.outputRate},
              {"coefficientTableConstructions",
               std::to_string(resampler.coefficientTableConstructions)},
              {"coefficientCache", "immutable-two-table-bounded-lookup"}}}};
}
} // namespace loopz
