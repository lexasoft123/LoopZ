#include <loopz/sinc_kernel.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace loopz {
namespace {
double besselI0(double value) {
    double sum = 1, term = 1;
    for (unsigned order = 1; order < 64; ++order) {
        term *= value * value / (4 * order * order);
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}
} // namespace
SincKernel::SincKernel() {
    constexpr double beta = 9;
    const auto normalization = besselI0(beta);
    for (size_t index = 0; index < values_.size(); ++index) {
        const double distance = static_cast<double>(index) / kDensity;
        const auto x = std::numbers::pi_v<double> * distance;
        const auto sinc = index ? std::sin(x) / x : 1;
        const auto relative = distance / kRadius;
        const auto window = besselI0(beta * std::sqrt(std::max(0.0, 1 - relative * relative)));
        values_[index] = static_cast<float>(sinc * window / normalization);
    }
}
const SincKernel& SincKernel::prepared() {
    static const SincKernel value;
    return value;
}
} // namespace loopz
