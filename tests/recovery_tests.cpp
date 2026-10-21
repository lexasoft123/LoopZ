#include <loopz/recovery.hpp>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace loopz;

void finish(GraphRecovery& recovery, uint64_t now) {
    while (!recovery.restored()) {
        const auto attempts = recovery.attempts(now);
        assert(!attempts.empty());
        for (const auto& attempt : attempts)
            assert(recovery.complete(attempt, true, now));
    }
}
void lossAndRestore() {
    GraphRecovery recovery;
    recovery.setDesired({{"monitor", {"render"}},
                         {"render", {"bridge", "device"}},
                         {"microphone", {}},
                         {"app", {}},
                         {"device", {"bridge"}},
                         {"bridge", {}}},
                        0);
    finish(recovery, 0);
    const auto released = recovery.lost("bridge", 6000);
    assert((released == std::vector<std::string>{"monitor", "render", "device", "bridge"}));
    assert(recovery.ready("app") && recovery.ready("microphone"));
    assert(recovery.desiredCount() == 6 && !recovery.restored());
    assert(recovery.attempts(6099).empty());
    finish(recovery, 6100);
    recovery.lost("microphone", 6200);
    assert(recovery.ready("monitor") && recovery.ready("app"));
    // A device arrival/wake event accelerates retry without tearing down peers.
    recovery.retryNow(6200);
    finish(recovery, 6200);
}
void staleWorkAndTimeouts() {
    GraphRecovery recovery;
    recovery.setDesired({{"device", {}}, {"render", {"device"}}}, 0);
    const auto oldDevice = recovery.attempts(0).at(0);
    recovery.lost("device", 1);
    recovery.retryNow(1);
    const auto newDevice = recovery.attempts(1).at(0);
    assert(!recovery.complete(oldDevice, true, 1));
    assert(recovery.complete(newDevice, true, 1));
    const auto render = recovery.attempts(1).at(0);
    recovery.lost("device", 2);
    assert(!recovery.complete(render, true, 2));
    recovery.retryNow(2);
    finish(recovery, 2);
    recovery.setDesired({{"device", {}}}, 3);
    const auto slow = recovery.attempts(3).at(0);
    assert(recovery.attempts(10002).empty());
    assert(!recovery.complete(slow, true, 10003));
    assert(recovery.attempts(10003).empty());
    const auto retry = recovery.attempts(10103).at(0);
    assert(!recovery.complete(slow, true, 10103));
    assert(recovery.complete(retry, true, 10103));
    assert(!recovery.complete(retry, false, 10103));
    // A queued callback for an older desired graph never acquires its replacement.
    recovery.setDesired({{"device", {}}}, 11000);
    const auto replaced = recovery.attempts(11000).at(0);
    recovery.setDesired({{"device", {}}}, 11001);
    const auto current = recovery.attempts(11001).at(0);
    assert(!recovery.complete(replaced, true, 11001));
    assert(recovery.complete(current, true, 11001));
}
void unavailableForeverAndFlapping() {
    GraphRecovery recovery;
    recovery.setDesired({{"app", {}}, {"microphone", {}}, {"virtual-output", {}}}, 0);
    auto initial = recovery.attempts(0);
    for (const auto& attempt : initial)
        recovery.complete(attempt, attempt.id != "microphone", 0);
    uint64_t now = 0;
    uint64_t previous = 0;
    unsigned retries = 0;
    // One hour of absent hardware: finite polling cost, no retry exhaustion,
    // and successful routes stay online throughout.
    for (; now < 3600000; now += 10) {
        for (const auto& attempt : recovery.attempts(now)) {
            assert(attempt.id == "microphone");
            if (retries > 8)
                assert(now - previous == 5000);
            previous = now;
            ++retries;
            recovery.complete(attempt, false, now);
        }
        assert(recovery.ready("app") && recovery.ready("virtual-output"));
    }
    assert(retries > 700 && retries < 740);
    assert(recovery.desiredCount() == 3);
    recovery.retryNow(now);
    finish(recovery, now);
    // A brief reconnect retains backoff; sustained health resets it.
    recovery.lost("microphone", now + 1);
    assert(recovery.attempts(now + 5000).empty());
    finish(recovery, now + 5001);
    recovery.lost("microphone", now + 10001);
    assert(recovery.attempts(now + 10100).empty());
    finish(recovery, now + 10101);
}
void fullGraphRecovery() {
    std::vector<RecoveryNode> nodes{{"broker", {}}, {"driver", {"broker"}}};
    for (unsigned i = 0; i < 32; ++i) {
        const auto id = std::to_string(i);
        nodes.push_back({"device-" + id, {"driver"}});
        nodes.push_back({"render-" + id, {"device-" + id}});
        nodes.push_back({"app-" + id, {}});
        nodes.push_back({"hardware-" + id, {}});
        nodes.push_back({"monitor-" + id, {"render-" + id}});
    }
    GraphRecovery recovery;
    recovery.setDesired(nodes, 0);
    finish(recovery, 0);
    for (uint64_t cycle = 1; cycle <= 100; ++cycle) {
        const auto now = cycle * 10000;
        // Simulate service crashes, driver isolation loss, and sleep/wake.
        recovery.lost(cycle % 2 ? "broker" : "driver", now);
        assert(recovery.ready("hardware-31") && recovery.ready("app-0"));
        assert(!recovery.ready("monitor-31") && !recovery.ready("device-0"));
        recovery.retryNow(now);
        finish(recovery, now);
        assert(recovery.desiredCount() == nodes.size());
    }
}
void ownerRestartAndDisconnect() {
    GraphRecovery recovery;
    recovery.setDesired({{"driver", {}}, {"device", {"driver"}}}, 0);
    const auto stale = recovery.attempts(0).at(0);
    recovery.restart(1);
    const auto current = recovery.attempts(1).at(0);
    assert(current.token != stale.token && !recovery.complete(stale, true, 1));
    assert(recovery.complete(current, true, 1));
    finish(recovery, 1);
    recovery.disconnected(2);
    assert(!recovery.ready("driver") && !recovery.ready("device"));
    assert(recovery.desiredCount() == 2 && recovery.attempts(101).empty());
    finish(recovery, 102);
    recovery.disconnected(103);
    assert(recovery.attempts(302).empty());
    recovery.restart(104);
    finish(recovery, 104);
}
void invalidIntentPreservesGraph() {
    GraphRecovery recovery;
    recovery.setDesired({{"valid", {}}}, 0);
    finish(recovery, 0);
    for (const auto& nodes : std::vector<std::vector<RecoveryNode>>{
             {{"a", {"missing"}}}, {{"a", {"b"}}, {"b", {"a"}}}, {{"a", {}}, {"a", {}}}}) {
        bool rejected = false;
        try {
            recovery.setDesired(nodes, 0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected && recovery.ready("valid") && recovery.desiredCount() == 1);
    }
}
} // namespace
int main() {
    lossAndRestore();
    staleWorkAndTimeouts();
    unavailableForeverAndFlapping();
    fullGraphRecovery();
    ownerRestartAndDisconnect();
    invalidIntentPreservesGraph();
    std::puts("Recovery graph tests passed");
}
