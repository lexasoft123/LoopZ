#include "cached_fir_kernels.hpp"
#include <atomic>
namespace loopz::detail {
namespace {
static_assert(std::atomic<CachedFirPairKernel>::is_always_lock_free);
constinit std::atomic<CachedFirPairKernel> selected{nullptr};
} // namespace
void setCachedFirPairKernel(CachedFirPairKernel kernel) noexcept {
    selected.store(kernel, std::memory_order_relaxed);
}
CachedFirPairKernel cachedFirPairKernel() noexcept {
    return selected.load(std::memory_order_relaxed);
}
} // namespace loopz::detail
