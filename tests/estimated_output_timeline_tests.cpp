#ifdef LOOPZ_TIMELINE_TEST_HEADER
#include LOOPZ_TIMELINE_TEST_HEADER
#else
#include <loopz/runtime/estimated_output_timeline.hpp>
#endif
#include <cstdio>
#include <cstdlib>
#define CHECK(v)                                                                                   \
    do {                                                                                           \
        if (!(v)) {                                                                                \
            std::fprintf(stderr, "failed %d: %s\n", __LINE__, #v);                                 \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)
int main() {
    using loopz::runtime::detail::EstimatedOutputTimeline;
    for (uint64_t origin : {uint64_t{1000000000}, uint64_t{1000000000000000}, uint64_t{1} << 60}) {
        EstimatedOutputTimeline timeline;
        double maximum = 0;
        for (uint64_t callback = 0; callback < 100000; ++callback) {
            const auto frame = callback * 256;
            const uint64_t raw = origin + frame * 1000000000ULL / 48000;
            const auto result = timeline.observe(frame, raw, 256, 48000, false);
            const double error =
                result.ns >= raw ? double(result.ns - raw) : -double(raw - result.ns);
            maximum = std::max(maximum, std::abs(error));
            CHECK(result.hz >= 48000 * .995 && result.hz <= 48000 * 1.005);
        }
        std::printf("origin=%llu, 100000 callbacks, maximum phase=%.0f ns\n",
                    (unsigned long long)origin, maximum);
        std::fflush(stdout);
        CHECK(maximum < 21); // One thousandth of a sample, independent of uptime.
    }
}
