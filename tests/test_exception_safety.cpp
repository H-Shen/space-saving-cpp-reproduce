// Failure injection: an insert either completes or changes nothing, when an
// allocation fails (via a replaced global operator new) or when copying a key
// throws. Every possible failure point of every insert is tried in turn.
#include <space_saving/space_saving.hpp>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <new>
#include <print>
#include <random>
#include <source_location>
#include <string_view>

namespace {

// Injected failures: with a budget of $b \ge 0$, the next $b$ operations succeed and
// the one after throws. A budget of -1 never fails.
long allocations_left = -1;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
long copies_left = -1;       // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

bool spend(long& budget) {
    if (budget == 0) return false;
    if (budget > 0) --budget;
    return true;
}

struct CopyFailure : std::exception {};

/// A key whose copies can be made to fail; moving it never throws.
struct Key {
    int value = 0;

    explicit Key(int v) : value{v} {}
    Key(const Key& other) : value{other.value} {
        if (!spend(copies_left)) throw CopyFailure{};
    }
    Key& operator=(const Key& other) {
        if (this != &other) value = Key{other}.value;
        return *this;
    }
    Key(Key&&) noexcept = default;
    Key& operator=(Key&&) noexcept = default;
    ~Key() = default;
    friend bool operator==(const Key&, const Key&) = default;
};

struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept { return std::hash<int>{}(k.value); }
};

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

/// Inserts a random stream; each insert is first made to fail at its 1st, 2nd, ...
/// injected operation until it succeeds. Every failed insert must leave the summary
/// unchanged, and the end result must equal a summary built without failures.
/// Returns the number of injected failures.
template <class Failure>
long run_with_failures(Checker& t, std::mt19937_64& rng, long& budget) {
    const auto m = std::uniform_int_distribution<std::size_t>{1, 8}(rng);
    const auto n = std::uniform_int_distribution{1, 200}(rng);
    std::uniform_int_distribution pick{0, std::uniform_int_distribution{1, 20}(rng)};

    ss::SpaceSaving<Key, KeyHash> s{m};
    ss::SpaceSaving<Key, KeyHash> reference{m};
    long failures = 0;
    for (int i = 0; i < n; ++i) {
        const Key x{pick(rng)};
        reference.insert(x);
        for (long b = 0;; ++b) {
            const auto before = s.sorted();
            const auto length = s.stream_length();
            budget = b;
            try {
                s.insert(x);
                budget = -1;
                break;
            } catch (const Failure&) {
                budget = -1;
                ++failures;
                t.expect(s.sorted() == before && s.stream_length() == length, "a failed insert changes nothing");
                t.expect_valid(s);
            }
        }
    }
    t.expect(s.sorted() == reference.sorted() && s.stream_length() == reference.stream_length(),
             "same summary as without failures");
    t.expect_valid(s);
    return failures;
}

}  // namespace

// GCC cannot tell that this operator new pairs with the free() below.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t size) {
    if (!spend(allocations_left)) throw std::bad_alloc{};
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*size*/) noexcept { ::operator delete(p); }
#pragma GCC diagnostic pop

int main() {  // NOLINT(bugprone-exception-escape): a test may terminate on an unexpected exception
    Checker t;
    std::mt19937_64 rng{704};  // NOLINT(bugprone-random-generator-seed): reproducible tests
    long allocation_failures = 0;
    long copy_failures = 0;
    for (int trial = 0; trial < 300; ++trial) {
        allocation_failures += run_with_failures<std::bad_alloc>(t, rng, allocations_left);
        copy_failures += run_with_failures<CopyFailure>(t, rng, copies_left);
    }
    t.expect(allocation_failures > 0 && copy_failures > 0, "failures were injected");
    std::println("[{}] strong guarantee under {} allocation and {} key-copy failures",
                 t.failures() == 0 ? " OK " : "FAIL", allocation_failures, copy_failures);
    if (t.failures() != 0) {
        std::println("{} check(s) failed", t.failures());
        return EXIT_FAILURE;
    }
    std::println("all tests passed");
}
