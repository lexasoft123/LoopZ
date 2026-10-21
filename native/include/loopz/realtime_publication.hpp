#pragma once

#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace loopz {

class RealtimePublicationBusy final : public std::runtime_error {
  public:
    RealtimePublicationBusy()
        : std::runtime_error("Waiting for audio readers to release older graphs") {}
};

// One control owner prepares/publishes/reclaims; readers borrow without locks,
// allocation or shared ownership. A serial audio reader may mutate T. Other
// readers must only use T's explicitly thread-safe observation methods.
// The owner must outlive its updates and join every reader before destruction.
// Bounded retirement makes a stuck reader apply backpressure before a backend
// transaction starts, rather than allocating or destroying on the audio thread.
template <typename T> class RealtimePublication {
    struct Node {
        explicit Node(std::unique_ptr<T> item) : value(std::move(item)) {}
        const std::unique_ptr<T> value;
        std::atomic<uint64_t> readers{0};
    };

  public:
    class Borrow {
      public:
        Borrow(const Borrow&) = delete;
        Borrow& operator=(const Borrow&) = delete;
        Borrow(Borrow&& other) noexcept : node_(std::exchange(other.node_, nullptr)) {}
        ~Borrow() {
            if (node_)
                node_->readers.fetch_sub(1);
        }
        T* get() const noexcept {
            return node_ ? node_->value.get() : nullptr;
        }
        T* operator->() const noexcept {
            return get();
        }
        explicit operator bool() const noexcept {
            return get() != nullptr;
        }

      private:
        friend class RealtimePublication;
        explicit Borrow(RealtimePublication& owner) noexcept {
            // The short entry guard protects the loaded node until its own
            // borrower count is pinned. Sequential consistency pairs entry
            // with publication and collect(): a reader that could see a retired
            // node is either entering or already counted on that exact node.
            owner.entering_.fetch_add(1);
            node_ = owner.visible_.load();
            if (node_)
                node_->readers.fetch_add(1);
            owner.entering_.fetch_sub(1);
        }
        Node* node_{nullptr};
    };
    class Update {
      public:
        Update(const Update&) = delete;
        Update& operator=(const Update&) = delete;
        ~Update() {
            if (ownsStage_)
                owner_.staged_ = false;
        }
        // The staged slot guarantees capacity. No allocation or destruction of
        // a previous T occurs here, even with a reader holding the old graph.
        void commit() noexcept {
            if (committed_)
                return;
            auto previous = std::move(owner_.current_);
            owner_.current_ = std::move(next_);
            owner_.visible_.store(owner_.current_.get());
            if (previous)
                owner_.retired_.push_back(std::move(previous));
            committed_ = true;
            owner_.staged_ = false;
            ownsStage_ = false;
        }

      private:
        friend class RealtimePublication;
        Update(RealtimePublication& owner, std::unique_ptr<T> next)
            : owner_(owner), next_(next ? std::make_unique<Node>(std::move(next)) : nullptr) {}
        RealtimePublication& owner_;
        std::unique_ptr<Node> next_;
        bool committed_{false}, ownsStage_{true};
    };

    explicit RealtimePublication(size_t retirementLimit = 8) {
        if (!retirementLimit)
            throw std::invalid_argument("A publication needs a retirement slot");
        retired_.reserve(retirementLimit);
        limit_ = retirementLimit;
    }
    ~RealtimePublication() {
        assert(!staged_ && entering_.load() == 0);
        assert(!current_ || current_->readers.load() == 0);
        assert(std::all_of(retired_.begin(), retired_.end(), [](const auto& node) {
            return node->readers.load() == 0;
        }));
    }
    RealtimePublication(const RealtimePublication&) = delete;
    RealtimePublication& operator=(const RealtimePublication&) = delete;

    Borrow borrow() noexcept {
        return Borrow(*this);
    }
    std::unique_ptr<Update> stage(std::unique_ptr<T> next) {
        checkStageCapacity();
        auto update = std::unique_ptr<Update>(new Update(*this, std::move(next)));
        staged_ = true;
        return update;
    }
    // Control-owner preflight before constructing an expensive candidate. This
    // does not reserve a slot; stage() checks again. Readers cannot increase the
    // retired count, and no second control owner may prepare/commit concurrently.
    void checkStageCapacity() {
        if (staged_)
            throw std::logic_error("A publication update is already staged");
        collect();
        if (current_ && retired_.size() == limit_)
            throw RealtimePublicationBusy();
    }
    // Control-domain reclamation only. Current-version borrowers must not keep
    // unreferenced older versions alive indefinitely. The entry guard spans
    // only pointer acquisition, never rendering or observation of T.
    void collect() noexcept {
        if (!entering_.load())
            std::erase_if(retired_, [](const auto& node) {
                return !node->readers.load();
            });
    }

  private:
    std::atomic<uint64_t> entering_{0};
    std::atomic<Node*> visible_{nullptr};
    std::unique_ptr<Node> current_;
    std::vector<std::unique_ptr<Node>> retired_;
    size_t limit_;
    bool staged_{false};
};
} // namespace loopz
