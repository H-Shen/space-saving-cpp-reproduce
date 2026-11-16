// Sanity checks for the baselines used in the comparison experiments, and an
// exhaustive check of the exact top-k (hash map + two heaps).
#include <space_saving/baselines/count_sketch.hpp>
#include <space_saving/baselines/exact_top_k.hpp>
#include <space_saving/baselines/frequent.hpp>
#include <space_saving/baselines/group_test.hpp>
#include <space_saving/baselines/probabilistic_inplace.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <print>
#include <random>
#include <ranges>
#include <utility>
#include <vector>

namespace {

using Item = std::size_t;

int check(bool ok, std::string_view what) {
    if (!ok) std::println(stderr, "  FAIL: {}", what);
    return ok ? 0 : 1;
}

/// Misra-Gries guarantee on random skewed streams:
/// $f - N/(m+1) \le \hat f \le f$, and every $f > N/(m+1)$ is a candidate.
int frequent_bounds() {
    std::mt19937_64 rng{7};  // NOLINT(bugprone-random-generator-seed): reproducible tests
    int failures = 0;
    for (int trial = 0; trial < 500; ++trial) {
        const auto alphabet = std::uniform_int_distribution<Item>{2, 60}(rng);
        const auto m = std::uniform_int_distribution<std::size_t>{1, 20}(rng);
        const auto n = std::uniform_int_distribution{1, 2000}(rng);
        const auto weights = std::views::iota(Item{1}, alphabet + 1) |
                             std::views::transform([](Item i) { return 1.0 / static_cast<double>(i * i); }) |
                             std::ranges::to<std::vector>();
        std::discrete_distribution<Item> pick(weights.begin(), weights.end());

        ss::baseline::Frequent<Item> sketch{m};
        std::vector<ss::Count> f(alphabet);
        for (int i = 0; i < n; ++i) {
            const Item x = pick(rng);
            sketch.insert(x);
            ++f[x];
        }
        const auto slack = sketch.stream_length() / (m + 1);
        const auto candidates = sketch.candidates();
        for (const auto [x, fx] : std::views::enumerate(f)) {
            const auto item = static_cast<Item>(x);
            const auto est = sketch.estimate(item);
            failures += check(est <= fx && fx <= est + slack, "f - N/(m+1) <= estimate <= f");
            failures += check(fx <= slack || std::ranges::contains(candidates, item), "f > N/(m+1) => candidate");
        }
    }
    return failures;
}

/// Items $0, \dots, 4$ with 1000, 800, 600, 400, 200 hits among 5000 singletons, shuffled ($N = 8000$).
std::vector<Item> heavy_hitter_stream() {
    std::vector<Item> stream;
    for (const auto [item, hits] : std::views::zip(std::views::iota(Item{0}), std::array{1000, 800, 600, 400, 200}))
        stream.insert(stream.end(), static_cast<std::size_t>(hits), item);
    for (Item noise = 100; noise < 5100; ++noise) stream.push_back(noise);
    std::ranges::shuffle(stream, std::mt19937_64{1});  // NOLINT(bugprone-random-generator-seed)
    return stream;
}

int count_sketch_heavy_hitters() {
    ss::baseline::CountSketch<Item> sketch{7, 1024, 5, 42};
    for (const Item x : heavy_hitter_stream()) sketch.insert(x);
    int failures = check(sketch.top_k() == std::vector<Item>{0, 1, 2, 3, 4}, "top-5 heavy hitters in order");
    failures += check(std::abs(sketch.estimate(0) - 1000) < 50, "estimate close to the true count");
    return failures;
}

int group_test_hot_items() {
    ss::baseline::GroupTest<Item> sketch{5, 34, 13, 42};  // ids $< 2^{13}$
    for (const Item x : heavy_hitter_stream()) sketch.insert(x);
    auto hot = sketch.hot_items(0.06);  // $\phi N = 480$: items 0, 1, 2
    std::ranges::sort(hot);
    int failures = check(hot == std::vector<Item>{0, 1, 2}, "exactly the hot items");
    failures += check(sketch.hot_items(0.2).empty(), "no item above 20%");
    return failures;
}

int probabilistic_inplace_top() {
    const auto stream = heavy_hitter_stream();
    const auto round = static_cast<std::size_t>(std::sqrt(20.0 * static_cast<double>(stream.size()) / 2));
    ss::baseline::ProbabilisticInPlace<Item> sketch{20, round};
    for (const Item x : stream) sketch.insert(x);
    auto top = sketch.candidates();
    int failures = check(top.size() == 10, "m/2 candidates");
    top.resize(std::min<std::size_t>(top.size(), 3));
    std::ranges::sort(top);
    failures += check(top == std::vector<Item>{0, 1, 2}, "the 3 heaviest items lead the candidates");
    return failures;
}

/// After every arrival the two heaps hold an exact top-$k$, largest first, and exact
/// counts. Small alphabets, so there are many ties at the cutoff.
int exact_top_k_after_every_insert() {
    std::mt19937_64 rng{11};  // NOLINT(bugprone-random-generator-seed): reproducible tests
    int failures = 0;
    for (int trial = 0; trial < 300; ++trial) {
        const auto alphabet = std::uniform_int_distribution<Item>{1, 40}(rng);
        const auto k = std::uniform_int_distribution<std::size_t>{1, 12}(rng);
        const auto n = std::uniform_int_distribution{1, 400}(rng);
        const auto skew = std::uniform_real_distribution{0.0, 2.0}(rng);
        const auto weights = std::views::iota(Item{1}, alphabet + 1) |
                             std::views::transform([skew](Item i) { return std::pow(static_cast<double>(i), -skew); }) |
                             std::ranges::to<std::vector>();
        std::discrete_distribution<Item> pick(weights.begin(), weights.end());

        ss::baseline::ExactTopK<Item> exact{k};
        std::vector<ss::Count> f(alphabet);
        for (int i = 0; i < n; ++i) {
            const Item x = pick(rng);
            exact.insert(x);
            ++f[x];

            const auto top = exact.top_k();
            const auto seen = static_cast<std::size_t>(std::ranges::count_if(f, [](ss::Count c) { return c > 0; }));
            failures += check(top.size() == std::min(k, seen), "min(k, distinct) elements");
            failures += check(exact.counters() == seen && exact.count(x) == f[x], "exact counts");
            const auto freq = [&](Item y) { return f[y]; };
            failures += check(std::ranges::is_sorted(top, std::greater{}, freq), "largest first");
            const ss::Count least = top.empty() ? 0 : f[top.back()];
            for (const auto [y, fy] : std::views::enumerate(f))
                if (!std::ranges::contains(top, static_cast<Item>(y))) failures += check(fy <= least, "no omitted element is larger");
        }
    }
    return failures;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape): a test may terminate on an unexpected exception
    const std::array tests{
        std::pair{"Frequent: Misra-Gries bounds", &frequent_bounds},
        std::pair{"CountSketch: heavy hitters", &count_sketch_heavy_hitters},
        std::pair{"GroupTest: hot items", &group_test_hot_items},
        std::pair{"Probabilistic-InPlace: top candidates", &probabilistic_inplace_top},
        std::pair{"ExactTopK: exact top-k after every insert", &exact_top_k_after_every_insert},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        const int f = test();
        std::println("[{}] {}", f == 0 ? " OK " : "FAIL", name);
        failures += f;
    }
    if (failures != 0) return EXIT_FAILURE;
    std::println("all tests passed");
}
