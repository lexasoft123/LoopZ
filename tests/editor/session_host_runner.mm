#include "../../native/platform/macos/desktop/session_host.hpp"
#import <Foundation/Foundation.h>
#include <cstdio>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 7)
            return 2;
        try {
            loopz::macos::SessionPaths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
            loopz::macos::SessionSupervisor supervisor(
                loopz::macos::makeSessionHost(paths, argv[6]));
            supervisor.poll(0);
            const auto& state = supervisor.status();
            auto* result = @{@"managed" : @(state.managed), @"detail" : @(state.detail.c_str())};
            auto* data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
            std::fwrite(data.bytes, 1, data.length, stdout);
            return state.managed ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
