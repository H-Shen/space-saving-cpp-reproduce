// Exact top-k, the textbook way: a counter for every distinct element, plus two
// heaps that keep the current top-k apart from the rest, so the exact answer is
// ready after every arrival. Not from the paper; it shows what exactness costs.
//
// `top` is a min-heap of the $k$ largest counts, `rest` a max-heap of all other
// counts, and every count in `top` is $\ge$ every count in `rest`. The hash map
// remembers each element's heap and position, so a $+1$ is one sift in one heap
// plus at most one swap of the two roots: $O(\log d)$ per arrival, $\Theta(d)$
// memory, for $d$ distinct elements.
#pragma once

#include "../common.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ss::baseline {

template <std::copyable Key, HashFor<Key> Hash = std::hash<Key>,
          std::equivalence_relation<Key, Key> KeyEqual = std::equal_to<Key>>
class ExactTopK {
public:
    explicit ExactTopK(std::size_t k) : k_{k} {
        if (k == 0) throw std::invalid_argument{"ExactTopK: k must be > 0"};
    }

    // The heaps point into the hash map's nodes, which a copy would not update.
    ExactTopK(const ExactTopK&) = delete;
    ExactTopK& operator=(const ExactTopK&) = delete;
    ExactTopK(ExactTopK&&) noexcept = default;  // map nodes change owner, pointers stay valid
    ExactTopK& operator=(ExactTopK&&) noexcept = default;
    ~ExactTopK() = default;

    void insert(const Key& e) {
        const auto [it, fresh] = counts_.try_emplace(e, Slot{});
        Node* const node = &*it;
        ++node->second.count;
        if (fresh) {
            node->second.in_top = top_.size() < k_;
            push(node);
        } else if (node->second.in_top) {
            sift_down(top_, node->second.pos, true);  // a larger count sinks in the min-heap
        } else {
            sift_up(rest_, node->second.pos, false);  // and rises in the max-heap
        }
        rebalance();
    }

    /// The exact top-$k$, largest count first (fewer if fewer distinct elements were seen).
    [[nodiscard]] std::vector<Key> top_k() const {
        auto top = top_;
        std::ranges::sort(top, std::greater{}, [](const Node* n) { return n->second.count; });
        return top | std::views::transform([](const Node* n) { return n->first; }) | std::ranges::to<std::vector>();
    }

    [[nodiscard]] Count count(const Key& e) const {
        const auto it = counts_.find(e);
        return it == counts_.end() ? 0 : it->second.count;
    }

    /// One counter per distinct element seen.
    [[nodiscard]] std::size_t counters() const noexcept { return counts_.size(); }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return detail::hash_map_bytes(counts_) + (top_.capacity() + rest_.capacity()) * sizeof(Node*);
    }

private:
    struct Slot {
        Count count = 0;
        bool in_top = false;
        std::size_t pos = 0;  ///< index in its heap
    };
    using Map = std::unordered_map<Key, Slot, Hash, KeyEqual>;
    using Node = typename Map::value_type;
    using Heap = std::vector<Node*>;

    [[nodiscard]] static Count count_of(const Node* n) noexcept { return n->second.count; }

    /// Whether `a` belongs above `b`: smaller counts first in the min-heap, larger in the max-heap.
    [[nodiscard]] static bool above(const Node* a, const Node* b, bool min_heap) noexcept {
        return min_heap ? count_of(a) < count_of(b) : count_of(a) > count_of(b);
    }

    void place(Heap& h, std::size_t i, Node* n) noexcept {
        h[i] = n;
        n->second.pos = i;
    }

    void sift_up(Heap& h, std::size_t i, bool min_heap) noexcept {
        Node* const n = h[i];
        while (i > 0 && above(n, h[(i - 1) / 2], min_heap)) {
            place(h, i, h[(i - 1) / 2]);
            i = (i - 1) / 2;
        }
        place(h, i, n);
    }

    void sift_down(Heap& h, std::size_t i, bool min_heap) noexcept {
        Node* const n = h[i];
        for (std::size_t child = (2 * i) + 1; child < h.size(); child = (2 * i) + 1) {
            if (child + 1 < h.size() && above(h[child + 1], h[child], min_heap)) ++child;
            if (!above(h[child], n, min_heap)) break;
            place(h, i, h[child]);
            i = child;
        }
        place(h, i, n);
    }

    void push(Node* n) {
        Heap& h = n->second.in_top ? top_ : rest_;
        h.push_back(n);
        sift_up(h, h.size() - 1, n->second.in_top);
    }

    /// A $+1$ can lift one element of `rest` above the smallest of `top`: swap the two roots.
    void rebalance() noexcept {
        if (top_.empty() || rest_.empty() || count_of(rest_.front()) <= count_of(top_.front())) return;
        Node* const up = rest_.front();
        Node* const down = top_.front();
        up->second.in_top = true;
        down->second.in_top = false;
        place(top_, 0, up);
        place(rest_, 0, down);
        sift_down(top_, 0, true);
        sift_down(rest_, 0, false);
    }

    std::size_t k_;
    Map counts_;
    Heap top_;   // min-heap of the $k$ largest counts
    Heap rest_;  // max-heap of the others
};

}  // namespace ss::baseline
