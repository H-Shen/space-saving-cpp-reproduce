// Reproduces the synthetic experiments of Sec. 5 on Zipf($\alpha$) streams, $\alpha = 0, 0.5, \dots, 3$:
//   Fig. 5  frequent elements, $\phi = 10^{-2}$, $\varepsilon = \phi/100$: Space-Saving vs Frequent, GroupTest
//   Fig. 6  top-k, $k = 100$: Space-Saving vs CountSketch, Probabilistic-InPlace
//
// Space budgets (see experiment.hpp):
//   Space-Saving, frequent : $m = \min(|A|, (1/\varepsilon)^{1/\alpha}, 1/\varepsilon)$          (Theorem 4)
//   Space-Saving, top-k    : $m = \min(|A|, (k/\alpha)^{1/\alpha} k)$ for $\alpha > 1$,       (Theorem 6)
//                            $m = \min(|A|, k^2 \ln|A|)$ for $\alpha \le 1$
//   Frequent               : $m = 1/\phi$, its only parameter (Sec. 4.1)
//   GroupTest              : $t = \lceil \ln\frac{1}{\delta\phi} \rceil$ tests $\times\ 2/\phi$ groups
//                            $\times\ (1 + \lceil \log_2 |A| \rceil)$ counters, $\delta = 0.01$;
//                            cf. $O(\frac{1}{\phi} \log\frac{1}{\delta\phi} \log|A|)$ in [5]
//   CountSketch            : $t = \lceil \ln(N/\delta) \rceil$ rows $\times\ b = 16k$ columns, $\delta = 0.01$;
//                            $O(\log(N/\delta))$ rows as in [3], "hidden factor of 16" (Sec. 5)
//   Probabilistic-InPlace  : the same $m$ as Space-Saving (Sec. 5), rounds of $r = \sqrt{mN/2}$ [6]
//
// usage: zipf_experiment [--n=N] [--alphabet=A] [--phi=P] [--k=K] [--seed=S] [--topk-m=M]
#include "experiment.hpp"

#include <space_saving/baselines/count_sketch.hpp>
#include <space_saving/baselines/frequent.hpp>
#include <space_saving/baselines/group_test.hpp>
#include <space_saving/baselines/probabilistic_inplace.hpp>
#include <space_saving/space_saving.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bench::Config;
using bench::Freq;
using bench::Item;

// -------------------------------------------------------------- workload
/// Zipf over ranks $0, \dots, n-1$, $P(i) \propto (i+1)^{-\alpha}$, by inverse-CDF lookup; $\alpha = 0$ is uniform.
class ZipfDistribution {
public:
    ZipfDistribution(Item n, double alpha) : cdf_(n) {
        auto weights = std::views::iota(Item{1}, n + 1) |
                       std::views::transform([alpha](Item i) { return std::pow(static_cast<double>(i), -alpha); });
        std::inclusive_scan(weights.begin(), weights.end(), cdf_.begin());
        std::ranges::transform(cdf_, cdf_.begin(), [total = cdf_.back()](double p) { return p / total; });
        cdf_.back() = 1.0;
    }

    Item operator()(std::uniform_random_bit_generator auto& rng) const {
        const double u = std::uniform_real_distribution<>{}(rng);
        return static_cast<Item>(std::ranges::upper_bound(cdf_, u) - cdf_.begin());
    }

private:
    std::vector<double> cdf_;
};

struct Workload {
    double alpha;
    std::vector<Item> stream;
    Freq freq;
};

/// Zipf ranks mapped to shuffled ids, so ids carry no frequency order.
Workload make_workload(const Config& cfg, double alpha, std::mt19937_64& rng) {
    std::vector<Item> ids(cfg.alphabet);
    std::ranges::iota(ids, Item{0});
    std::ranges::shuffle(ids, rng);
    const ZipfDistribution zipf{cfg.alphabet, alpha};

    Workload w{.alpha = alpha, .stream = std::vector<Item>(cfg.n), .freq = Freq(cfg.alphabet)};
    std::ranges::generate(w.stream, [&] { return ids[zipf(rng)]; });
    for (const Item x : w.stream) ++w.freq[x];
    return w;
}

// ------------------------------------------------------------- measuring
/// What a query reports; a flag is nullopt when the algorithm offers no such guarantee.
struct Answer {
    std::vector<Item> items;
    std::optional<bool> guaranteed = std::nullopt;
    std::optional<bool> order = std::nullopt;
};

struct Row {
    double alpha;
    std::string_view algorithm;
    std::size_t counters;
    double ms;
    bench::Score score;
    std::optional<bool> guaranteed;
    std::optional<bool> order;
    std::size_t bytes;
};

/// Feeds the stream into `sketch` (timed), asks `query` and scores the answer with `score`.
Row measure(std::string_view name, const Workload& w, auto sketch, auto query, auto score) {
    const auto start = std::chrono::steady_clock::now();
    for (const Item x : w.stream) sketch.insert(x);
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;

    const Answer answer = query(std::as_const(sketch));
    return {
        .alpha = w.alpha,
        .algorithm = name,
        .counters = sketch.counters(),
        .ms = elapsed.count(),
        .score = score(std::span<const Item>{answer.items}),
        .guaranteed = answer.guaranteed,
        .order = answer.order,
        .bytes = sketch.memory_bytes(),
    };
}

