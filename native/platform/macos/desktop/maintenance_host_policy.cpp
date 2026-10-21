#include "maintenance_host_policy.hpp"
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace loopz::macos {
namespace {
uint64_t number(std::string_view text, int base) {
    if (base == 16 && text.starts_with("0x"))
        text.remove_prefix(2);
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid mapped audio image identity.");
    return value;
}
} // namespace
std::optional<AudioMappedImage> loopZMappedImage(std::string_view inventory, int pid) {
    if (pid <= 1 || inventory.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Invalid audio host inventory.");
    std::vector<std::map<char, std::string>> records;
    bool process = false;
    while (!inventory.empty()) {
        const auto end = inventory.find('\0');
        auto field = inventory.substr(0, end);
        inventory.remove_prefix(end == std::string_view::npos ? inventory.size() : end + 1);
        while (!field.empty() && field.front() == '\n')
            field.remove_prefix(1);
        if (field.empty())
            continue;
        const auto key = field.front();
        const auto value = field.substr(1);
        if (key == 'p') {
            if (process || number(value, 10) != static_cast<uint64_t>(pid))
                throw std::runtime_error("Audio host inventory identifies another process.");
            process = true;
        } else if (key == 'f' && process)
            records.push_back({{'f', std::string(value)}});
        else if ((key == 'D' || key == 'i' || key == 'n') && !records.empty()) {
            if (!records.back().emplace(key, value).second)
                throw std::runtime_error("Duplicate audio image inventory field.");
        } else
            throw std::runtime_error("Incomplete audio host inventory.");
    }
    if (!process || records.empty())
        throw std::runtime_error("Missing audio host inventory.");
    std::optional<AudioMappedImage> own;
    bool other = false;
    for (const auto& record : records) {
        const auto name = record.find('n');
        if (name == record.end())
            continue;
        std::string_view path = name->second;
        if (path.ends_with(" (deleted)"))
            path.remove_suffix(10);
        if (path.starts_with("/System/Volumes/Data/Library/"))
            path.remove_prefix(20);
        const bool slot = path.starts_with("/Library/Audio/Plug-Ins/HAL/LoopZAudio.driver/");
        const bool backup = path.starts_with("/Library/Application Support/LoopZ/.maintenance-") &&
                            path.ends_with("/old/5/Contents/MacOS/LoopZAudio");
        if ((path.starts_with("/Library/Audio/Plug-Ins/HAL/") ||
             path.starts_with("/System/Library/Audio/Plug-Ins/HAL/")) &&
            !slot)
            other = true;
        const auto kind = record.at('f');
        if (!(slot || backup) || !path.ends_with("/Contents/MacOS/LoopZAudio") ||
            (kind != "txt" && kind != "mem" && kind != "DEL"))
            continue;
        if (!record.contains('D') || !record.contains('i'))
            throw std::runtime_error("Mapped LoopZ image lacks a filesystem identity.");
        const AudioMappedImage image{number(record.at('D'), 16), number(record.at('i'), 10)};
        if (!image.inode || (own && *own != image))
            throw std::runtime_error("Multiple LoopZ images are resident in the audio host.");
        own = image;
    }
    if (own && other)
        throw std::runtime_error("The LoopZ audio host also contains another audio driver.");
    return own;
}
} // namespace loopz::macos
