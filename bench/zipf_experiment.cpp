// Reproduces the synthetic experiments of Sec. 5 on Zipf($\alpha$) streams, $\alpha = 0, 0.5, \dots, 3$:
//   Fig. 5  frequent elements, $\phi = 10^{-2}$, $\varepsilon = \phi/100$: Space-Saving vs Frequent, GroupTest
//   Fig. 6  top-k, $k = 100$: Space-Saving vs CountSketch, Probabilistic-InPlace
//
// Space budgets:
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
#include <space_saving/baselines/count_sketch.hpp>
#include <space_saving/baselines/frequent.hpp>
#include <space_saving/baselines/group_test.hpp>
#include <space_saving/baselines/probabilistic_inplace.hpp>
#include <space_saving/space_saving.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
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

using Item = std::uint32_t;
using Freq = std::vector<ss::Count>;  // exact frequency, indexed by item

constexpr double delta = 0.01;  // $\delta$: failure probability of the sketches
constexpr std::size_t count_sketch_factor = 16;

struct Config {
    std::uint64_t n = 10'000'000;
    Item alphabet = 1'000'000;
    double phi = 1e-2;
    std::size_t k = 100;
    std::uint64_t seed = 2005;
    std::optional<std::size_t> topk_m;
};

// ------------------------------------------------------------------- CLI
/// Parses all of `text` into `out`; false on malformed input.
template <class T>
bool assign(T& out, std::string_view text) {
    const auto* const last = std::to_address(text.end());
    const auto result = std::from_chars(std::to_address(text.begin()), last, out);
    return result && result.ptr == last;  // C++26: from_chars_result is testable
}

std::expected<Config, std::string> parse_args(std::span<char* const> args) {
    Config cfg;
    for (const std::string_view arg : args | std::views::drop(1)) {
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
    const bool phi_ok = cfg.phi > 0 && cfg.phi < 1;  // also rejects NaN
    if (!phi_ok || cfg.alphabet == 0 || cfg.k == 0 || cfg.k > cfg.alphabet || cfg.topk_m == 0)
        return std::unexpected{std::string{"argument out of range"}};
    return cfg;
}

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
    double precision;
    double recall;
    std::optional<bool> guaranteed;
    std::optional<bool> order;
    std::size_t bytes;
};

/// Feeds the stream into `sketch` (timed), asks `query` and scores its answer against
/// `correct`: $\mathrm{precision} = \mathrm{hits} / |\mathrm{answer}|$, $\mathrm{recall} = \min(\mathrm{hits}, \mathrm{relevant}) / \mathrm{relevant}$.
Row measure(std::string_view name, const Workload& w, auto sketch, auto query, auto correct, std::size_t relevant) {
    const auto start = std::chrono::steady_clock::now();
    for (const Item x : w.stream) sketch.insert(x);
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;

    const Answer answer = query(std::as_const(sketch));
    const auto hits = static_cast<std::size_t>(std::ranges::count_if(answer.items, correct));
    const auto ratio = [](std::size_t a, std::size_t b) {
        return b == 0 ? 1.0 : static_cast<double>(a) / static_cast<double>(b);
    };
    return {
        .alpha = w.alpha,
        .algorithm = name,
        .counters = sketch.counters(),
        .ms = elapsed.count(),
        .precision = ratio(hits, answer.items.size()),
        .recall = ratio(std::min(hits, relevant), relevant),
        .guaranteed = answer.guaranteed,
        .order = answer.order,
        .bytes = sketch.memory_bytes(),
    };
}

std::vector<Item> items_of(std::ranges::input_range auto&& entries) {
    return entries | std::views::transform(&ss::Entry<Item>::item) | std::ranges::to<std::vector>();
}

std::size_t capped(double m, Item alphabet) {
    return static_cast<std::size_t>(std::min(std::ceil(m), static_cast<double>(alphabet)));
}

