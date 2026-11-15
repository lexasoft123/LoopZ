#include "../../native/platform/macos/desktop/session_host.hpp"
#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <cassert>
#include <fstream>

using namespace loopz::macos;
namespace fs = std::filesystem;
int main() {
    @autoreleasepool {
        const fs::path root = std::string(NSTemporaryDirectory().UTF8String) + "loopz-inspect-" +
                              NSUUID.UUID.UUIDString.UTF8String;
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                fs::remove_all(path);
            }
        } cleanup{root};
        const auto old = root / "old.app", preferred = root / "new.app";
        const auto directory = root / "Session", routes = root / "routes.json";
        fs::create_directories(directory);
        chmod(directory.c_str(), 0700);
        fs::create_directories(old / "Contents/MacOS");
        const auto program = old / "Contents/MacOS/LoopZ Engine";
        std::ofstream(program) << "fixture";
        NSArray* arguments = @[
            @(program.c_str()), @"--routing-document", @(routes.c_str()), @"--editor-service",
            @"true", @"--block-frames", @"64"
        ];
        NSDictionary* definition = @{
            @"Label" : @"fixture.session",
            @"ProgramArguments" : arguments,
            @"RunAtLoad" : @YES,
            @"KeepAlive" : @YES,
            @"ThrottleInterval" : @10,
            @"ProcessType" : @"Interactive",
            @"ExitTimeOut" : @5,
            @"MachServices" : @{@"fixture.control" : @YES},
            @"StandardOutPath" : @"/dev/null",
            @"StandardErrorPath" : @"/dev/null"
        };
        NSData* data =
            [NSPropertyListSerialization dataWithPropertyList:definition
                                                       format:NSPropertyListXMLFormat_v1_0
                                                      options:0
                                                        error:nil];
        assert([data writeToFile:@((directory / "engine.plist").c_str()) atomically:YES]);
        chmod((directory / "engine.plist").c_str(), 0600);
        std::string output = "path = " + (directory / "engine.plist").string() +
                             "\nprogram = " + program.string() + "\narguments = {\n";
        for (NSString* argument in arguments)
            output += std::string(argument.UTF8String) + "\n";
        output += "}\nproperties = keepalive\n";
        auto inspect = [&](const auto& command) -> SessionCommandResult {
            assert(command.size() == 3 && command[0] == "/bin/launchctl" && command[1] == "print");
            return {0, output}; // No launchd operations or live processes in this fixture.
        };
        SessionPaths paths{preferred, routes, directory, "fixture.session", "fixture.control", old};
        auto host = makeSessionHost(paths, "fixture pin", inspect);
        // inspect validates against jobEngine (old), then compares it with paths.engine.
        assert(host->inspect() == SessionJob::KnownOld);
        paths.engine = old;
        assert(makeSessionHost(paths, "fixture pin", inspect)->inspect() == SessionJob::Managed);
        const auto original = output;
        output.replace(output.find("\n64\n"), 4, "\n63\n");
        assert(host->inspect() == SessionJob::Foreign);
        output = original;
        fs::remove(program);
        bool recoverable = false;
        try {
            (void)host->inspect();
        } catch (const std::runtime_error& error) {
            recoverable = std::string(error.what()).find("Repair audio") != std::string::npos;
        }
        assert(recoverable);
    }
}
