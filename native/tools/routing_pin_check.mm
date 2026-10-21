#include <loopz/macos/routing_service.hpp>
#import <Foundation/Foundation.h>
#include <dlfcn.h>

#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2)
            return 2;
        try {
            const auto library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
            if (!library)
                throw std::runtime_error("Cannot load routing driver for trust validation");
            const auto symbol = dlsym(library, "LoopZRoutingAudio_Create");
            const auto endpoint = loopz::macos::bundledRoutingServiceEndpoint(symbol);
            NSDictionary* result = @{
                @"brokerRequirement" : @(endpoint.serverRequirement.c_str()),
                @"service" : @(endpoint.serviceName.c_str()),
                @"serverUID" : @(endpoint.serverUID),
                @"privileged" : @(endpoint.privileged),
            };
            const auto data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
            if (!data)
                return 1;
            std::fwrite(data.bytes, 1, data.length, stdout);
            std::putchar('\n');
            // No factory invocation, audio IO, connection or unload. This
            // process owns the mapped image until normal process teardown.
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Routing bundle trust validation: %s\n", error.what());
            return 1;
        }
    }
}
