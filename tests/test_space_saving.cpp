// Checks the paper's worked examples, Lemma 1, Theorems 1-2, that every
// `guaranteed` / `order` flag reported by Fig. 3 and Fig. 4 is actually true,
// and that copies and moves are independent, valid summaries.
#include <space_saving/space_saving.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <print>
#include <random>
#include <ranges>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Item = std::size_t;
using Freq = std::vector<ss::Count>;  // exact frequency, indexed by item

class Checker {
public:
    void expect(bool ok, std::string_view what, std::source_location loc = std::source_location::current()) {
        if (ok) return;
        ++failures_;
        std::println(stderr, "  FAIL line {}: {}", loc.line(), what);
    }
    void expect_valid(const auto& s, std::source_location loc = std::source_location::current()) {
        const auto v = s.validate();
        expect(v.has_value(), v ? "" : v.error(), loc);
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_ = 0;
};

// ------------------------------------------------------------ paper examples
void paper_example_1(Checker& t) {  // Fig. 2: $m = 2$, $S = X, Y, Y, Z$
    ss::SpaceSaving<char> s{2};
    for (const char c : std::string_view{"XYYZ"}) s.insert(c);
    t.expect(!s.estimate('X'), "X evicted");
    t.expect(s.estimate('Y') == ss::Entry<char>{.item = 'Y', .count = 2, .error = 0}, "Y = 2, eps 0");
    t.expect(s.estimate('Z') == ss::Entry<char>{.item = 'Z', .count = 2, .error = 1}, "Z = 2, eps 1");
    t.expect_valid(s);
}

void lemma1_permutations(Checker& t) {  // Sec. 3.2: $\mathit{min}$ depends on the arrival order
    ss::SpaceSaving<char> a{2};
    ss::SpaceSaving<char> b{2};
    for (const char c : std::string_view{"XZYY"}) a.insert(c);
    for (const char c : std::string_view{"XYYZ"}) b.insert(c);
    t.expect(a.min_count() == 1, "XZYY -> min 1");
    t.expect(b.min_count() == 2, "XYYZ -> min 2");
}

void exact_when_not_full(Checker& t) {
    ss::SpaceSaving<Item> s{10};
    for (const Item x : std::array<Item, 10>{1, 2, 2, 3, 3, 3, 4, 4, 4, 4}) s.insert(x);
    t.expect(std::ranges::all_of(s.sorted(), [](const auto& e) { return e.error == 0 && e.count == e.item; }),
             "exact counts");
    const auto top = s.query_top_k(2);
    t.expect(top.guaranteed && top.order && top.elements.size() == 2 && top.elements.front().item == 4,
             "exact top-2");
    t.expect(s.query_frequent(0.25).elements.size() == 2, "exact frequent");
}

// The extended answer $\{D, C\}$ is a guaranteed top-2, but its first element alone is
// not a guaranteed top-1 (true frequencies: C = 3, D = 2, B = E = 1).
void extended_top_k(Checker& t) {
    ss::SpaceSaving<char> s{3};
    for (const char c : std::string_view{"BCCECDD"}) s.insert(c);
    const auto r = s.query_top_k(1);
    t.expect(r.elements.size() == 2 && r.guaranteed && !r.order, "top-1 extended to a guaranteed top-2");
    t.expect(!r.certified_top(1), "the top-1 prefix alone is not certified");
    t.expect(r.certified_top(2), "the extended top-2 is");
}

// Copies are independent of their source and outlive it; moves leave an empty summary.
void copy_and_move(Checker& t) {
    const auto count = [](const auto& s, int x) {  // nullopt if `x` is not monitored
        return s.estimate(x).transform([](const auto& e) { return e.count; });
    };
    auto a = std::make_unique<ss::SpaceSaving<int>>(3);
    a->insert(1);
    auto b = *a;
    t.expect_valid(b);
    b.insert(1);
    t.expect(count(*a, 1) == ss::Count{1} && count(b, 1) == ss::Count{2}, "copy is independent of its source");
    a.reset();
    t.expect(count(b, 1) == ss::Count{2}, "copy outlives its source");
    t.expect_valid(b);

    ss::SpaceSaving<int> c{5};
    for (const int x : {7, 8, 8}) c.insert(x);
    c = b;  // copy assignment replaces the old contents
    c.insert(2);
    t.expect(!c.estimate(7) && count(c, 1) == ss::Count{2} && !b.estimate(2), "copy assignment is independent");
    t.expect_valid(c);
    t.expect_valid(b);

    auto d = std::move(c);
    t.expect(d.estimate(2).has_value() && d.stream_length() == 3, "move keeps the contents");
    t.expect_valid(d);
    t.expect_valid(c);  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from state
    t.expect(c.stream_length() == 0 && c.sorted().empty(), "moved-from summary is empty");
    c.insert(9);  // and still usable
    t.expect_valid(c);
    d = std::move(c);
    t.expect(d.stream_length() == 1 && d.estimate(9).has_value(), "move assignment");
    t.expect_valid(d);
}

// ------------------------------------------------- randomized property checks
struct Trial {
    ss::SpaceSaving<Item> sketch;
    Freq freq;
};

/// A random skewed stream over a small alphabet, summarised with a random $m$.
Trial random_trial(std::mt19937_64& rng, Checker& t) {
    const auto alphabet = std::uniform_int_distribution<Item>{2, 80}(rng);
    const auto m = std::uniform_int_distribution<std::size_t>{1, 25}(rng);
    const auto n = std::uniform_int_distribution{1, 3000}(rng);
    const auto skew = std::uniform_real_distribution{0.0, 2.5}(rng);

    auto weights = std::views::iota(Item{1}, alphabet + 1) |
                   std::views::transform([skew](Item i) { return std::pow(static_cast<double>(i), -skew); }) |
                   std::ranges::to<std::vector>();
    std::ranges::shuffle(weights, rng);
    std::discrete_distribution<Item> pick(weights.begin(), weights.end());

    Trial tr{.sketch = ss::SpaceSaving<Item>{m}, .freq = Freq(alphabet)};
    for (int i = 0; i < n; ++i) {
        const Item x = pick(rng);
        tr.sketch.insert(x);
        ++tr.freq[x];
        if (i % 101 == 0) t.expect_valid(tr.sketch);
    }
    t.expect_valid(tr.sketch);
    return tr;
}

/// Largest true frequency among the elements not listed in `chosen`.
ss::Count best_outside(const Freq& f, std::span<const ss::Entry<Item>> chosen) {
    std::vector<bool> inside(f.size());
    for (const auto& e : chosen) inside[e.item] = true;
    ss::Count best = 0;
    for (const auto [fx, in] : std::views::zip(f, inside))
        if (!in) best = std::max(best, fx);
    return best;
}

void check_bounds(const Trial& tr, Checker& t) {
    const auto& [s, f] = tr;
    const auto mn = s.min_count();
    const auto all = s.sorted();

    t.expect(mn <= s.stream_length() / s.counters(), "Lemma 1: min <= N/m");
    for (const auto& e : all)
        t.expect(e.guaranteed() <= f[e.item] && f[e.item] <= e.count && e.error <= mn, "error bounds");
    for (const auto [x, fx] : std::views::enumerate(f))
        t.expect(fx <= mn || s.estimate(static_cast<Item>(x)).has_value(), "Theorem 1: f > min => monitored");

    auto ranked = f;
    std::ranges::sort(ranked, std::greater{});
    for (const auto& [e, rank_f] : std::views::zip(all, ranked))
        t.expect(e.count >= rank_f, "Theorem 2: count_i >= F_i");
}

void check_frequent(const Trial& tr, Checker& t) {
    const auto& [s, f] = tr;
    for (const double phi : {0.01, 0.05, 0.1, 0.2, 0.4}) {
        const auto r = s.query_frequent(phi);
        if (!r.guaranteed) continue;
        const double support = phi * static_cast<double>(s.stream_length());
        auto got = r.elements | std::views::transform(&ss::Entry<Item>::item) | std::ranges::to<std::vector>();
        const auto want = std::views::iota(Item{0}, f.size()) |
                          std::views::filter([&](Item x) { return static_cast<double>(f[x]) > support; }) |
                          std::ranges::to<std::vector>();
        std::ranges::sort(got);
        t.expect(got == want, "Fig. 3: guaranteed => exactly the frequent set");
    }
}

void check_top_k(const Trial& tr, Checker& t) {
    const auto& [s, f] = tr;
    for (const std::size_t k : {1UZ, 2UZ, 3UZ, 5UZ, 10UZ}) {
        const auto r = s.query_top_k(k);
        const std::span out{r.elements};
        if (r.guaranteed && !out.empty()) {
            const auto worst = std::ranges::min(out | std::views::transform([&](const auto& e) { return f[e.item]; }));
            t.expect(worst >= best_outside(f, out), "Fig. 4: guaranteed => correct top-k'");
        }
        if (r.certified_top(k)) {
            const auto top = out.first(std::min(k, out.size()));
            const auto worst = std::ranges::min(top | std::views::transform([&](const auto& e) { return f[e.item]; }));
            t.expect(worst >= best_outside(f, top), "certified_top(k) => correct top-k");
        }
        if (r.order) {
            t.expect(r.certified_top(k), "order => certified top-k");
            for (std::size_t i = 0; i < std::min(k, out.size()); ++i)
                t.expect(f[out[i].item] >= best_outside(f, out.first(i + 1)), "Fig. 4: order => true rank");
        }
    }
}

/// A copy starts equal to its source, then changes without affecting it.
void check_copy(const Trial& tr, std::mt19937_64& rng, Checker& t) {
    const auto before = tr.sketch.sorted();
    auto copy = tr.sketch;
    t.expect(copy.sorted() == before && copy.stream_length() == tr.sketch.stream_length(), "copy equal");
    std::uniform_int_distribution<Item> pick{0, 100};
    for (int i = 0; i < 50; ++i) copy.insert(pick(rng));
    t.expect_valid(copy);
    t.expect(tr.sketch.sorted() == before, "source unchanged by the copy");
    t.expect_valid(tr.sketch);
}

void randomized(Checker& t, int trials) {
    std::mt19937_64 rng{20051};  // NOLINT(bugprone-random-generator-seed): reproducible tests
    for (int i = 0; i < trials; ++i) {
        const auto tr = random_trial(rng, t);
        check_bounds(tr, t);
        check_frequent(tr, t);
        check_top_k(tr, t);
        check_copy(tr, rng, t);
    }
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape): a test may terminate on an unexpected exception
    Checker t;
    const auto run = [&t](std::string_view name, auto test) {
        const int before = t.failures();
        test(t);
        std::println("[{}] {}", t.failures() == before ? " OK " : "FAIL", name);
    };
    run("Example 1 / Fig. 2", paper_example_1);
    run("Lemma 1 permutation example", lemma1_permutations);
    run("exact when distinct <= m", exact_when_not_full);
    run("extended top-k: guarantee does not carry over to the prefix", extended_top_k);
    run("copy and move", copy_and_move);
    run("randomized: invariants, Lemma 1, Thm 1-2, Fig. 3-4 soundness, copies",
        [](Checker& c) { randomized(c, 3000); });
    if (t.failures() != 0) {
        std::println("{} check(s) failed", t.failures());
        return EXIT_FAILURE;
    }
    std::println("all tests passed");
}
