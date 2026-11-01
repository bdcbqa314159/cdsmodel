// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
//
// Two kinds of assertion:
//   1. Port semantics that stand on their own (constexpr use, order checking).
//   2. Replay of reference answers recorded from the oracle into
//      tests/golden/bsearch_cases.hpp, so the port stays pinned to upstream
//      behaviour on machines that cannot legally obtain upstream.

#include <cdsmodel/bsearch.hpp>

#include "golden/bsearch_cases.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string_view>

namespace {

template <typename T, std::size_t N>
void replay(const std::array<std::span<const T>, N> &domains, const auto &cases,
            std::string_view tag) {
  for (const cdsmodel::golden::Case &c : cases) {
    ASSERT_LT(c.domain, domains.size()) << tag;
    const std::span<const T> xs = domains[c.domain];
    const auto got = cdsmodel::bracket(xs, c.desired);

    ASSERT_EQ(got.has_value(), c.ok)
        << tag << ": success/failure disagreement, domain=" << c.domain << " desired=" << c.desired;
    if (c.ok) {
      EXPECT_EQ(got->lo, static_cast<std::size_t>(c.lo))
          << tag << ": lo, domain=" << c.domain << " desired=" << c.desired;
      EXPECT_EQ(got->hi, static_cast<std::size_t>(c.hi))
          << tag << ": hi, domain=" << c.domain << " desired=" << c.desired;
    }
  }
}

TEST(BSearchGolden, DoublesMatchRecordedReference) {
  replay<double>(cdsmodel::golden::kDoubleDomains, cdsmodel::golden::kDoubleCases, "double");
}

TEST(BSearchGolden, LongsMatchRecordedReference) {
  replay<long>(cdsmodel::golden::kLongDomains, cdsmodel::golden::kLongCases, "long");
}

TEST(BSearchGolden, VectorSetIsNotEmpty) {
  // Guards against a truncated or half-regenerated golden header silently
  // turning the replay tests into no-ops.
  EXPECT_GT(cdsmodel::golden::kDoubleCases.size(), 200u);
  EXPECT_GT(cdsmodel::golden::kLongCases.size(), 100u);
}

// --- Port semantics that need no oracle -----------------------------------

TEST(BSearchOrderCheck, StrictIncreaseRejectsEqualNeighbours) {
  constexpr std::array<double, 3> ok{1.0, 2.0, 3.0};
  constexpr std::array<double, 4> equal_neighbours{1.0, 2.0, 2.0, 3.0};
  constexpr std::array<double, 3> decreasing{3.0, 2.0, 1.0};

  EXPECT_TRUE(cdsmodel::is_strictly_increasing(std::span<const double>{ok}));
  EXPECT_FALSE(cdsmodel::is_strictly_increasing(std::span<const double>{equal_neighbours}));
  EXPECT_FALSE(cdsmodel::is_strictly_increasing(std::span<const double>{decreasing}));
}

TEST(BSearchOrderCheck, CheckedVariantRejectsUnorderedInput) {
  constexpr std::array<double, 3> bad{5.0, 1.0, 9.0};
  EXPECT_FALSE(cdsmodel::bracket_checked(std::span<const double>{bad}, 3.0).has_value());
}

TEST(BSearchEdges, EmptyDomainFails) {
  EXPECT_FALSE(cdsmodel::bracket(std::span<const double>{}, 1.0).has_value());
}

TEST(BSearchConstexpr, UsableAtCompileTime) {
  // Upstream could not be: it writes through out-params and reads a global flag.
  static constexpr std::array<double, 4> xs{1.0, 2.0, 3.0, 4.0};
  static_assert(cdsmodel::bracket(std::span<const double>{xs}, 2.5) == cdsmodel::Bracket{1, 2});
  static_assert(cdsmodel::bracket(std::span<const double>{xs}, 0.0) == cdsmodel::Bracket{0, 1});
  static_assert(cdsmodel::bracket(std::span<const double>{xs}, 9.0) == cdsmodel::Bracket{2, 3});
  static_assert(cdsmodel::is_strictly_increasing(std::span<const double>{xs}));
  SUCCEED();
}

} // namespace
