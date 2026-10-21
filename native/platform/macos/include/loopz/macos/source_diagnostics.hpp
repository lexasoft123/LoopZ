#pragma once

#include <loopz/source_registry.hpp>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>

namespace loopz::macos {
// Control-domain encoding only. Exact counters remain strings at the JS
// boundary; source samples and callback-owned live state are never inspected.
inline NSDictionary* sourceDiagnosticsJSON(const SourceDiagnostics& diagnostics) {
    const auto counter = [](auto value) {
        return @(std::to_string(value).c_str());
    };
    const auto& history = diagnostics.history;
    auto* missing = [NSMutableDictionary dictionary];
    constexpr std::array names{"invalidRequest", "offline",           "anchorUnavailable",
                               "clockMapping",   "resamplingSupport", "generationChanged"};
    static_assert(names.size() == static_cast<size_t>(AudioHistoryMissingReason::Count));
    for (size_t index = 0; index < names.size(); ++index)
        missing[@(names[index])] = counter(history.missingFrames[index]);
    NSMutableDictionary* result = [@{
        @"history" : @{
            @"enabled" : @(history.enabled),
            @"reads" : counter(history.reads),
            @"requestedFrames" : counter(history.requestedFrames),
            @"missingOutputFrames" : missing,
            @"unpublishedSampleReads" : counter(history.unpublishedSampleReads),
            @"expiredSampleReads" : counter(history.expiredSampleReads),
            @"racingSampleReads" : counter(history.racingSampleReads),
            @"absentSampleReads" : counter(history.absentSampleReads)
        }
    } mutableCopy];
    if (diagnostics.stoppedDelivery) {
        const auto& delivery = *diagnostics.stoppedDelivery;
        auto* events = [NSMutableArray array];
        for (uint32_t index = 0;
             index < std::min<size_t>(delivery.eventCount, delivery.events.size()); ++index) {
            const auto& event = delivery.events[index];
            [events addObject:@{
                @"elapsedNs" : counter(event.elapsedNs),
                @"hostIntervalNs" : counter(event.hostIntervalNs),
                @"callbackIntervalNs" : counter(event.callbackIntervalNs),
                @"processingNs" : counter(event.processingNs),
                @"previousFrames" : @(event.previousFrames),
                @"frames" : @(event.frames),
                @"sourceFrameDelta" : std::isfinite(event.sourceFrameDelta)
                    ? @(event.sourceFrameDelta)
                    : NSNull.null,
                @"expectedSourceFrameDelta" : @(event.expectedSourceFrameDelta),
                @"sourceFrameValid" : @(event.sourceFrameValid)
            }];
        }
        result[@"stoppedDelivery"] = @{
            @"callbacks" : counter(delivery.callbacks),
            @"frames" : counter(delivery.frames),
            @"warmupCallbacks" : counter(delivery.warmupCallbacks),
            @"rejected" : counter(delivery.rejected),
            @"minimumFrames" : @(delivery.minimumFrames),
            @"maximumFrames" : @(delivery.maximumFrames),
            @"minimumEntryAgeNs" : counter(delivery.minimumEntryAgeNs),
            @"maximumEntryAgeNs" : counter(delivery.maximumEntryAgeNs),
            @"minimumReadyEndAgeNs" : counter(delivery.minimumReadyEndAgeNs),
            @"maximumReadyEndAgeNs" : counter(delivery.maximumReadyEndAgeNs),
            @"minimumGapNs" : counter(delivery.minimumGapNs),
            @"maximumGapNs" : counter(delivery.maximumGapNs),
            @"maximumProcessingNs" : counter(delivery.maximumProcessingNs),
            @"maximumAnchorErrorNs" : counter(delivery.maximumAnchorErrorNs),
            @"unsupportedAnchorIntervals" : counter(delivery.unsupportedAnchorIntervals),
            @"sourceFrameDiscontinuities" : counter(delivery.sourceFrameDiscontinuities),
            @"events" : events
        };
    }
    return result;
}
} // namespace loopz::macos
