#include "../../native/platform/macos/desktop/session_host.hpp"
#include "../../native/platform/macos/desktop/engine_migration.hpp"
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
        SessionPaths paths{preferred,         routes, directory, "fixture.session",
                           "fixture.control", old,    preferred};
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
        fs::create_directories(preferred / "Contents/MacOS");
        const auto newProgram = preferred / "Contents/MacOS/LoopZ Engine";
        std::ofstream(newProgram) << "fixture";
        chmod(newProgram.c_str(), 0755);
        NSMutableDictionary* migrated = [definition mutableCopy];
        NSMutableArray* newArguments = [arguments mutableCopy];
        newArguments[0] = @(newProgram.c_str());
        migrated[@"ProgramArguments"] = newArguments;
        NSData* migratedData =
            [NSPropertyListSerialization dataWithPropertyList:migrated
                                                       format:NSPropertyListBinaryFormat_v1_0
                                                      options:0
                                                        error:nil];
        assert([migratedData writeToFile:@((directory / "engine.plist").c_str()) atomically:YES]);
        chmod((directory / "engine.plist").c_str(), 0600);
        output = "path = " + (directory / "engine.plist").string() +
                 "\nprogram = " + newProgram.string() + "\narguments = {\n";
        for (NSString* argument in newArguments)
            output += std::string(argument.UTF8String) + "\n";
        output += "}\nproperties = keepalive\n";
        NSDictionary* graph =
            @{@"devices" : @[
                @{@"bindings" : @[ @{@"kind" : @"application"} ]}
            ]};
        const auto selected = sessionEnginePath(graph, preferred, old, false,
                                                accountEngineJob(directory / "engine.plist"));
        assert(selected == preferred);
        paths.engine = selected;
        assert(makeSessionHost(paths, "fixture pin", inspect)->inspect() == SessionJob::Managed);
        paths.engine = old; // Host created before the move still accepts its new-path job.
        assert(makeSessionHost(paths, "fixture pin", inspect)->inspect() == SessionJob::Managed);

        // Use the same account factory called by app launch. Invalid or transient
        // job metadata cannot escape construction; ordinary supervision reports
        // validation errors and can recover after the file becomes valid.
        const auto job = directory / "engine.plist", alias = directory / "publish.tmp";
        for (int kind = 0; kind < 8; ++kind) {
            fs::remove(job);
            fs::remove(alias);
            if (kind == 0) {
                std::ofstream(job) << "unreadable";
                chmod(job.c_str(), 0000);
            } else if (kind == 1) {
                std::ofstream(alias) << "linked";
                fs::create_symlink(alias, job);
            } else if (kind == 2) {
                assert([migratedData writeToFile:@(job.c_str()) atomically:YES]);
                fs::create_hard_link(job, alias); // linkat/unlinkat publication window
            } else if (kind == 3) {
                std::ofstream empty(job);
            } else if (kind == 4) {
                std::ofstream(job) << std::string(65537, 'x');
            } else if (kind == 5) {
                NSData* array = [NSPropertyListSerialization
                    dataWithPropertyList:@[]
                                  format:NSPropertyListBinaryFormat_v1_0
                                 options:0
                                   error:nil];
                assert([array writeToFile:@(job.c_str()) atomically:YES]);
            } else if (kind == 6)
                std::ofstream(job) << "not a property list";
            else
                assert(!mkfifo(job.c_str(), 0600));
            if (kind != 0 && kind != 1)
                chmod(job.c_str(), 0600);
            bool invalid = false;
            assert(!accountEngineJob(job, &invalid) && invalid);
            auto launchHost = makeAccountSessionHost(paths, "fixture pin", inspect);
            SessionSupervisor supervisor(std::move(launchHost));
            supervisor.poll(0);
            assert(!supervisor.status().managed && !supervisor.status().detail.empty());
            fs::remove(job);
            fs::remove(alias);
            assert([migratedData writeToFile:@(job.c_str()) atomically:YES]);
            chmod(job.c_str(), 0600);
            supervisor.retry();
            supervisor.poll(1);
            assert(supervisor.status().managed);
        }
    }
}
