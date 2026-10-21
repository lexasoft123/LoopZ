#include <loopz/edit_history.hpp>

#include <cassert>
#include <stdexcept>
#include <string>
#include <type_traits>

using loopz::EditHistory;

template <typename Operation> void rejects(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

int main() {
    static_assert(noexcept(std::declval<EditHistory&>().swap(std::declval<EditHistory&>())));
    EditHistory history;
    assert(history.empty() && !history.canUndo("A") && !history.canRedo("A"));
    auto prepared = history.withEdit("A", "B");
    assert(history.empty()); // A failed durable save discards this preparation.
    history.swap(prepared);
    assert(history.canUndo("B") && !history.canRedo("B"));
    auto undo = history.withUndo("B");
    assert(history.current() == "B" && undo.current() == "A");
    assert(undo.canRedo("A") && !undo.canUndo("A"));
    assert(undo.withRedo("A").current() == "B");
    rejects([&] {
        (void)history.withUndo("external");
    });
    rejects([&] {
        (void)undo.withRedo("external");
    });
    auto branch = undo.withEdit("A", "C");
    assert(branch.canUndo("C") && !branch.canRedo("C"));
    assert(branch.withUndo("C").current() == "A");
    assert(undo.withEdit("A", "A").canRedo("A")); // No-op preserves redo.
    auto external = history.withEdit("external", "next");
    assert(external.withUndo("next").current() == "external");
    assert(!external.withUndo("next").canUndo("external"));

    EditHistory bounded({2, 12});
    bounded = bounded.withEdit("aaa", "bbb").withEdit("bbb", "ccc").withEdit("ccc", "ddd");
    assert(bounded.payloadBytes() == 9);
    auto oldest = bounded.withUndo("ddd").withUndo("ccc");
    assert(oldest.current() == "bbb" && !oldest.canUndo("bbb"));
    auto byteBound = bounded.withEdit("ddd", "123456789");
    assert(byteBound.payloadBytes() == 12);
    assert(byteBound.withUndo("123456789").current() == "ddd");
    assert(!byteBound.withUndo("123456789").canUndo("ddd"));
    auto oversize = bounded.withEdit("ddd", std::string(13, 'x'));
    assert(oversize.empty());
    auto backInBudget = oversize.withEdit(std::string(13, 'x'), "small");
    assert(backInBudget.current() == "small" && !backInBudget.canUndo("small"));
    assert(EditHistory().withEdit("", "first").withUndo("first").matches(""));
    rejects([] {
        EditHistory invalid({0, 1});
    });
    rejects([] {
        EditHistory invalid({33, 1});
    });
    rejects([] {
        EditHistory invalid({1, 0});
    });
}
