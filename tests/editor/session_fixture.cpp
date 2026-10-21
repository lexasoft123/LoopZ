#include <chrono>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--runtime-capabilities") {
        std::puts("{\"backgroundSession\":1}");
        return 0;
    }
    if (argc != 7 || std::string_view(argv[1]) != "--routing-document" ||
        std::string_view(argv[3]) != "--editor-service" || std::string_view(argv[4]) != "true" ||
        std::string_view(argv[5]) != "--block-frames" || std::string_view(argv[6]) != "64")
        return 2;
    std::ofstream events(std::string(argv[2]) + ".events", std::ios::app);
    events << getpid() << std::endl;
    events.close();
    while (true)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
