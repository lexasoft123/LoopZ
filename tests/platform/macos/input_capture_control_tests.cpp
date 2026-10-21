#include "../../../native/platform/macos/probe/input_capture_control.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace {
class Input {
    int original_{-1};

  public:
    explicit Input(std::string_view bytes) {
        int descriptors[2];
        assert(::pipe(descriptors) == 0);
        assert(::write(descriptors[1], bytes.data(), bytes.size()) ==
               static_cast<ssize_t>(bytes.size()));
        ::close(descriptors[1]);
        original_ = ::dup(STDIN_FILENO);
        assert(original_ >= 0 && ::dup2(descriptors[0], STDIN_FILENO) >= 0);
        ::close(descriptors[0]);
    }
    ~Input() {
        assert(::dup2(original_, STDIN_FILENO) >= 0);
        ::close(original_);
    }
};
bool accepts(std::string_view bytes, const char* expected, bool stop = false) {
    Input input(bytes);
    volatile std::sig_atomic_t stopping = stop;
    try {
        loopz::macos::diagnostic::awaitInputCapture(expected, stopping);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace
int main() {
    assert(accepts("begin-capture\n", "begin-capture"));
    assert(accepts("release-input\n", "release-input"));
    assert(!accepts("release-input\n", "begin-capture"));
    assert(!accepts("begin-capture\n", "begin-capture", true));
    assert(!accepts("", "begin-capture"));
    assert(!accepts("begin-capture", "begin-capture"));
    assert(!accepts("12345678901234567890123456789012345\n", "begin-capture"));
    std::puts(
        "PASS: input capture barrier commands, bounds, EOF and cancellation without audio IO");
}
