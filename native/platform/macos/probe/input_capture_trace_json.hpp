#pragma once

#include <loopz/probe_capture_trace.hpp>
#import <Foundation/Foundation.h>
#include <stdexcept>
#include <string>

inline std::string captureTraceJSON(const loopz::probe::CaptureTrace& trace) {
    NSMutableArray* zeros = [NSMutableArray array];
    NSMutableArray* gaps = [NSMutableArray array];
    for (const auto& span : trace.zeros())
        [zeros addObject:@{
            @"capturedFrame" : @(std::to_string(span.capturedFrame).c_str()),
            @"hostNs" : @(std::to_string(span.hostNs).c_str()),
            @"receivedNs" : @(std::to_string(span.receivedNs).c_str()),
            @"callback" : @(std::to_string(span.callback).c_str()),
            @"frames" : @(span.frames),
            @"channelMask" : @(span.channels),
            @"renderFlags" : @(span.renderFlags)
        }];
    for (const auto& gap : trace.gaps())
        [gaps addObject:@{
            @"capturedFrame" : @(std::to_string(gap.capturedFrame).c_str()),
            @"hostNs" : @(std::to_string(gap.hostNs).c_str()),
            @"receivedNs" : @(std::to_string(gap.receivedNs).c_str()),
            @"callback" : @(std::to_string(gap.callback).c_str()),
            @"sampleFrameJump" : @(gap.sampleFrameJump),
            @"hostTimeJumpNs" : @(std::to_string(gap.hostTimeJumpNs).c_str()),
            @"timestampValid" : @(gap.timestampValid)
        }];
    auto* data = [NSJSONSerialization dataWithJSONObject:@{
        @"zeroSpans" : zeros,
        @"callbackGaps" : gaps,
        @"capturedFrames" : @(std::to_string(trace.capturedFrames).c_str()),
        @"callbacks" : @(std::to_string(trace.callbacks).c_str()),
        @"zeroSamples" : @(std::to_string(trace.zeroSamples).c_str()),
        @"overflows" : @(std::to_string(trace.overflows).c_str()),
        @"invalidBlocks" : @(std::to_string(trace.invalidBlocks).c_str()),
        @"firstReceivedNs" : @(std::to_string(trace.firstReceivedNs).c_str()),
        @"lastReceivedNs" : @(std::to_string(trace.lastReceivedNs).c_str())
    }
                                                 options:0
                                                   error:nil];
    if (!data)
        throw std::runtime_error("Input capture trace could not be encoded");
    return {static_cast<const char*>(data.bytes), data.length};
}
