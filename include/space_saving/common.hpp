// Shared vocabulary for Space-Saving and the baselines.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <random>
#include <type_traits>

namespace ss {

using Count = std::uint64_t;

template <class H, class K>
concept HashFor = std::regular_invocable<const H&, const K&> &&
                  std::convertible_to<std::invoke_result_t<const H&, const K&>, std::size_t>;

namespace detail {

/// Approximate footprint of a node-based hash map (nodes + bucket array).
template <class Map>
[[nodiscard]] std::size_t hash_map_bytes(const Map& map) noexcept {
    constexpr std::size_t node = sizeof(typename Map::value_type) + sizeof(void*) + sizeof(std::size_t);
    return map.size() * node + map.bucket_count() * sizeof(void*);
}

/// Multiply-add hash $h(x) = (a x + b) \bmod 2^{64}$ with $a$ odd; its high bits are the well-mixed ones.
struct MultiplyAdd {
    std::uint64_t a, b;

    static MultiplyAdd random(std::mt19937_64& rng) { return {.a = rng() | 1U, .b = rng()}; }

    [[nodiscard]] constexpr std::uint64_t operator()(std::uint64_t x) const noexcept { return a * x + b; }

    /// $h(x)$ mapped onto $[0, n)$ by multiply-high: $\lfloor (h(x) \gg 32) \cdot n / 2^{32} \rfloor$, $n < 2^{32}$.
    [[nodiscard]] constexpr std::size_t operator()(std::uint64_t x, std::size_t n) const noexcept {
        return static_cast<std::size_t>((((*this)(x) >> 32U) * n) >> 32U);
    }
};

}  // namespace detail
}  // namespace ss
