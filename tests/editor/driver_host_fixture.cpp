#include <chrono>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
int version(void* image) {
    const auto symbol = reinterpret_cast<int (*)()>(dlsym(image, "loopz_private_fixture_version"));
    return symbol ? symbol() : -1;
}

void report(const std::string& path, const char* event, int value) {
    std::ofstream output(path, std::ios::app);
    output << "{\"pid\":" << getpid() << ",\"event\":\"" << event << "\",\"version\":" << value
           << "}" << std::endl;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    void* original = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!original) {
        std::cerr << dlerror() << std::endl;
        return 1;
    }
    const std::string events = argv[2];
    report(events, "ready", version(original));
    void* extra = nullptr;
    while (true) {
        const auto request = events + ".load";
        if (std::filesystem::exists(request)) {
            std::string path;
            std::getline(std::ifstream(request), path);
            extra = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!extra) {
                std::cerr << dlerror() << std::endl;
                return 1;
            }
            report(events, "loaded", version(extra));
            std::filesystem::remove(request);
        }
        const auto release = events + ".unload";
        if (extra && std::filesystem::exists(release)) {
            dlclose(extra);
            extra = nullptr;
            report(events, "unloaded", version(original));
            std::filesystem::remove(release);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
