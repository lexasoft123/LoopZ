#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

// CPU/filesystem-only child for workspace ownership tests. No audio APIs,
// IPC services, permission requests or PID signals are used.
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    std::puts("ready");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (access(argv[1], F_OK) == 0)
            return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 3;
}
