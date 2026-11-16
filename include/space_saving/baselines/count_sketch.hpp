// CountSketch (Charikar, Chen, Farach-Colton, ICALP 2002 [3]) - the sketch-based
// top-k baseline of Fig. 6.
//
// $t$ rows of $b$ signed counters. Row $i$ adds $s_i(e) \in \{-1, +1\}$ to $C[i][h_i(e)]$;
// $f_e$ is estimated by $\operatorname{median}_i s_i(e)\, C[i][h_i(e)]$. Alongside the sketch, the
// $k$ elements with the largest estimates seen so far are kept as the answer.
#pragma once

#include "../common.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ss::baseline {

template <std::totally_ordered Key, HashFor<Key> Hash = std::hash<Key>>
class CountSketch {
public:
    CountSketch(std::size_t rows, std::size_t width, std::size_t k, std::uint64_t seed)
        : width_{width}, k_{k}, counts_(rows * width), scratch_(rows) {
        if (rows == 0 || width == 0 || k == 0) throw std::invalid_argument{"CountSketch: bad parameters"};
        std::mt19937_64 rng{seed};
        hashes_ = std::views::iota(0UZ, rows) | std::views::transform([&](auto) {
                      return RowHash{.bucket = detail::MultiplyAdd::random(rng), .sign = detail::MultiplyAdd::random(rng)};
                  }) |
                  std::ranges::to<std::vector>();
    }

    void insert(const Key& e) {
        const std::uint64_t x = hash_(e);
        for (auto&& [row, h] : std::views::zip(rows(), hashes_)) row[h.index(x, width_)] += h.sign_of(x);
        track(e, estimate(x, scratch_));
    }

    [[nodiscard]] std::int64_t estimate(const Key& e) const {
        std::vector<std::int64_t> buffer(hashes_.size());
        return estimate(hash_(e), buffer);
    }

    /// The tracked candidates, largest estimate first.
    [[nodiscard]] std::vector<Key> top_k() const {
        return by_estimate_ | std::views::reverse | std::views::values | std::ranges::to<std::vector>();
    }

    [[nodiscard]] std::size_t counters() const noexcept { return counts_.size(); }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        constexpr std::size_t tree_node = sizeof(std::pair<std::int64_t, Key>) + 4 * sizeof(void*);
        return counts_.size() * sizeof(std::int64_t) + hashes_.size() * sizeof(RowHash) +
               by_estimate_.size() * tree_node + detail::hash_map_bytes(estimate_of_);
    }

private:
    struct RowHash {
        detail::MultiplyAdd bucket, sign;

        [[nodiscard]] std::ptrdiff_t index(std::uint64_t x, std::size_t width) const noexcept {
            return static_cast<std::ptrdiff_t>(bucket(x, width));
        }
        [[nodiscard]] std::int64_t sign_of(std::uint64_t x) const noexcept { return (sign(x) >> 63U) != 0 ? 1 : -1; }
    };

    /// The counters viewed row by row.
    auto rows(this auto& self) { return self.counts_ | std::views::chunk(self.width_); }

    /// $\operatorname{median}_i s_i(x)\, C[i][h_i(x)]$, using `buffer` (one slot per row) as scratch.
    std::int64_t estimate(std::uint64_t x, std::span<std::int64_t> buffer) const {
        for (auto&& [out, row, h] : std::views::zip(buffer, rows(), hashes_)) out = h.sign_of(x) * row[h.index(x, width_)];
        const auto median = buffer.begin() + std::ssize(buffer) / 2;
        std::ranges::nth_element(buffer, median);
        return *median;
    }

    /// Keeps the $k$ elements with the largest current estimates.
    void track(const Key& e, std::int64_t estimate) {
        if (const auto it = estimate_of_.find(e); it != estimate_of_.end()) {
            by_estimate_.erase({it->second, e});
            it->second = estimate;
        } else if (estimate_of_.size() < k_) {
            estimate_of_.emplace(e, estimate);
        } else if (estimate > by_estimate_.begin()->first) {
            estimate_of_.erase(by_estimate_.begin()->second);
            by_estimate_.erase(by_estimate_.begin());
            estimate_of_.emplace(e, estimate);
        } else {
            return;
        }
        by_estimate_.emplace(estimate, e);
    }

    std::size_t width_;
    std::size_t k_;
    std::vector<std::int64_t> counts_;  // $t \times b$, row-major
    std::vector<std::int64_t> scratch_;
    std::vector<RowHash> hashes_;
    [[no_unique_address]] Hash hash_;
    std::set<std::pair<std::int64_t, Key>> by_estimate_;
    std::unordered_map<Key, std::int64_t, Hash> estimate_of_;
};

}  // namespace ss::baseline
