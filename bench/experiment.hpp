// The parts of the Sec. 5 experiment that decide what it reports: the
// configuration, the space budget of every algorithm, and the scoring of answers
// against exact frequencies. They live in a header so that
// tests/test_experiment.cpp can check them.
#pragma once

#include <space_saving/common.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

using Item = std::uint32_t;
using Freq = std::vector<ss::Count>;  // exact frequency, indexed by item

inline constexpr double delta = 0.01;  // $\delta$: failure probability of the sketches
inline constexpr std::size_t count_sketch_factor = 16;

struct Config {
    std::uint64_t n = 10'000'000;
    Item alphabet = 1'000'000;
    double phi = 1e-2;
    std::size_t k = 100;
    std::uint64_t seed = 2005;
    std::optional<std::size_t> topk_m;
};

/// The skews of Figs. 5 and 6: $\alpha = 0, 0.5, \dots, 3$.
inline auto alphas() {
    return std::views::iota(0, 7) | std::views::transform([](int i) { return i * 0.5; });
}

// ---------------------------------------------------------------- budgets
struct FrequentBudget {
    std::size_t space_saving;  ///< $m = \min(|A|, (1/\varepsilon)^{1/\alpha}, 1/\varepsilon)$ (Theorem 4)
    std::size_t frequent;      ///< $m = 1/\phi$
    std::size_t tests;         ///< GroupTest: $\lceil \ln\frac{1}{\delta\phi} \rceil$ tests ...
    std::size_t groups;        ///< ... of $2/\phi$ groups ...
    int id_bits;               ///< ... of $1 + \lceil \log_2 |A| \rceil$ counters
};

struct TopKBudget {
    std::size_t space_saving;  ///< $m$ (Theorem 6, or `--topk-m`), also used by Probabilistic-InPlace
    std::size_t rows;          ///< CountSketch: $\lceil \ln(N/\delta) \rceil$ rows ...
    std::size_t columns;       ///< ... of $16k$ columns
    std::size_t round;         ///< Probabilistic-InPlace: rounds of $\lceil \sqrt{mN/2} \rceil$ elements
};

inline std::size_t capped(double m, Item alphabet) {
    return static_cast<std::size_t>(std::min(std::ceil(m), static_cast<double>(alphabet)));
}

/// Pre: `check(cfg)` succeeded.
inline FrequentBudget frequent_budget(const Config& cfg, double alpha) {
    const double inv_eps = 100.0 / cfg.phi;
    return {
        .space_saving = capped(alpha >= 1 ? std::pow(inv_eps, 1 / alpha) : inv_eps, cfg.alphabet),
        .frequent = static_cast<std::size_t>(std::ceil(1 / cfg.phi)),
        .tests = static_cast<std::size_t>(std::ceil(std::log(1 / (delta * cfg.phi)))),
        .groups = static_cast<std::size_t>(std::ceil(2 / cfg.phi)),
        .id_bits = std::max(1, std::bit_width(cfg.alphabet - 1)),
    };
}

/// $m = \min(|A|, (k/\alpha)^{1/\alpha} k)$ for $\alpha > 1$, $\min(|A|, k^2 \ln|A|)$ otherwise.
/// Pre: `check(cfg)` succeeded, except for the budget checks themselves.
inline TopKBudget top_k_budget(const Config& cfg, double alpha) {
    const auto k = static_cast<double>(cfg.k);
    const auto n = static_cast<double>(cfg.n);
    const auto m = cfg.topk_m.value_or(capped(
        alpha > 1 ? std::pow(k / alpha, 1 / alpha) * k : k * k * std::log(cfg.alphabet), cfg.alphabet));
    return {
        .space_saving = m,
        .rows = static_cast<std::size_t>(std::ceil(std::log(n / delta))),
        .columns = count_sketch_factor * cfg.k,
        .round = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(m) * n / 2))),
    };
}

/// Rejects a configuration that is out of range, or that leaves an algorithm
/// without a usable budget at some $\alpha$.
inline std::expected<void, std::string> check(const Config& cfg) {
    const auto fail = [](std::string why) { return std::unexpected{std::move(why)}; };
    constexpr double hash_range = 0x1p32;  // MultiplyAdd maps onto $[0, n)$ only for $n < 2^{32}$

    if (cfg.n == 0) return fail("--n must be at least 1");
    if (cfg.alphabet == 0 || cfg.alphabet == std::numeric_limits<Item>::max())
        return fail("--alphabet must be in [1, 2^32 - 2]");
    if (!(cfg.phi > 0 && cfg.phi < 1 && 2 / cfg.phi < hash_range))  // also rejects NaN
        return fail("--phi must be in (2^-31, 1)");
    if (cfg.k == 0 || cfg.k > cfg.alphabet || static_cast<double>(count_sketch_factor * cfg.k) >= hash_range)
        return fail("--k must be in [1, min(alphabet, 2^28 - 1)]");
    for (const double alpha : alphas()) {
        // Probabilistic-InPlace keeps $m/2$ candidates, so it needs $m \ge 2$.
        if (const auto m = top_k_budget(cfg, alpha).space_saving; m < 2)
            return fail(std::format("the top-k budget at alpha = {} is m = {}, but Probabilistic-InPlace needs "
                                    "m >= 2; use a larger --k or --topk-m >= 2",
                                    alpha, m));
    }
    return {};
}

