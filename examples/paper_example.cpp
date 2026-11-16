// Prints the Stream-Summary after every arrival for the paper's small examples.
#include <space_saving/space_saving.hpp>

#include <format>
#include <iterator>
#include <print>
#include <ranges>
#include <string>
#include <string_view>

namespace {

/// The Stream-Summary bucket by bucket, largest count first: "[2: Z(ε=1) Y(ε=0)] [1: X(ε=0)]".
std::string buckets(const ss::SpaceSaving<char>& s) {
    std::string out;
    const auto same_count = [](const auto& a, const auto& b) { return a.count == b.count; };
    for (const auto bucket : s.sorted() | std::views::chunk_by(same_count)) {
        std::format_to(std::back_inserter(out), "[{}:", bucket.front().count);
        for (const auto& e : bucket) std::format_to(std::back_inserter(out), " {}(ε={})", e.item, e.error);
        out += "] ";
    }
    return out;
}

void trace(std::string_view stream, std::size_t m) {
    std::println("m = {}, S = {}", m, stream);
    ss::SpaceSaving<char> s{m};
    for (const char c : stream) {
        s.insert(c);
        std::println("  {} -> {}   min = {}", c, buckets(s), s.min_count());
    }
}

void print_entries(std::string_view label, const auto& entries) {
    std::print("  {:<28}", label);
    for (const auto& e : entries) std::print(" {}[{}..{}]", e.item, e.guaranteed(), e.count);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape): demo may terminate on an unexpected exception
    std::println("== Example 1 / Fig. 2 ==");
    trace("XYYZ", 2);

    std::println("\n== Lemma 1: min depends on the permutation ==");
    trace("XZYY", 2);

    std::println("\n== Queries (entries shown as item[count-eps..count]) ==");
    ss::SpaceSaving<char> s{3};
    for (const char c : std::string_view{"AABACAADBBAEABFAGB"}) s.insert(c);
    std::println("  summary: {}", buckets(s));

    const auto top = s.query_top_k(2);
    print_entries("top-2:", top.elements);
    std::println("  guaranteed={} order={}", top.guaranteed, top.order);

    const auto freq = s.query_frequent(0.2);
    print_entries("frequent (phi = 0.2):", freq.elements);
    std::println("  guaranteed={}", freq.guaranteed);
}
