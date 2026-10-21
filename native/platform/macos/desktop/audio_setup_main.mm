#include "audio_maintenance.hpp"
#include "audio_maintenance_service.hpp"
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <cstdio>
#include <string_view>
#include <stdexcept>
#include <vector>

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            if (argc == 2 && std::string_view(argv[1]) == "--service") {
                loopz::macos::serveAudioMaintenance();
                return 0;
            }
            const bool verifyOnly = argc == 2 && std::string_view(argv[1]) == "--verify-payload";
            const bool maintenance =
                argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "--install" ||
                                            std::string_view(argv[1]) == "--update" ||
                                            std::string_view(argv[1]) == "--repair" ||
                                            std::string_view(argv[1]) == "--activate"));
            if (!verifyOnly && (!maintenance || geteuid() != 0))
                throw std::runtime_error("Audio setup needs administrator authorization.");
            uint32_t length = 0;
            _NSGetExecutablePath(nullptr, &length);
            std::vector<char> executable(length);
            if (_NSGetExecutablePath(executable.data(), &length))
                throw std::runtime_error("The setup executable could not be located.");
            const auto helper = std::filesystem::canonical(executable.data());
            const auto app = helper.parent_path().parent_path().parent_path().parent_path();
            if (app.extension() != ".app")
                throw std::runtime_error("Run audio setup from the LoopZ app.");
            if (!verifyOnly && app != "/Applications/LoopZ.app")
                throw std::runtime_error("Move LoopZ to Applications before setting up audio.");
            loopz::macos::verifyAudioPayload(app);
            if (verifyOnly) {
                std::puts("Audio payload verified.");
            } else {
                NSDictionary* result = loopz::macos::maintainAudioComponents(
                    app, argc == 2 && std::string_view(argv[1]) == "--activate");
                NSData* bytes = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
                fwrite(bytes.bytes, 1, bytes.length, stdout);
                std::puts("");
            }
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
}