// ----------------------------------------------------------- experiments
std::vector<Row> frequent_experiment(const Config& cfg, const Workload& w) {
    const double support = cfg.phi * static_cast<double>(cfg.n);
    const auto is_frequent = [&](Item x) { return static_cast<double>(w.freq[x]) > support; };
    const auto relevant = static_cast<std::size_t>(
        std::ranges::count_if(w.freq, [&](ss::Count f) { return static_cast<double>(f) > support; }));
    const auto run = [&](std::string_view name, auto sketch, auto query) {
        return measure(name, w, std::move(sketch), query, is_frequent, relevant);
    };

    const double inv_eps = 100.0 / cfg.phi;
    const auto m = capped(w.alpha >= 1 ? std::pow(inv_eps, 1 / w.alpha) : inv_eps, cfg.alphabet);
    const auto tests = static_cast<std::size_t>(std::ceil(std::log(1 / (delta * cfg.phi))));
    const auto groups = static_cast<std::size_t>(std::ceil(2 / cfg.phi));
    const int id_bits = std::max(1, std::bit_width(cfg.alphabet - 1));

    return {
        run("Space-Saving", ss::SpaceSaving<Item>{m},
            [&](const auto& s) {
                const auto r = s.query_frequent(cfg.phi);
                return Answer{.items = items_of(r.elements), .guaranteed = r.guaranteed};
            }),
        run("Frequent", ss::baseline::Frequent<Item>{static_cast<std::size_t>(std::ceil(1 / cfg.phi))},
            [](const auto& s) { return Answer{.items = s.candidates()}; }),
        run("GroupTest", ss::baseline::GroupTest<Item>{tests, groups, id_bits, cfg.seed},
            [&](const auto& s) { return Answer{.items = s.hot_items(cfg.phi)}; }),
    };
}

std::vector<Row> top_k_experiment(const Config& cfg, const Workload& w) {
    auto ranked = w.freq;
    std::ranges::nth_element(ranked, ranked.begin() + static_cast<std::ptrdiff_t>(cfg.k - 1), std::greater{});
    const ss::Count f_k = ranked[cfg.k - 1];
    const auto in_top_k = [&](Item x) { return w.freq[x] >= f_k; };  // ties at $F_k$ count as correct
    const auto run = [&](std::string_view name, auto sketch, auto query) {
        return measure(name, w, std::move(sketch), query, in_top_k, cfg.k);
    };

    const auto k = static_cast<double>(cfg.k);
    const auto m = cfg.topk_m.value_or(capped(
        w.alpha > 1 ? std::pow(k / w.alpha, 1 / w.alpha) * k : k * k * std::log(cfg.alphabet), cfg.alphabet));
    const auto rows = static_cast<std::size_t>(std::ceil(std::log(static_cast<double>(cfg.n) / delta)));
    const auto round = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(m * cfg.n) / 2)));

    return {
        run("Space-Saving", ss::SpaceSaving<Item>{m},
            [&](const auto& s) {
                const auto r = s.query_top_k(cfg.k);
                return Answer{
                    .items = items_of(r.elements | std::views::take(cfg.k)),
                    .guaranteed = r.guaranteed,
                    .order = r.order,
                };
            }),
        run("CountSketch", ss::baseline::CountSketch<Item>{rows, count_sketch_factor * cfg.k, cfg.k, cfg.seed},
            [](const auto& s) { return Answer{.items = s.top_k()}; }),
        // FindCandidateTop$(S, k, m/2)$: all $m/2$ candidates are the answer (Sec. 2)
        run("Probabilistic-InPlace", ss::baseline::ProbabilisticInPlace<Item>{m, round},
            [](const auto& s) { return Answer{.items = s.candidates()}; }),
    };
}

void print_table(std::string_view title, std::span<const Row> rows) {
    const auto flag = [](std::optional<bool> b) -> std::string_view {
        if (!b) return "-";
        return *b ? "yes" : "no";
    };
    std::println("\n### {}\n", title);
    std::println("|  α  | algorithm             | counters | time (ms) | precision | recall | guaranteed | order | memory (KiB) |");
    std::println("|----:|-----------------------|---------:|----------:|----------:|-------:|:----------:|:-----:|-------------:|");
    for (const auto& r : rows)
        std::println("| {:.1f} | {:<21} | {:>8} | {:>9.0f} | {:>9.3f} | {:>6.3f} | {:^10} | {:^5} | {:>12.1f} |",
                     r.alpha, r.algorithm, r.counters, r.ms, r.precision, r.recall, flag(r.guaranteed),
                     flag(r.order), static_cast<double>(r.bytes) / 1024.0);
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape): terminate on bad_alloc etc.
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const auto cfg = parse_args(args);
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
    for (const double alpha : std::views::iota(0, 7) | std::views::transform([](int i) { return i * 0.5; })) {
        const auto workload = make_workload(*cfg, alpha, rng);
        frequent_rows.append_range(frequent_experiment(*cfg, workload));
        top_k_rows.append_range(top_k_experiment(*cfg, workload));
    }
    print_table(std::format("Frequent elements (phi = {}) - cf. Fig. 5", cfg->phi), frequent_rows);
    print_table(std::format("Top-{} - cf. Fig. 6", cfg->k), top_k_rows);
}