std::vector<Item> items_of(std::ranges::input_range auto&& entries) {
    return entries | std::views::transform(&ss::Entry<Item>::item) | std::ranges::to<std::vector>();
}

// ----------------------------------------------------------- experiments
std::vector<Row> frequent_experiment(const Config& cfg, const Workload& w) {
    const double support = cfg.phi * static_cast<double>(cfg.n);
    const auto score = [&](std::span<const Item> items) { return bench::score_frequent(items, w.freq, support); };
    const auto run = [&](std::string_view name, auto sketch, auto query) {
        return measure(name, w, std::move(sketch), query, score);
    };
    const auto b = bench::frequent_budget(cfg, w.alpha);

    return {
        run("Space-Saving", ss::SpaceSaving<Item>{b.space_saving},
            [&](const auto& s) {
                const auto r = s.query_frequent(cfg.phi);
                return Answer{.items = items_of(r.elements), .guaranteed = r.guaranteed};
            }),
        run("Frequent", ss::baseline::Frequent<Item>{b.frequent},
            [](const auto& s) { return Answer{.items = s.candidates()}; }),
        run("GroupTest", ss::baseline::GroupTest<Item>{b.tests, b.groups, b.id_bits, cfg.seed},
            [&](const auto& s) { return Answer{.items = s.hot_items(cfg.phi)}; }),
    };
}

std::vector<Row> top_k_experiment(const Config& cfg, const Workload& w) {
    const auto score = [&](std::span<const Item> items) { return bench::score_top_k(items, w.freq, cfg.k); };
    const auto run = [&](std::string_view name, auto sketch, auto query) {
        return measure(name, w, std::move(sketch), query, score);
    };
    const auto b = bench::top_k_budget(cfg, w.alpha);

    return {
        // The answer is the first $k$ elements. Fig. 4's `guaranteed` covers the whole,
        // possibly extended, answer, so the top-$k$ alone is certified only without extension.
        run("Space-Saving", ss::SpaceSaving<Item>{b.space_saving},
            [&](const auto& s) {
                const auto r = s.query_top_k(cfg.k);
                return Answer{
                    .items = items_of(r.elements | std::views::take(cfg.k)),
                    .guaranteed = r.certified_top(cfg.k),
                    .order = r.order,
                };
            }),
        run("CountSketch", ss::baseline::CountSketch<Item>{b.rows, b.columns, cfg.k, cfg.seed},
            [](const auto& s) { return Answer{.items = s.top_k()}; }),
        // FindCandidateTop$(S, k, m/2)$: all $m/2$ candidates are the answer (Sec. 2)
        run("Probabilistic-InPlace", ss::baseline::ProbabilisticInPlace<Item>{b.space_saving, b.round},
            [](const auto& s) { return Answer{.items = s.candidates()}; }),
    };
}

void print_table(std::string_view title, std::span<const Row> rows) {
    const auto flag = [](std::optional<bool> b) -> std::string_view {
        if (!b) return "-";
        return *b ? "yes" : "no";
    };
    std::println("\n### {}\n", title);
    std::println("|  α  | algorithm             | counters | time (ms) | precision | recall | exact | guaranteed | "
                 "order | memory (KiB) |");
    std::println("|----:|-----------------------|---------:|----------:|----------:|-------:|:-----:|:----------:|"
                 ":-----:|-------------:|");
    for (const auto& r : rows)
        std::println("| {:.1f} | {:<21} | {:>8} | {:>9.0f} | {:>9.3f} | {:>6.3f} | {:^5} | {:^10} | {:^5} | {:>12.1f} |",
                     r.alpha, r.algorithm, r.counters, r.ms, r.score.precision, r.score.recall, flag(r.score.exact),
                     flag(r.guaranteed), flag(r.order), static_cast<double>(r.bytes) / 1024.0);
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape): terminate on bad_alloc etc.
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const auto options = args | std::views::drop(1) | std::ranges::to<std::vector<std::string_view>>();
    const auto cfg = bench::parse_args(options);
    if (!cfg) {
        std::println(stderr, "{}\nusage: {} [--n=N] [--alphabet=A] [--phi=P] [--k=K] [--seed=S] [--topk-m=M]",
                     cfg.error(), args.front());
        return EXIT_FAILURE;
    }
    std::println("N = {}, |A| = {}, phi = {}, eps = phi/100, k = {}, seed = {}", cfg->n, cfg->alphabet, cfg->phi,
                 cfg->k, cfg->seed);

    std::vector<Row> frequent_rows;
    std::vector<Row> top_k_rows;
    std::mt19937_64 rng{cfg->seed};
    for (const double alpha : bench::alphas()) {
        const auto workload = make_workload(*cfg, alpha, rng);
        frequent_rows.append_range(frequent_experiment(*cfg, workload));
        top_k_rows.append_range(top_k_experiment(*cfg, workload));
    }
    print_table(std::format("Frequent elements (phi = {}) - cf. Fig. 5", cfg->phi), frequent_rows);
    print_table(std::format("Top-{} - cf. Fig. 6", cfg->k), top_k_rows);
}