/// Parses all of `text` into `out`; false on malformed input.
template <class T>
bool assign(T& out, std::string_view text) {
    const auto* const last = std::to_address(text.end());
    const auto result = std::from_chars(std::to_address(text.begin()), last, out);
    return result && result.ptr == last;  // C++26: from_chars_result is testable
}

/// Parses `--key=value` arguments (without the program name) and checks the result.
inline std::expected<Config, std::string> parse_args(std::span<const std::string_view> args) {
    Config cfg;
    for (const std::string_view arg : args) {
        const auto eq = arg.find('=');
        const auto key = arg.substr(0, eq);
        const auto val = eq == std::string_view::npos ? std::string_view{} : arg.substr(eq + 1);

        bool ok = false;
        if (key == "--n") ok = assign(cfg.n, val);
        else if (key == "--alphabet") ok = assign(cfg.alphabet, val);
        else if (key == "--phi") ok = assign(cfg.phi, val);
        else if (key == "--k") ok = assign(cfg.k, val);
        else if (key == "--seed") ok = assign(cfg.seed, val);
        else if (key == "--topk-m") ok = assign(cfg.topk_m.emplace(), val);
        if (!ok) return std::unexpected{std::format("invalid argument '{}'", arg)};
    }
    return check(cfg).transform([&] { return cfg; });
}

// ---------------------------------------------------------------- scoring
struct Score {
    double precision;
    double recall;
    std::optional<bool> exact = std::nullopt;  ///< top-k only, for answers of exactly $k$ items
};

inline double ratio(std::size_t a, std::size_t b) {
    return b == 0 ? 1.0 : static_cast<double>(a) / static_cast<double>(b);
}

inline std::size_t count_items(std::span<const Item> items, auto pred) {
    return static_cast<std::size_t>(std::ranges::count_if(items, pred));
}

/// Frequent elements: the correct answer $\{x : f_x > \phi N\}$ is unique.
/// `answer` lists distinct items.
inline Score score_frequent(std::span<const Item> answer, const Freq& freq, double support) {
    const auto frequent = [&](ss::Count f) { return static_cast<double>(f) > support; };
    const auto hits = count_items(answer, [&](Item x) { return frequent(freq[x]); });
    const auto relevant = static_cast<std::size_t>(std::ranges::count_if(freq, frequent));
    return {.precision = ratio(hits, answer.size()), .recall = ratio(hits, relevant)};
}

/// $F_k$, the $k$-th largest frequency.  Pre: $1 \le k \le |A|$.
inline ss::Count kth_frequency(const Freq& freq, std::size_t k) {
    auto ranked = freq;
    std::ranges::nth_element(ranked, ranked.begin() + static_cast<std::ptrdiff_t>(k - 1), std::greater{});
    return ranked[k - 1];
}

/// Independent check that `answer` is a correct top-k: $k$ distinct items, and no
/// item left out is more frequent than the least frequent item returned.
inline bool is_exact_top_k(std::span<const Item> answer, const Freq& freq, std::size_t k) {
    if (answer.size() != k) return false;
    if (answer.empty()) return true;
    std::vector<bool> inside(freq.size());
    for (const Item x : answer) {
        if (inside[x]) return false;
        inside[x] = true;
    }
    const ss::Count least = std::ranges::min(answer | std::views::transform([&](Item x) { return freq[x]; }));
    return std::ranges::none_of(std::views::zip(freq, inside), [least](const auto& p) {
        const auto [f, in] = p;
        return !in && f > least;
    });
}

/// Top-k. With ties at $F_k$ several sets are correct: every item with $f > F_k$
/// is required, and the remaining slots may go to any items with $f = F_k$. An
/// answer is credited with the required items it contains, plus tied items up to
/// the number of remaining slots:
///   $\mathrm{precision} = \mathrm{credited} / |\mathrm{answer}|$, $\mathrm{recall} = \mathrm{credited} / k$.
/// Both are 1 exactly when the answer is a correct top-k. `answer` lists distinct items.
/// Pre: $1 \le k \le |A|$.
inline Score score_top_k(std::span<const Item> answer, const Freq& freq, std::size_t k) {
    const ss::Count f_k = kth_frequency(freq, k);
    const auto required = static_cast<std::size_t>(std::ranges::count_if(freq, [&](ss::Count f) { return f > f_k; }));
    const auto required_hits = count_items(answer, [&](Item x) { return freq[x] > f_k; });
    const auto tied_hits = count_items(answer, [&](Item x) { return freq[x] == f_k; });
    const auto credited = required_hits + std::min(tied_hits, k - required);
    return {
        .precision = ratio(credited, answer.size()),
        .recall = ratio(credited, k),
        .exact = answer.size() == k ? std::optional{is_exact_top_k(answer, freq, k)} : std::nullopt,
    };
}

}  // namespace bench
