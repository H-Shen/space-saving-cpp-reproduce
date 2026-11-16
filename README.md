# Reproducing the Experiments

This project reproduces the Space-Saving algorithm and the experiments presented in **[Efficient Computation of Frequent and Top-k Elements in Data Streams](https://doi.org/10.1007/978-3-540-30570-5_27)** by Ahmed Metwally, Divyakant Agrawal, and Amr El Abbadi (ICDT 2005).

The implementation follows the pseudocode in the paper: Space-Saving (Fig. 1), the Stream-Summary data structure (Sec. 3.1), QueryFrequent (Fig. 3) and QueryTop-k (Fig. 4). It also implements the four algorithms the paper compares against: **Frequent** and **GroupTest** for frequent elements (Fig. 5), and **CountSketch** and **Probabilistic-InPlace** for top-k (Fig. 6). The library is header-only and depends only on the C++ standard library.

## Building and Running

Build the project from this directory with [xmake](https://xmake.io). The following commands use Ubuntu 26.04 LTS as an example:

```sh
sudo apt-get update
sudo apt-get install -y g++-15 curl
curl -fsSL https://xmake.io/shget.text | bash
xmake f -m release --cc=gcc-15 --cxx=g++-15
xmake
xmake test
```

If your system provides a different version of `g++`, adjust `--cc` and `--cxx` accordingly.

The project uses C++23 (`std::generator`, `std::expected`, `std::print`, ranges, deducing `this`) and builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` without warnings.

To run the worked example and the full experiment:

```sh
xmake run paper_example
xmake run zipf_experiment
```

`paper_example` prints the Stream-Summary step by step for the paper's examples and answers both queries on a short stream:

```
== Queries (entries shown as item[count-eps..count]) ==
  summary: [8: A(ε=0)] [5: B(ε=0) G(ε=4)]
  top-2:                       A[8..8] B[5..5]  guaranteed=true order=true
  frequent (phi = 0.2):        A[8..8] B[5..5] G[1..5]  guaranteed=false
```

Here $m = 3$ and the stream is `AABACAADBBAEABFAGB` ($N = 18$).

Each reported element carries the interval `[count - ε .. count]` that contains its true frequency. The answers certify themselves: the top-2 is guaranteed to be correct and correctly ordered, while `G` may or may not be frequent (its true count is 1), so the frequent answer is not guaranteed.

## Test Coverage

`xmake test` runs four tests:

1. **`test_space_saving`:** checks the paper's examples (Example 1 / Fig. 2, and the permutation example of Sec. 3.2), then runs 3,000 random skewed streams against exact counts. On every stream it verifies the Stream-Summary invariants, Lemma 1 ($\mathit{min} \le N/m$), the bounds $\mathrm{count}_e - \varepsilon_e \le F_e \le \mathrm{count}_e$, Theorems 1 and 2, and that every `guaranteed` and `order` flag returned by Fig. 3 and Fig. 4 is correct.
2. **`test_baselines`:** checks the error bound of Frequent on 500 random streams, and that CountSketch, GroupTest and Probabilistic-InPlace recover planted heavy hitters.
3. **`paper_example`:** runs the worked example.
4. **`zipf_experiment` (smoke):** runs the experiment with $N = 2 \times 10^5$.

The code is also clean under AddressSanitizer and UBSan:

```sh
xmake f -m debug --cc=gcc-15 --cxx=g++-15 --policies=build.sanitizer.address,build.sanitizer.undefined
xmake && xmake test
```

and under clang-tidy with the checks in `.clang-tidy`:

```sh
xmake project -k compile_commands build
run-clang-tidy -quiet -p build
```

## Experimental Setup

`zipf_experiment` follows Sec. 5 of the paper: synthetic Zipf($\alpha$) streams with $\alpha \in \{0, 0.5, \dots, 3\}$, $N = 10^7$, an alphabet of $10^6$ elements (ids shuffled), $\phi = 0.01$, $\varepsilon = \phi/100$, $k = 100$, and $\delta = 0.01$ for the sketches. Timings cover the update loop only, single-threaded.

| Algorithm | Space |
|---|---|
| Space-Saving, frequent | $m = \min(\lvert A\rvert, (1/\varepsilon)^{1/\alpha}, 1/\varepsilon)$ (Theorem 4) |
| Space-Saving, top-k | $m = \min(\lvert A\rvert, (k/\alpha)^{1/\alpha} k)$ for $\alpha > 1$, $\min(\lvert A\rvert, k^2 \ln\lvert A\rvert)$ otherwise (Theorem 6) |
| Frequent | $m = 1/\phi$ |
| GroupTest | $\lceil \ln\frac{1}{\delta\phi} \rceil$ tests $\times$ $2/\phi$ groups $\times$ 21 counters |
| CountSketch | $\lceil \ln(N/\delta) \rceil$ rows $\times$ $16k$ columns (the paper's "hidden factor of 16") |
| Probabilistic-InPlace | the same $m$ as Space-Saving |

Parameters can be changed on the command line, for example:

```sh
xmake run zipf_experiment --n=1000000 --alphabet=100000 --k=50 --topk-m=2000
```

## Results

Results obtained on Linux x86-64 with GCC 15.2. Precision is the fraction of reported elements that are correct; recall is the fraction of correct elements that are reported.

**Frequent elements** (cf. Fig. 5):

|  α  | algorithm    | counters | time (ms) | precision | recall | guaranteed | memory (KiB) |
|----:|--------------|---------:|----------:|----------:|-------:|:----------:|-------------:|
| 0.0 | Space-Saving |    10000 |       373 |     1.000 |  1.000 |    yes     |        783.5 |
| 0.0 | Frequent     |      100 |       348 |     0.000 |  1.000 |     -      |          3.6 |
| 0.0 | GroupTest    |    42000 |       527 |     1.000 |  1.000 |     -      |        328.3 |
| 1.0 | Space-Saving |    10000 |       287 |     1.000 |  1.000 |    yes     |        828.8 |
| 1.0 | Frequent     |      100 |       322 |     0.162 |  1.000 |     -      |          2.0 |
| 1.0 | GroupTest    |    42000 |       510 |     0.750 |  1.000 |     -      |        328.3 |
| 2.0 | Space-Saving |      100 |        73 |     1.000 |  1.000 |    yes     |         10.5 |
| 2.0 | Frequent     |      100 |        42 |     0.070 |  1.000 |     -      |          3.9 |
| 2.0 | GroupTest    |    42000 |       527 |     1.000 |  1.000 |     -      |        328.3 |
| 3.0 | Space-Saving |       22 |        43 |     1.000 |  1.000 |    yes     |          2.6 |
| 3.0 | Frequent     |      100 |        37 |     0.048 |  1.000 |     -      |          3.4 |
| 3.0 | GroupTest    |    42000 |       420 |     1.000 |  1.000 |     -      |        328.3 |

**Top-100** (cf. Fig. 6):

|  α  | algorithm             | counters | time (ms) | precision | recall | guaranteed | order | memory (KiB) |
|----:|-----------------------|---------:|----------:|----------:|-------:|:----------:|:-----:|-------------:|
| 0.0 | Space-Saving          |   138156 |       573 |     0.000 |  0.000 |     no     |  no   |      10869.0 |
| 0.0 | CountSketch           |    33600 |      2174 |     0.010 |  0.010 |     -      |   -   |        272.0 |
| 0.0 | Probabilistic-InPlace |   138156 |       259 |     0.001 |  0.750 |     -      |   -   |       3470.0 |
| 1.0 | Space-Saving          |   138156 |       353 |     1.000 |  1.000 |    yes     |  yes  |      10937.9 |
| 1.0 | CountSketch           |    33600 |      2063 |     0.990 |  0.990 |     -      |   -   |        272.0 |
| 1.0 | Probabilistic-InPlace |   138156 |       259 |     0.001 |  1.000 |     -      |   -   |       2666.8 |
| 2.0 | Space-Saving          |      708 |        70 |     1.000 |  1.000 |    yes     |  yes  |         67.6 |
| 2.0 | CountSketch           |    33600 |      1084 |     1.000 |  1.000 |     -      |   -   |        272.0 |
| 2.0 | Probabilistic-InPlace |      708 |        48 |     0.282 |  1.000 |     -      |   -   |         12.9 |
| 3.0 | Space-Saving          |      322 |        68 |     1.000 |  1.000 |    yes     |  yes  |         26.0 |
| 3.0 | CountSketch           |    33600 |       922 |     1.000 |  1.000 |     -      |   -   |        272.0 |
| 3.0 | Probabilistic-InPlace |      322 |        31 |     0.596 |  0.960 |     -      |   -   |          4.6 |

The qualitative claims of the paper hold:

- Space-Saving reports every frequent element with precision and recall 1 for all $\alpha$, and certifies the answer. Frequent cannot filter its candidates and reaches a precision of only 0.05–0.16 (paper: 0.05–0.22).
- Space-Saving returns the exact top-100 in the correct order, with a guarantee, for every $\alpha \ge 0.5$, and runs 4–18× faster than CountSketch (paper: 30–82×).
- The space needed by Space-Saving falls sharply with skew: 22 counters at $\alpha = 3$, 126× less memory than GroupTest.
- On uniform data ($\alpha = 0$) there is no meaningful top-k, and Space-Saving reports `guaranteed = no`.

The one claim that does not hold with the theorem budgets is that Space-Saving always uses the least space: the bounds for $\alpha \le 1$ are loose. With `--topk-m=2000` (about 60% of CountSketch's memory), Space-Saving still certifies the exact, ordered top-100 for every $\alpha \ge 1$.

## Implementation Notes

Where the paper leaves details open or is inconsistent, this project makes the following choices:

- **QueryFrequent (Fig. 3):** in addition to checking $\mathrm{count}_i - \varepsilon_i > \phi N$ for every reported element, the answer is guaranteed only if $\mathit{min} \le \phi N$. Otherwise an unmonitored element could still be frequent. This only matters when $m < 1/\phi$.
- **QueryTop-k (Fig. 4):** the two loops are merged into one with the same meaning; past the last counter, $\mathrm{count}_{m+1} = \mathit{min}$.
- **Stream-Summary:** a bucket with a single element changes its value in place instead of creating a new bucket and deleting the old one.
- **Frequent:** decrement-all is $O(m)$ but happens at most $N/(m+1)$ times, so updates are amortized $O(1)$.
- **GroupTest:** a recovered id is reported only if it hashes back to the group it was recovered from and all $t$ groups containing it exceed $\phi N$. Counters are 8 bytes, so memory is twice the paper's.
- **CountSketch:** candidates are re-estimated on every arrival, which makes it slightly more accurate than the original.
- **Probabilistic-InPlace:** a query treats the current round as finished.
- **Stream length:** the text of the paper uses $N = 10^7$ while the caption of Fig. 5 says "100,000 Hits"; this project uses $N = 10^7$.
- **Top-k space for $\alpha < 1$:** the paper gives no Zipf bound, so the $\alpha = 1$ bound $k^2 \ln\lvert A\rvert$ is used; `--topk-m` overrides it.

## Using the Library

```cpp
#include <space_saving/space_saving.hpp>

ss::SpaceSaving<std::string> s{1000};   // m = 1000 counters
for (const auto& x : stream) s.insert(x);
auto top  = s.query_top_k(10);          // top.elements, top.guaranteed, top.order
auto freq = s.query_frequent(0.01);     // freq.elements, freq.guaranteed
auto e    = s.estimate("foo");          // std::optional<Entry>, O(1)
```
