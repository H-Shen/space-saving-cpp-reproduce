// Frequent (Misra-Gries 1982; rediscovered by Demaine et al. 2002 [6] and
// Karp et al. 2003 [10]) - the counter-based baseline of Fig. 5.
//
// Keeps $m$ counters. A monitored arrival increments its counter; an unmonitored
// arrival decrements every counter, and counters reaching 0 are freed for the
// next new element. Counts are *under*-estimates: $f - N/(m+1) \le \mathrm{count} \le f$.
//
// Decrement-all costs $O(m)$ but happens at most $N/(m+1)$ times, so updates are
// $O(1)$ amortised ([6] gives an $O(1)$ worst-case structure with the same output).
#pragma once

#include "../common.hpp"

#include <cstddef>
#include <functional>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ss::baseline {

template <std::copyable Key, HashFor<Key> Hash = std::hash<Key>,
          std::equivalence_relation<Key, Key> KeyEqual = std::equal_to<Key>>
class Frequent {
public:
    explicit Frequent(std::size_t m) : capacity_{m} {
        if (m == 0) throw std::invalid_argument{"Frequent: m must be > 0"};
        counters_.reserve(m);
    }

    void insert(const Key& e) {
        ++n_;
        if (const auto it = counters_.find(e); it != counters_.end()) {
            ++it->second;
        } else if (counters_.size() < capacity_) {
            counters_.emplace(e, 1);
        } else {
            for (auto& count : counters_ | std::views::values) --count;
            std::erase_if(counters_, [](const auto& kv) { return kv.second == 0; });
        }
    }

    [[nodiscard]] Count stream_length() const noexcept { return n_; }

    /// Lower bound on the frequency of `e` (0 if not monitored).
    [[nodiscard]] Count estimate(const Key& e) const {
        const auto it = counters_.find(e);
        return it == counters_.end() ? 0 : it->second;
    }

    /// The query used in the paper: every monitored element is a candidate.
    /// Frequent has no per-counter error, so it cannot filter the candidates.
    [[nodiscard]] std::vector<Key> candidates() const {
        return counters_ | std::views::keys | std::ranges::to<std::vector>();
    }

    [[nodiscard]] std::size_t counters() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t memory_bytes() const noexcept { return detail::hash_map_bytes(counters_); }

private:
    std::size_t capacity_;
    Count n_ = 0;
    std::unordered_map<Key, Count, Hash, KeyEqual> counters_;
};

}  // namespace ss::baseline
