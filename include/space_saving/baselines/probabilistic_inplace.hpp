// Probabilistic-InPlace (Demaine, López-Ortiz, Munro, ESA 2002 [6], Sec. 4.3) -
// the counter-based top-k baseline of Fig. 6.
//
// $m$ counters, half of them reserved for the current best candidates. The stream
// is cut into rounds of $r$ elements; in each round the other $m/2$ counters are
// zeroed and count the first $m/2$ distinct elements of the round (a sample). At
// the end of a round the reserved half is replaced by the top $m/2$ of all $m$
// counters. The answer is the $m/2$ reserved candidates, i.e. it solves
// FindCandidateTop$(S, k, m/2)$. [6] analyses $r = \sqrt{mN/2}$ for randomly ordered streams.
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
class ProbabilisticInPlace {
public:
    ProbabilisticInPlace(std::size_t m, std::size_t round_length) : half_{m / 2}, round_length_{round_length} {
        if (half_ == 0 || round_length == 0) throw std::invalid_argument{"ProbabilisticInPlace: bad parameters"};
        round_.reserve(half_);
    }

    void insert(const Key& e) {
        if (const auto it = round_.find(e); it != round_.end())
            ++it->second;
        else if (round_.size() < half_)
            round_.emplace(e, 1);
        if (++seen_in_round_ == round_length_) end_round();
    }

    /// The $m/2$ candidates, largest per-round count first. A round in progress
    /// takes part as if it ended now.
    [[nodiscard]] std::vector<Key> candidates() const {
        return top_half() | std::views::keys | std::ranges::to<std::vector>();
    }

    [[nodiscard]] std::size_t counters() const noexcept { return 2 * half_; }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return reserved_.capacity() * sizeof(Candidate) + detail::hash_map_bytes(round_);
    }

private:
    using Candidate = std::pair<Key, Count>;

    /// Top $m/2$ of reserved and round counters; an element in both keeps its larger count.
    [[nodiscard]] std::vector<Candidate> top_half() const {
        std::unordered_map<Key, Count, Hash, KeyEqual> best;
        for (const auto& [item, count] : std::views::concat(reserved_, round_)) {
            auto& c = best[item];
            c = std::max(c, count);
        }
        auto all = best | std::ranges::to<std::vector<Candidate>>();
        const auto keep = all.begin() + static_cast<std::ptrdiff_t>(std::min(half_, all.size()));
        std::ranges::partial_sort(all, keep, std::greater{}, &Candidate::second);
        all.erase(keep, all.end());
        return all;
    }

    void end_round() {
        reserved_ = top_half();
        round_.clear();
        seen_in_round_ = 0;
    }

    std::size_t half_;
    std::size_t round_length_;
    std::size_t seen_in_round_ = 0;
    std::vector<Candidate> reserved_;
    std::unordered_map<Key, Count, Hash, KeyEqual> round_;
};

}  // namespace ss::baseline
