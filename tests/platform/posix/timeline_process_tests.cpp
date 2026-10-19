#include <loopz/timeline.hpp>
#include <cassert>
#include <cstdio>
#include <new>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    using namespace loopz;
    struct Exchange {
        Timeline ring;
        std::atomic<uint64_t> published{0}, acknowledged[2]{};
    };
    // One writer and two live reader processes exchange 128 batches, including
    // wraparound. The writer cannot finish until both independently validate.
    auto* memory =
        mmap(nullptr, sizeof(Exchange), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    auto* shared = new (memory) Exchange{};
    pid_t children[2]{};
    for (uint32_t reader = 0; reader < 2; ++reader) {
        children[reader] = fork();
        assert(children[reader] >= 0);
        if (children[reader] == 0) {
            uint64_t observations = 0;
            for (uint64_t batch = 1; batch <= 128; ++batch) {
                while (shared->published.load() < batch)
                    std::this_thread::yield();
                for (uint64_t i = (batch - 1) * 512; i < batch * 512; ++i) {
                    float left, right;
                    if (!shared->ring.read(i, left, right) || left != static_cast<float>(i) ||
                        right != -static_cast<float>(i))
                        _exit(1);
                    ++observations;
                }
                shared->acknowledged[reader].store(batch);
            }
            _exit(observations == 65536 ? 0 : 1);
        }
    }
    for (uint64_t batch = 1; batch <= 128; ++batch) {
        for (uint64_t i = (batch - 1) * 512; i < batch * 512; ++i)
            shared->ring.write(i, static_cast<float>(i), -static_cast<float>(i));
        shared->published.store(batch);
        while (shared->acknowledged[0].load() < batch || shared->acknowledged[1].load() < batch)
            std::this_thread::yield();
    }
    for (auto child : children) {
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
    }
    munmap(memory, sizeof(Exchange));
    std::puts("PASS: POSIX process-shared timeline with two independent readers");
}
