// GroupTest (Cormode & Muthukrishnan, PODS 2003 [5]) - the sketch-based
// frequent-elements baseline of Fig. 5.
//
// Non-adaptive group testing for hot items ($f > \phi N$). Each of $t$ tests hashes
// items into $b$ groups; a group keeps $1 + L$ counters: its total, and for every
// bit $j$ of the id the total of the items whose bit $j$ is 1 (FindMajority). A
// group whose total exceeds $\phi N$ recovers its hot item bit by bit; the
// recovered id is reported if it hashes back to that group and all of its $t$
// groups exceed $\phi N$.
//
// Ids must be integers in $[0, 2^L)$ - the "IDs are $1, \dots, |A|$" assumption the paper
// criticises in Sec. 4.1.
#pragma once

#include "../common.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

namespace ss::baseline {

template <std::unsigned_integral Key>
class GroupTest {
public:
    /// @param tests $t$ (independent repetitions), @param groups $b$, @param id_bits $L$.
    GroupTest(std::size_t tests, std::size_t groups, int id_bits, std::uint64_t seed)
        : groups_{groups}, bits_{static_cast<std::size_t>(id_bits)}, counts_(tests * groups * (bits_ + 1)),
          hashes_(tests) {
        if (tests == 0 || groups == 0 || id_bits <= 0 || id_bits > std::numeric_limits<Key>::digits)
            throw std::invalid_argument{"GroupTest: bad parameters"};
        std::mt19937_64 rng{seed};
        std::ranges::generate(hashes_, [&] { return detail::MultiplyAdd::random(rng); });
    }

    void insert(Key e) {
        assert(std::bit_width(e) <= static_cast<int>(bits_));
        ++n_;
        for (std::size_t t = 0; t < hashes_.size(); ++t) {
            const auto g = group(t, hashes_[t](e, groups_));
            ++g[0];
            for (Key rest = e; rest != 0; rest &= rest - 1) ++g[1 + static_cast<std::size_t>(std::countr_zero(rest))];
        }
    }

    /// Items reported as having frequency $> \phi N$.
    [[nodiscard]] std::vector<Key> hot_items(double phi) const {
        const double threshold = phi * static_cast<double>(n_);
        const auto above = [threshold](Count c) { return static_cast<double>(c) > threshold; };
        const auto passes_all_tests = [&](Key x) {
            return std::ranges::all_of(std::views::iota(0UZ, hashes_.size()),
                                       [&](std::size_t t) { return above(group(t, hashes_[t](x, groups_))[0]); });
        };

        std::vector<Key> hot;
        for (std::size_t t = 0; t < hashes_.size(); ++t) {
            for (std::size_t b = 0; b < groups_; ++b) {
                const auto g = group(t, b);
                if (!above(g[0])) continue;
                const auto x = majority(g, above);
                if (x && hashes_[t](*x, groups_) == b && !std::ranges::contains(hot, *x) && passes_all_tests(*x))
                    hot.push_back(*x);
            }
        }
        return hot;
    }

    [[nodiscard]] std::size_t counters() const noexcept { return counts_.size(); }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return counts_.size() * sizeof(Count) + hashes_.size() * sizeof(detail::MultiplyAdd);
    }

private:
    /// Group $b$ of test $t$: $[\,c_\mathrm{total}, c_0, \dots, c_{L-1}\,]$, $c_j$ = hits of items whose bit $j$ is 1.
    auto group(this auto& self, std::size_t t, std::size_t b) {
        const std::size_t width = self.bits_ + 1;
        return std::span{self.counts_}.subspan((t * self.groups_ + b) * width, width);
    }

    /// FindMajority: the id of the single item with $> \phi N$ hits in the group, if any.
    [[nodiscard]] std::optional<Key> majority(std::span<const Count> g, auto above) const {
        Key x = 0;
        for (std::size_t j = 0; j < bits_; ++j) {
            const bool one = above(g[j + 1]);
            const bool zero = above(g[0] - g[j + 1]);
            if (one == zero) return std::nullopt;  // two hot items collide, or none dominates
            if (one) x |= static_cast<Key>(Key{1} << j);
        }
        return x;
    }

    std::size_t groups_;
    std::size_t bits_;
    Count n_ = 0;
    std::vector<Count> counts_;  // $t \times b \times (1 + L)$
    std::vector<detail::MultiplyAdd> hashes_;  // one group hash per test
};

}  // namespace ss::baseline
