// Space-Saving (Metwally et al., ICDT 2005): Fig. 1 (update), Fig. 3 (frequent
// elements query) and Fig. 4 (top-k query) on top of Stream-Summary.
#pragma once

#include "stream_summary.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace ss {

template <class Key>
struct FrequentResult {
    std::vector<Entry<Key>> elements;
    bool guaranteed = true;  ///< elements $= \{\, e : f_e > \phi N \,\}$ exactly
};

template <class Key>
struct TopKResult {
    std::vector<Entry<Key>> elements;  ///< the first $k$, or $k' > k$ if the search was extended
    bool guaranteed = false;           ///< elements are a correct top-$|\mathrm{elements}|$
    bool order = true;                 ///< the first $k$ elements are in their true order
};

template <std::copyable Key, HashFor<Key> Hash = std::hash<Key>,
          std::equivalence_relation<Key, Key> KeyEqual = std::equal_to<Key>>
class SpaceSaving {
public:
    using Summary = StreamSummary<Key, Hash, KeyEqual>;

    /// @param m number of counters $m$.
    explicit SpaceSaving(std::size_t m) : summary_{m} {}

    /// Fig. 1.
    void insert(const Key& e) {
        ++n_;
        if (summary_.try_increment(e)) return;
        if (summary_.full())
            summary_.replace_min(e);
        else
            summary_.add(e);
    }

    [[nodiscard]] Count stream_length() const noexcept { return n_; }
    [[nodiscard]] std::size_t counters() const noexcept { return summary_.capacity(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept { return summary_.memory_bytes(); }

    /// $\mathit{min}$ of Lemma 1 ($\mathit{min} \le \lfloor N/m \rfloor$); bounds every $\varepsilon_i$ and every unmonitored frequency.
    [[nodiscard]] Count min_count() const noexcept { return summary_.min_count(); }

    [[nodiscard]] std::optional<Entry<Key>> estimate(const Key& e) const { return summary_.find(e); }

    [[nodiscard]] std::vector<Entry<Key>> sorted() const {
        return summary_.descending() | std::ranges::to<std::vector>();
    }

    /// Fig. 3: report every element whose counter exceeds $\phi N$.
    ///
    /// Beyond Fig. 3 we also require $\mathit{min} \le \phi N$ for `guaranteed`: otherwise an
    /// unmonitored element might be frequent too (only possible when $m < 1/\phi$).
    [[nodiscard]] FrequentResult<Key> query_frequent(double phi) const {
        const double support = phi * static_cast<double>(n_);
        const auto above = [support](Count c) { return static_cast<double>(c) > support; };

        FrequentResult<Key> r;
        r.guaranteed = !above(min_count());
        for (const auto& e : summary_.descending() |
                                 std::views::take_while([&](const auto& x) { return above(x.count); })) {
            r.guaranteed = r.guaranteed && above(e.guaranteed());
            r.elements.push_back(e);
        }
        return r;
    }

    /// Fig. 4: report the top-k; if they cannot be guaranteed, keep extending to the
    /// smallest $k' > k$ whose top-$k'$ is guaranteed. The two loops of Fig. 4 are merged.
    /// Beyond the last counter we use $\mathrm{count}_{m+1} = \mathit{min}$, which makes the top-$m$
    /// guaranteed only when $\varepsilon_m = 0$, as stated in Sec. 4.2.
    [[nodiscard]] TopKResult<Key> query_top_k(std::size_t k) const {
        const auto all = sorted();
        const auto count_at = [&](std::size_t i) { return i < all.size() ? all[i].count : min_count(); };

        TopKResult<Key> r;
        Count min_guaranteed = std::numeric_limits<Count>::max();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (i >= k && count_at(i) <= min_guaranteed) break;  // top-$i$ is guaranteed
            r.elements.push_back(all[i]);
            min_guaranteed = std::min(min_guaranteed, all[i].guaranteed());
            if (i < k && all[i].guaranteed() < count_at(i + 1)) r.order = false;
        }
        r.guaranteed = count_at(r.elements.size()) <= min_guaranteed;
        return r;
    }

    /// Stream-Summary invariants plus the accounting identity $\sum_i \mathrm{count}_i = N$.
    [[nodiscard]] std::expected<void, std::string> validate() const {
        return summary_.validate().and_then([this] -> std::expected<void, std::string> {
            const Count total = std::ranges::fold_left(
                summary_.descending() | std::views::transform(&Entry<Key>::count), Count{0}, std::plus{});
            if (total != n_) return std::unexpected{"sum of counters != N"};
            return {};
        });
    }

private:
    Summary summary_;
    Count n_ = 0;
};

}  // namespace ss
