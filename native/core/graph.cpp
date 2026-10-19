#include <loopz/graph.hpp>
#include <loopz/timeline.hpp>
#include <zdsp/graph_runner.h>
#include <array>
#include <stdexcept>
#include <vector>

namespace loopz {
struct MixingGraph::Impl {
    std::vector<uint8_t> memory{std::vector<uint8_t>(4 * 1024 * 1024)};
    zdsp::RealtimeArena arena{};
    zdsp::AudioBusDescriptor stereo{2, zdsp::SampleFormat::Float32Planar,
                                    zdsp::AudioChannelLayout::Stereo, nullptr};
    zdsp::AudioBusDescriptor mixInputs[3]{stereo, stereo, stereo};
    zdsp::GraphNodeDescription nodes[7]{};
    zdsp::GraphConnection edges[6]{};
    zdsp::GraphCompileResult compiled{};
    zdsp::SnapshotPublisher publisher{};
    zdsp::RetirementSlot retired[2]{};
    zdsp::PublishedGraphSnapshot snapshot{};
    zdsp::GraphRunner runner{};
    zdsp::RuntimeDiagnostics diagnostics{};
    double rate;
    explicit Impl(double sampleRate) : rate(sampleRate) {
        using namespace zdsp;
        if (!succeeded(
                initializeArena(&arena, {memory.data(), static_cast<uint32_t>(memory.size())})))
            throw std::runtime_error("DSP arena initialization failed");
        for (uint32_t i = 0; i < 3; ++i)
            nodes[i] = {{i + 1},           {0, i + 1}, 1, GraphNodeRole::Input,
                        GraphNodeFlagNone, 0,          1, nullptr,
                        &stereo,           {},         {}};
        auto processor = [&](uint32_t index, BuiltinNodeKind kind, uint32_t inputs, float value) {
            BuiltinNodeConfig config{
                kind, {index + 1}, 2, 2, inputs, value, 0, 0, OscillatorWaveform::Sine, nullptr, 0};
            auto stateBytes = builtinStateBytes(config);
            auto* state = static_cast<uint8_t*>(arenaAllocate(&arena, stateBytes, 64));
            const auto preparedBytes = builtinPreparedBytes(config, {kBlock});
            void* prepared = preparedBytes ? arenaAllocate(&arena, preparedBytes, 64) : nullptr;
            if (!state || (preparedBytes && !prepared))
                throw std::runtime_error("DSP processor capacity exhausted");
            const auto handle =
                createBuiltinProcessor(config, {state, static_cast<uint32_t>(stateBytes)});
            nodes[index] = {{index + 1},
                            {1, static_cast<uint64_t>(kind)},
                            1,
                            GraphNodeRole::Processor,
                            GraphNodeFlagNone,
                            inputs,
                            1,
                            inputs == 3 ? mixInputs : &stereo,
                            &stereo,
                            handle,
                            {prepared, preparedBytes, 64}};
        };
        processor(3, BuiltinNodeKind::Mix, 3, 1);
        processor(4, BuiltinNodeKind::SafetyLimiter, 1, 0.98f);
        processor(5, BuiltinNodeKind::PeakRms, 1, 1);
        nodes[6] = {{7},     {0, 7}, 1, GraphNodeRole::Output, GraphNodeFlagNone, 1, 0, &stereo,
                    nullptr, {},     {}};
        edges[0] = {{1}, 0, {4}, 0};
        edges[1] = {{2}, 0, {4}, 1};
        edges[2] = {{3}, 0, {4}, 2};
        edges[3] = {{4}, 0, {5}, 0};
        edges[4] = {{5}, 0, {6}, 0};
        edges[5] = {{6}, 0, {7}, 0};
        GraphDescription description{kGraphFormatVersion, {rate}, {kBlock}, nodes, 7, edges, 6};
        GraphCompileError error{};
        if (!succeeded(compileGraph(description, &arena, &compiled, &error))) {
            (void)cleanupFailedCompile(&compiled);
            throw std::runtime_error("DSP graph compilation failed: " +
                                     std::to_string(static_cast<uint32_t>(error.kind)));
        }
        initializePublisher(&publisher, retired, 2, &diagnostics);
        snapshot.graph = compiled.graph;
        snapshot.generation = 1;
        snapshot.transition = {TransitionKind::HardCut,
                               {0},
                               {0},
                               {0},
                               InfiniteTailPolicy::Cut,
                               {TailKind::None, {0}},
                               {0},
                               0,
                               100,
                               1000,
                               0};
        if (!succeeded(submitSnapshot(&publisher, &snapshot).status))
            throw std::runtime_error("DSP graph publication failed");
        initializeGraphRunner(&runner, &publisher, {}, nullptr, nullptr, &diagnostics);
    }
    ~Impl() {
        zdsp::PublishedGraphSnapshot* snapshots[4]{};
        uint32_t count = 0;
        (void)zdsp::shutdownGraphRunner(&runner, snapshots, 4, &count);
        if (compiled.graph)
            (void)zdsp::deactivateCompiledGraph(compiled.graph);
    }
};
MixingGraph::MixingGraph(double rate) : impl_(std::make_unique<Impl>(rate)) {}
MixingGraph::~MixingGraph() = default;
bool MixingGraph::render(const float* al, const float* ar, const float* ml, const float* mr,
                         const float* pl, const float* pr, float* l, float* r, uint32_t frames,
                         uint64_t position) noexcept {
    using namespace zdsp;
    const float* app[]{al, ar};
    const float* mic[]{ml, mr};
    const float* pass[]{pl, pr};
    ConstAudioBusView inputs[]{{app, 2, {frames}, {frames}, nullptr},
                               {mic, 2, {frames}, {frames}, nullptr},
                               {pass, 2, {frames}, {frames}, nullptr}};
    float* output[]{l, r};
    MutableAudioBusView outputs{output, 2, {frames}, {frames}};
    ProcessContext context{kProcessContextInterfaceVersion,
                           kProcessContextV1RequiredSize,
                           {{1}, {1}, {position}, {0}, {0}, RenderTimeNone},
                           nullptr,
                           {impl_->rate},
                           {frames},
                           nullptr,
                           0,
                           nullptr,
                           0,
                           {nullptr, 0},
                           {DiscontinuityReason::None, DiscontinuityFlagNone}};
    return succeeded(renderGraphBlock(&impl_->runner, context, inputs, 3, &outputs, 1));
}
} // namespace loopz
