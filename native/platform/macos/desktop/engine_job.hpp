#pragma once

#import <Foundation/Foundation.h>
#include "engine_location.hpp"

namespace loopz::macos {
enum class EngineJobDefinition { Current, KnownOld, Foreign };
// Only the program path may differ. All other launchd policy and arguments
// must match exactly; a known old job is preserved until explicit maintenance.
// KnownOld classification here requires expected to use newEnginePath.
// Host::inspect validates the actual jobEngine definition strictly, then returns
// KnownOld via jobEngine != paths_.engine after checking the loaded job.
inline EngineJobDefinition classifyEngineJob(NSDictionary* stored, NSDictionary* expected) {
    if ([stored isEqual:expected])
        return EngineJobDefinition::Current;
    NSArray* arguments = expected[@"ProgramArguments"];
    const auto preferred = newEnginePath / "Contents/MacOS/LoopZ Engine";
    if (![arguments isKindOfClass:NSArray.class] || !arguments.count ||
        ![arguments[0] isEqual:@(preferred.c_str())])
        return EngineJobDefinition::Foreign;
    NSMutableDictionary* legacy = [expected mutableCopy];
    NSMutableArray* oldArguments = [arguments mutableCopy];
    const auto old = legacyEnginePath / "Contents/MacOS/LoopZ Engine";
    oldArguments[0] = @(old.c_str());
    legacy[@"ProgramArguments"] = oldArguments;
    return [stored isEqual:legacy] ? EngineJobDefinition::KnownOld : EngineJobDefinition::Foreign;
}
} // namespace loopz::macos
