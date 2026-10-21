#include "session.hpp"
#include <cstdio>

int main(int argc, char**) {
    if (argc != 1)
        return 2;
    @autoreleasepool {
        try {
            loopz::macos::SessionSupervisor supervisor(loopz::macos::productionSessionHost());
            supervisor.poll(0);
            if (supervisor.status().managed)
                return 0;
            std::fprintf(stderr, "LoopZ login: %s\n", supervisor.status().detail.c_str());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "LoopZ login: %s\n", error.what());
        }
        return 1;
    }
}
