#pragma once
#include <memory>
#include <cstdint>
namespace loopz {
class MixingGraph {
  public:
    explicit MixingGraph(double rate);
    ~MixingGraph();
    MixingGraph(const MixingGraph&) = delete;
    MixingGraph& operator=(const MixingGraph&) = delete;
    bool render(const float* appLeft, const float* appRight, const float* micLeft,
                const float* micRight, const float* passLeft, const float* passRight, float* left,
                float* right, uint32_t frames, uint64_t position) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace loopz
