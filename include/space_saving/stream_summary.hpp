// Stream-Summary (Metwally et al., ICDT 2005, Sec. 3.1).
//
// Counters with equal value hang off one bucket; buckets form a list sorted by
// value. Every update is a constant number of $O(1)$ list splices, and a hash
// index gives $\Theta(1)$ lookup of a monitored element.
#pragma once

#include "common.hpp"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <generator>
#include <iterator>
#include <list>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace ss {

/// A monitored element: its true frequency $f$ satisfies $\mathrm{count} - \varepsilon \le f \le \mathrm{count}$.
template <class Key>
struct Entry {
    Key item;
    Count count;  ///< $\mathrm{count}_i$: over-estimated hits
    Count error;  ///< $\varepsilon_i$: over-estimation inherited when the element was inserted

    /// $\mathrm{count}_i - \varepsilon_i$: guaranteed hits, a lower bound on $f_i$.
    [[nodiscard]] constexpr Count guaranteed() const noexcept { return count - error; }
    friend constexpr bool operator==(const Entry&, const Entry&) = default;
};

template <std::copyable Key, HashFor<Key> Hash = std::hash<Key>,
          std::equivalence_relation<Key, Key> KeyEqual = std::equal_to<Key>>
class StreamSummary {
public:
    explicit StreamSummary(std::size_t capacity) : capacity_{capacity} {
        if (capacity == 0) throw std::invalid_argument{"StreamSummary: capacity must be > 0"};
        index_.reserve(capacity);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] bool full() const noexcept { return size() == capacity_; }

    /// The smallest counter value; unused counters hold 0.
    [[nodiscard]] Count min_count() const noexcept { return full() ? buckets_.front().value : 0; }

    [[nodiscard]] std::optional<Entry<Key>> find(const Key& key) const {
        const auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        const auto& [bucket, node] = it->second;
        return Entry<Key>{node->item, bucket->value, node->error};
    }

    /// Increments the counter of `key`; returns false if `key` is not monitored.
    bool try_increment(const Key& key) {
        const auto it = index_.find(key);
        if (it == index_.end()) return false;
        increment(it->second);
        return true;
    }

    /// Puts `key` into an unused counter: $\mathrm{count} = 1$, $\varepsilon = 0$.  Pre: `!full()`.
    void add(const Key& key) {
        assert(!full() && !index_.contains(key));
        if (buckets_.empty() || buckets_.front().value != 1) buckets_.emplace_front(1);
        auto& children = buckets_.front().children;
        children.emplace_front(key, 0);
        index_.emplace(key, Slot{buckets_.begin(), children.begin()});
    }

    /// Replaces an element with the least hits by `key`: $\mathrm{count} = \mathit{min} + 1$, $\varepsilon = \mathit{min}$.
    /// Pre: `full()`.
    void replace_min(const Key& key) {
        assert(full() && !index_.contains(key));
        const auto bucket = buckets_.begin();
        const auto node = bucket->children.begin();
        auto handle = index_.extract(node->item);  // re-key the hash node: no allocation
        handle.key() = key;
        *node = Node{key, bucket->value};
        increment(index_.insert(std::move(handle)).position->second);
    }

    /// Sequential traversal as a sorted list, largest count first.
    [[nodiscard]] std::generator<Entry<Key>> descending() const {
        for (const auto& [value, children] : buckets_ | std::views::reverse)
            for (const auto& [item, error] : children) co_yield Entry<Key>{item, value, error};
    }

    /// Approximate footprint in bytes (list nodes + hash index).
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        constexpr std::size_t list_links = 2 * sizeof(void*);
        return buckets_.size() * (sizeof(Bucket) + list_links) + size() * (sizeof(Node) + list_links) +
               detail::hash_map_bytes(index_);
    }

    /// Checks the structural invariants: buckets non-empty and strictly increasing,
    /// index consistent, and $\varepsilon_i < \mathrm{count}_i$, $\varepsilon_i \le \mathit{min}$ for every counter.
    [[nodiscard]] std::expected<void, std::string> validate() const {
        const auto fail = [](std::string why) { return std::unexpected{std::move(why)}; };
        if (std::ranges::adjacent_find(buckets_, std::greater_equal{}, &Bucket::value) != buckets_.end())
            return fail("buckets not strictly increasing");
        std::size_t seen = 0;
        for (auto b = buckets_.begin(); b != buckets_.end(); ++b) {
            if (b->children.empty()) return fail("empty bucket");
            for (auto n = b->children.begin(); n != b->children.end(); ++n, ++seen) {
                const auto it = index_.find(n->item);
                if (it == index_.end() || it->second.bucket != b || it->second.node != n)
                    return fail("stale index entry");
                if (n->error >= b->value || n->error > min_count()) return fail("bad epsilon");
            }
        }
        if (seen != size()) return fail("unreachable counters");
        return {};
    }

private:
    struct Node {
        Key item;
        Count error;
    };
    struct Bucket {
        Count value;
        std::list<Node> children;
    };
    using BucketIt = typename std::list<Bucket>::iterator;
    using NodeIt = typename std::list<Node>::iterator;
    struct Slot {
        BucketIt bucket;
        NodeIt node;
    };

    /// Moves the counter to the neighbouring bucket of value $v + 1$, creating that
    /// bucket if needed and deleting the old one if it becomes empty.
    void increment(Slot& slot) {
        const auto old = slot.bucket;
        const Count value = old->value + 1;
        auto next = std::next(old);
        const bool next_matches = next != buckets_.end() && next->value == value;

        if (!next_matches && old->children.size() == 1) {  // sole child: relabel in place
            old->value = value;
            return;
        }
        if (!next_matches) next = buckets_.emplace(next, value);
        next->children.splice(next->children.begin(), old->children, slot.node);
        slot.bucket = next;
        if (old->children.empty()) buckets_.erase(old);
    }

    std::size_t capacity_;
    std::list<Bucket> buckets_;  // ascending by value
    std::unordered_map<Key, Slot, Hash, KeyEqual> index_;
};

}  // namespace ss
