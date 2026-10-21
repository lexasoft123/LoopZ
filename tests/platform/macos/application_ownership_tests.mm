#include <loopz/macos/application_routes.hpp>
#include <loopz/macos/application_ownership.hpp>
#import <Foundation/Foundation.h>

#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    @autoreleasepool {
        using namespace loopz::macos;
        if (argc == 3 && std::string(argv[1]) == "--inspect") {
            ApplicationAudioSelection selected;
            std::string error;
            bool success = selectApplicationAudio(argv[2], selected, error);
            NSMutableArray* processes = [NSMutableArray array];
            for (const auto& item : selected.identities)
                [processes addObject:@{
                    @"object" : @(item.object),
                    @"pid" : @(item.process.id),
                    @"generation" : @(item.process.generation)
                }];
            NSMutableArray* outputs = [NSMutableArray array];
            for (auto device : selected.outputs) {
                CFStringRef uid = nullptr;
                UInt32 size = sizeof(uid);
                AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceUID,
                                                   kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMain};
                if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &uid) || !uid ||
                    CFGetTypeID(uid) != CFStringGetTypeID()) {
                    if (uid)
                        CFRelease(uid);
                    success = false;
                    error = "An application output changed during metadata inspection";
                    break;
                }
                [outputs addObject:@{@"object" : @(device), @"uid" : (__bridge NSString*)uid}];
                CFRelease(uid);
            }
            const auto data = [NSJSONSerialization dataWithJSONObject:@{
                @"success" : @(success),
                @"error" : @(error.c_str()),
                @"processes" : processes,
                @"outputs" : outputs,
                @"producing" : @(selected.producing),
                @"responsibilityAvailable" : @(ApplicationOwnership::responsibilityAvailable()),
                @"devicesOpened" : @NO,
                @"tapsCreated" : @NO,
                @"permissionRequested" : @NO
            }
                                                              options:0
                                                                error:nil];
            std::fwrite(data.bytes, 1, data.length, stdout);
            std::putchar('\n');
            return success ? 0 : 1;
        }
        ApplicationAudioSelection selected;
        std::string error;
        assert(!selectApplicationAudio("", selected, error) && !error.empty());
        assert(!selectApplicationAudio(std::string(4097, 'x'), selected, error));
        assert(!selectApplicationAudio(std::string(1, char(0xff)), selected, error));
        // The ordinary test target performs no native process/device queries.
        // Real metadata-only inspection is an explicit diagnostic above.
    }
}
