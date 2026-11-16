// Checks what the experiment reports (bench/experiment.hpp): the top-k scoring
// with ties, the frequent-elements scoring, and that every accepted configuration
// gives every algorithm a usable budget.
#include <experiment.hpp>

#include <space_saving/baselines/count_sketch.hpp>
#include <space_saving/baselines/frequent.hpp>
#include <space_saving/baselines/group_test.hpp>
#include <space_saving/baselines/probabilistic_inplace.hpp>
#include <space_saving/space_saving.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <numeric>
#include <print>
#include <random>
#include <ranges>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace {

using bench::Item;

class Checker {
public:
    void expect(bool ok, std::string_view what, std::source_location loc = std::source_location::current()) {
        if (ok) return;
        ++failures_;
        std::println(stderr, "  FAIL line {}: {}", loc.line(), what);
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_ = 0;
};

// ----------------------------------------------------------------- top-k
void top_k_with_ties(Checker& t) {
    const bench::Freq f{5, 4, 4};  // A = 5, B = 4, C = 4
    const auto score = [&](std::initializer_list<Item> answer) { return bench::score_top_k(answer, f, 2); };

    const auto bc = score({1, 2});  // misses A: only one of the tied B, C fits next to it
    t.expect(bc.precision == 0.5 && bc.recall == 0.5 && bc.exact == false, "{B, C} is not a top-2");
    for (const auto answer : {std::array<Item, 2>{0, 1}, std::array<Item, 2>{0, 2}}) {
        const auto s = bench::score_top_k(answer, f, 2);
        t.expect(s.precision == 1 && s.recall == 1 && s.exact == true, "{A, B} and {A, C} are top-2");
    }
    const auto abc = score({0, 1, 2});  // a candidate superset: no exact verdict
    t.expect(std::abs(abc.precision - (2.0 / 3)) < 1e-12 && abc.recall == 1 && !abc.exact, "{A, B, C}");
}

/// Precision and recall are both 1 exactly when the independent check accepts the answer.
void top_k_randomized(Checker& t) {
    std::mt19937_64 rng{2005};  // NOLINT(bugprone-random-generator-seed): reproducible tests
    for (int trial = 0; trial < 20'000; ++trial) {
        const auto alphabet = std::uniform_int_distribution<Item>{1, 12}(rng);
        bench::Freq f(alphabet);
        std::uniform_int_distribution<ss::Count> small{0, 4};  // many ties
        std::ranges::generate(f, [&] { return small(rng); });
        const auto k = std::uniform_int_distribution<std::size_t>{1, alphabet}(rng);

        std::vector<Item> answer(alphabet);
        std::ranges::iota(answer, Item{0});
        std::ranges::shuffle(answer, rng);
        answer.resize(std::uniform_int_distribution<std::size_t>{0, alphabet}(rng));

        const auto s = bench::score_top_k(answer, f, k);
        const bool perfect = s.precision == 1 && s.recall == 1;
        t.expect(perfect == bench::is_exact_top_k(answer, f, k), "perfect score <=> exact top-k");
        t.expect(s.exact.has_value() == (answer.size() == k), "exact reported for k-item answers only");
        t.expect(s.precision <= 1 && s.recall <= 1, "scores in [0, 1]");
    }
}

void frequent_scoring(Checker& t) {
    const bench::Freq f{10, 3, 7};  // support 5: items 0 and 2 are frequent
    const auto s = bench::score_frequent(std::array<Item, 2>{0, 1}, f, 5.0);
    t.expect(s.precision == 0.5 && s.recall == 0.5 && !s.exact, "frequent scoring");
}

// ------------------------------------------------------------ configuration
auto parse(std::initializer_list<std::string_view> args) {
    return bench::parse_args(std::span<const std::string_view>{args});
}

void rejects_bad_configurations(Checker& t) {
    t.expect(parse({}).has_value(), "defaults accepted");
    t.expect(parse({"--k=1", "--topk-m=2"}).has_value(), "k = 1 with an explicit budget accepted");

    const auto k1 = parse({"--k=1"});  // $m = 1$ at $\alpha = 1.5$
    t.expect(!k1 && k1.error().contains("Probabilistic-InPlace"), "k = 1 rejected with a reason");
    const std::vector<std::vector<std::string_view>> bad{
        {"--topk-m=1"}, {"--n=0"}, {"--phi=0"}, {"--phi=1"}, {"--phi=1e-12"}, {"--phi=nan"},
        {"--alphabet=0"}, {"--alphabet=4294967295"}, {"--k=0"}, {"--alphabet=50", "--k=51"},
        {"--n=12x"}, {"--bogus=1"}, {"--n"},
    };
    for (const auto& args : bad) t.expect(!bench::parse_args(args).has_value(), "bad configuration rejected");
}

/// Every configuration that passes `check` must let every algorithm be built.
void accepted_budgets_are_usable(Checker& t) {
    for (const double phi : {0.9, 0.3, 0.01})
        for (const std::size_t k : {1UZ, 2UZ, 3UZ, 10UZ, 50UZ}) {
            bench::Config cfg{.n = 1000, .alphabet = 50, .phi = phi, .k = k, .seed = 1, .topk_m = std::nullopt};
            if (!bench::check(cfg)) cfg.topk_m = 2;
            t.expect(bench::check(cfg).has_value(), "configuration accepted");
            for (const double alpha : bench::alphas()) {
                try {
                    const auto f = bench::frequent_budget(cfg, alpha);
                    const auto top = bench::top_k_budget(cfg, alpha);
                    const ss::SpaceSaving<Item> a{f.space_saving};
                    const ss::baseline::Frequent<Item> b{f.frequent};
                    const ss::baseline::GroupTest<Item> c{f.tests, f.groups, f.id_bits, cfg.seed};
                    const ss::SpaceSaving<Item> d{top.space_saving};
                    const ss::baseline::CountSketch<Item> e{top.rows, top.columns, cfg.k, cfg.seed};
                    const ss::baseline::ProbabilisticInPlace<Item> g{top.space_saving, top.round};
                } catch (const std::exception& e) {
                    t.expect(false, e.what());
                }
            }
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
    run("top-k scoring with ties", top_k_with_ties);
    run("top-k scoring: perfect <=> exact (randomized)", top_k_randomized);
    run("frequent scoring", frequent_scoring);
    run("bad configurations rejected", rejects_bad_configurations);
    run("accepted budgets are usable", accepted_budgets_are_usable);
    if (t.failures() != 0) {
        std::println("{} check(s) failed", t.failures());
        return EXIT_FAILURE;
    }
    std::println("all tests passed");
}
