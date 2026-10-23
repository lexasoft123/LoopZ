#include "process_capture.hpp"
#include "endpoint_roles.hpp"
#include <loopz/audio_meter.hpp>
#include <loopz/routing.hpp>
#include <zcore/device/audio_host.h>
#include <zcore/device/audio_input.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
std::string quote(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

struct RenderContext {
    std::unique_ptr<loopz::RoutingGraph> graph;
    std::array<std::array<float, singz::kAudioHostMaxFrames>, 2> tone{};
    loopz::AudioMeter meter{2};
    bool microphone{false};
    bool generateTone{false};
    double frequency{440};
    uint64_t position{0};

    static bool render(void* raw, const singz::AudioHostRenderBlock& block) noexcept {
        auto& self = *static_cast<RenderContext*>(raw);
        if (block.frames > singz::kAudioHostMaxFrames || block.outputChannels != 2)
            return false;
        std::array<const float*, 2> input{self.tone[0].data(), self.tone[1].data()};
        if (self.microphone && block.input && block.inputChannels) {
            input = {block.input[0], block.input[block.inputChannels > 1 ? 1 : 0]};
        } else {
            for (uint32_t frame = 0; frame < block.frames; ++frame) {
                // Quiet, explicitly requested tone; enumeration never opens audio.
                const float sample = self.generateTone
                                         ? 0.03f * static_cast<float>(std::sin(
                                                       6.283185307179586 * self.frequency *
                                                       static_cast<double>(self.position + frame) /
                                                       block.sampleRate))
                                         : 0.0f;
                self.tone[0][frame] = self.tone[1][frame] = sample;
            }
        }
        bool ok = true;
        for (uint32_t offset = 0; offset < block.frames; offset += 256) {
            const auto count = std::min(256u, block.frames - offset);
            const std::array<const float*, 2> inputs{input[0] + offset, input[1] + offset};
            const std::array<float*, 2> outputs{block.output[0] + offset, block.output[1] + offset};
            const std::array<loopz::RoutingInput, 1> sources{{{inputs, count}}};
            ok = self.graph->render(sources, {outputs, count}, count, self.position + offset) && ok;
        }
        self.position += block.frames;
        self.meter.publish({{block.output, 2}, block.frames}, block.frames);
        return ok;
    }
};

void inventory(const singz::AudioHostInventory& devices) {
    std::cout << "{\"backend\":\"wasapi\",\"defaultInput\":" << quote(devices.defaultInputUid)
              << ",\"defaultOutput\":" << quote(devices.defaultOutputUid) << ",\"devices\":[";
    bool first = true;
    for (const auto& device : devices.devices) {
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"uid\":" << quote(device.uid) << ",\"name\":" << quote(device.label)
                  << ",\"inputChannels\":" << device.inputChannels
                  << ",\"outputChannels\":" << device.outputChannels
                  << ",\"sampleRate\":" << device.nominalSampleRate
                  << ",\"minimumBufferFrames\":" << device.bufferFrames.minimumFrames << '}';
    }
    std::cout << "]}\n";
}

uint32_t seconds(std::string_view text) {
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !value || value > 60)
        throw std::invalid_argument("Duration must be an integer between 1 and 60 seconds");
    return value;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view command = argc > 1 ? argv[1] : "--devices";
        if (command == "--help") {
            std::cout << "LoopZ Windows WASAPI probe\n"
                         "  --devices\n"
                         "  --capture [seconds]\n"
                         "  --silence [seconds]\n"
                         "  --tone [seconds]\n"
                         "  --feed-tone seconds FEED_UID (virtual endpoint only)\n"
                         "  --duplex [seconds]  (microphone monitoring; use headphones)\n";
            return 0;
        }
        if (command == "--process") {
            if (argc < 3 || argc > 4)
                throw std::invalid_argument("--process PID [seconds]");
            uint32_t pid = 0;
            const std::string_view id = argv[2];
            const auto parsed = std::from_chars(id.data(), id.data() + id.size(), pid);
            if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || !pid)
                throw std::invalid_argument("Invalid process ID");
            loopz::windows::ProcessCapture capture;
            std::string error;
            if (!capture.start(pid, error))
                throw std::runtime_error(error);
            std::this_thread::sleep_for(std::chrono::seconds(argc > 3 ? seconds(argv[3]) : 3));
            const auto status = capture.history()->status();
            error = capture.error();
            capture.stop();
            std::cout << "{\"mode\":\"process\",\"callbacks\":" << status.callbacks
                      << ",\"discontinuities\":" << status.discontinuities
                      << ",\"rate\":" << status.measuredRate << ",\"error\":" << quote(error)
                      << "}\n";
            return error.empty() ? 0 : 3;
        }
        singz::AudioHost host;
        const auto devices = host.enumerate();
        if (command == "--devices") {
            inventory(devices);
            return devices.devices.empty() ? 2 : 0;
        }
        const bool feedTone = command == "--feed-tone" || command == "--feed-tone-high";
        if (command != "--capture" && command != "--silence" && command != "--tone" &&
            command != "--tone-high" && command != "--duplex" && !feedTone)
            throw std::invalid_argument("Unknown option; use --help");
        if ((feedTone && argc != 4) || (!feedTone && argc > 3))
            throw std::invalid_argument(
                "Expected duration and, for feed tones, a virtual Feed UID");
        std::string outputUid = devices.defaultOutputUid;
        if (feedTone) {
            outputUid = argv[3];
            const auto endpoints = loopz::windows::loopzEndpoints();
            if (!endpoints.contains(outputUid) ||
                endpoints.at(outputUid).role != loopz::windows::EndpointRole::Feed)
                throw std::invalid_argument("Feed tones require a LoopZ virtual playback endpoint");
        }
        const auto duration = std::chrono::seconds(argc > 2 ? seconds(argv[2]) : 3);
        if (command == "--capture") {
            auto inputs = singz::enumerateAudioInputDevices();
            auto selected = std::find_if(inputs.begin(), inputs.end(), [](const auto& device) {
                return device.isDefault;
            });
            if (selected == inputs.end())
                throw std::runtime_error("No default input; run in the logged-in desktop session");
            singz::AudioInput input;
            std::atomic<uint64_t> frames{0}, blocks{0};
            std::atomic<float> peak{0};
            const auto opened = input.start(
                {selected->uid, 0, 32},
                [&](const singz::AudioInputBlockView& block) {
                    float value = peak.load();
                    for (uint32_t i = 0; i < block.frames; ++i)
                        value = std::max(value, std::abs(block.mono[i]));
                    peak.store(value);
                    frames.fetch_add(block.frames);
                    blocks.fetch_add(1);
                },
                &inputs);
            if (!opened.ok)
                throw std::runtime_error(opened.error);
            std::this_thread::sleep_for(duration);
            const auto error = input.lastError();
            input.stop();
            std::cout << "{\"mode\":\"capture\",\"sampleRate\":" << opened.sampleRate
                      << ",\"frames\":" << frames.load() << ",\"blocks\":" << blocks.load()
                      << ",\"peak\":" << peak.load() << ",\"error\":" << quote(error) << "}\n";
            return frames.load() && error.empty() ? 0 : 3;
        }
        const auto output =
            std::find_if(devices.devices.begin(), devices.devices.end(), [&](const auto& device) {
                return device.uid == outputUid;
            });
        if (output == devices.devices.end() || output->outputChannels < 2)
            throw std::runtime_error("No stereo output; run in the logged-in desktop session");
        RenderContext context;
        context.microphone = command == "--duplex";
        context.generateTone = command == "--tone" || command == "--tone-high" || feedTone;
        context.frequency = command == "--tone-high" || command == "--feed-tone-high" ? 880 : 440;
        loopz::RoutingConfiguration routing;
        routing.sources.push_back({"probe", 2, 1, true, false, {{0, 0, 1}, {1, 1, 1}}});
        context.graph = std::make_unique<loopz::RoutingGraph>(output->nominalSampleRate, routing);
        singz::AudioHostConfig config;
        config.outputDeviceUid = output->uid;
        config.outputChannels = {0, 1};
        if (context.microphone) {
            config.inputDeviceUid = devices.defaultInputUid;
            config.inputChannels = {0};
        }
        const auto opened = host.open(config, RenderContext::render, &context);
        if (!opened.ok)
            throw std::runtime_error(opened.message);
        const auto started = host.start();
        if (!started.ok)
            throw std::runtime_error(started.message);
        std::this_thread::sleep_for(duration);
        const auto status = host.status();
        host.stop();
        loopz::AudioMeterSnapshot meter;
        (void)context.meter.read(meter);
        std::cout << "{\"mode\":" << quote(command.substr(2))
                  << ",\"sampleRate\":" << opened.format.sampleRate
                  << ",\"bufferFrames\":" << opened.format.nominalBufferFrames
                  << ",\"callbacks\":" << status.callbacks
                  << ",\"renderedFrames\":" << status.renderedFrames
                  << ",\"renderFailures\":" << status.renderFailures
                  << ",\"xruns\":" << status.xruns << ",\"peak\":" << meter.peak[0] << "}\n";
        return status.state == singz::AudioHostState::Running && status.renderedFrames &&
                       !status.renderFailures
                   ? 0
                   : 3;
    } catch (const std::exception& error) {
        std::cerr << "{\"error\":" << quote(error.what()) << "}\n";
        return 1;
    }
}
