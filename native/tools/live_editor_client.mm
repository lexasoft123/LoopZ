#include <loopz/macos/editor_service.hpp>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <charconv>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3)
            return 2;
        const std::string_view mode = argv[1];
        uint32_t previewSeconds = 0;
        if (mode == "preview" && argc == 3) {
            const std::string_view value = argv[2];
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), previewSeconds);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                previewSeconds < 1 || previewSeconds > 120)
                return 2;
        } else if (argc != 2 || (mode != "snapshot" && mode != "edit" && mode != "initialize" &&
                                 mode != "hardware-buffer"))
            return 2;
        loopz::macos::EditorServiceClient client(loopz::macos::productionEditorEndpoint());
        client.connect();
        std::string result;
        if (previewSeconds) {
            client.preview();
            std::puts("{\"preview\":true}");
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(previewSeconds));
            return 0;
        } else if (mode == "snapshot")
            result = client.snapshot();
        else if (mode == "edit" || mode == "initialize" || mode == "hardware-buffer") {
            std::string request;
            char value;
            while (std::cin.get(value)) {
                if (request.size() >= 1024 * 1024)
                    throw std::runtime_error("Request exceeds the diagnostic bound");
                request += value;
            }
            if (mode == "edit")
                result = client.edit(request);
            else if (mode == "initialize")
                result = client.initialize(request);
            else
                result = client.hardwareBuffer(request);
        } else
            return 2;
        std::puts(result.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
