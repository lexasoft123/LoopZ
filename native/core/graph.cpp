#include <loopz/graph.hpp>
#include <loopz/routing.hpp>
#include <loopz/timeline.hpp>

#include <array>

namespace loopz {
namespace {
RoutingConfiguration feasibilityRoute() {
    RoutingConfiguration configuration;
    for (const auto* id : {"application", "microphone", "pass-through"}) {
        SourceRouting source;
        source.id = id;
        source.connections = defaultChannelConnections(2, 2);
        configuration.sources.push_back(std::move(source));
    }
    return configuration;
}
} // namespace

struct MixingGraph::Impl {
    RoutingGraph graph;
    explicit Impl(double rate) : graph(rate, feasibilityRoute()) {}
};

MixingGraph::MixingGraph(double rate) : impl_(std::make_unique<Impl>(rate)) {}
MixingGraph::~MixingGraph() = default;

bool MixingGraph::render(const float* appLeft, const float* appRight, const float* micLeft,
                         const float* micRight, const float* passLeft, const float* passRight,
                         float* left, float* right, uint32_t frames, uint64_t position) noexcept {
    const float* app[]{appLeft, appRight};
    const float* mic[]{micLeft, micRight};
    const float* pass[]{passLeft, passRight};
    std::array<RoutingInput, 3> sources{{{app, frames}, {mic, frames}, {pass, frames}}};
    float* output[]{left, right};
    // The legacy call prepares kBlock frames. Larger requests do not establish
    // that its caller allocated buffers of the claimed size.
    if (frames > kBlock)
        return false;
    return impl_->graph.render(sources, {output, frames}, frames, position);
}
} // namespace loopz
