// A1 -- cdsmodel::Expected<T> and cdsmodel::Error.
//
// The criterion has two halves and the second is the one that matters: the API
// must be a STRICT SUBSET of std::expected's, so that the day the project
// moves to C++23 this header becomes an alias and a deletion. A hand-rolled
// type that drifts from the standard one is debt with no end date, which is
// the objection this design exists to answer.
//
// So the same call sequences are written twice -- once against
// cdsmodel::Expected and once against std::expected -- in a shared template
// that only compiles if both satisfy it. Under -std=c++20 only the first
// instantiation is built; the asan preset adds the C++23 check where available.

#include <cdsmodel/expected.hpp>

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

#if defined(__has_include)
#if __has_include(<expected>) && __cplusplus > 202002L
#include <expected>
#define CDSMODEL_HAVE_STD_EXPECTED 1
#endif
#endif

namespace {

using cdsmodel::Error;
using cdsmodel::ErrorKind;
using cdsmodel::Expected;
using cdsmodel::unexpected;

// The subset contract, written once. Instantiated below for both types; a
// member this uses that std::expected lacks would fail to compile there.
// The error-state constructor is parameterised too, because `std::unexpected`
// is itself C++23 -- so the mirror needs its own, and the swap later aliases
// it. `value()` is deliberately ABSENT: std::expected::value() throws, this
// project puts no exceptions on the core API, and OMITTING a member still
// leaves a strict subset. Call sites use `*e` after checking, which is what
// the optional-based code they replace already did.
template <template <typename, typename> class E, template <typename> class U>
void subset_contract() {
  E<int, Error> good{42};
  E<int, Error> bad{U<Error>{Error{ErrorKind::bad_argument, "no"}}};

  static_assert(std::is_same_v<decltype(good.has_value()), bool>);
  (void)good.has_value();
  (void)static_cast<bool>(good);
  (void)*good;
  (void)bad.error();
  (void)good.value_or(0);
  (void)good.and_then([](int v) { return E<long, Error>{v + 1L}; });
}

template void subset_contract<Expected, cdsmodel::unexpected>();
#ifdef CDSMODEL_HAVE_STD_EXPECTED
template void subset_contract<std::expected, std::unexpected>();
#endif

TEST(ExpectedType, HoldsAValue) {
  const Expected<int, Error> e{7};
  ASSERT_TRUE(e.has_value());
  EXPECT_TRUE(static_cast<bool>(e));
  EXPECT_EQ(*e, 7);
  EXPECT_EQ(e.value_or(99), 7);
}

TEST(ExpectedType, HoldsAnError) {
  const Expected<int, Error> e{unexpected(Error{ErrorKind::no_root, "no root in [1,2]"})};
  ASSERT_FALSE(e.has_value());
  EXPECT_FALSE(static_cast<bool>(e));
  EXPECT_EQ(e.error().kind, ErrorKind::no_root);
  EXPECT_EQ(e.error().message, "no root in [1,2]");
  EXPECT_EQ(e.value_or(99), 99);
}

// A4's mechanism in miniature: and_then must forward the error untouched,
// including its kind, rather than inventing a wrapper.
TEST(ExpectedType, AndThenForwardsTheErrorUnchanged) {
  const Expected<int, Error> bad{unexpected(Error{ErrorKind::no_convergence, "gave up after 100"})};
  const Expected<double, Error> chained =
      bad.and_then([](int v) { return Expected<double, Error>{v * 1.5}; });
  ASSERT_FALSE(chained.has_value());
  EXPECT_EQ(chained.error().kind, ErrorKind::no_convergence) << "the kind was rewritten";
  EXPECT_EQ(chained.error().message, "gave up after 100") << "the message was lost";
}

TEST(ExpectedType, AndThenRunsOnSuccess) {
  const Expected<int, Error> good{4};
  const Expected<double, Error> chained =
      good.and_then([](int v) { return Expected<double, Error>{v / 2.0}; });
  ASSERT_TRUE(chained.has_value());
  EXPECT_EQ(*chained, 2.0);
}

// Void specialisation: 341 sites include predicates that only report failure.
TEST(ExpectedType, SupportsVoid) {
  const Expected<void, Error> ok{};
  EXPECT_TRUE(ok.has_value());
  const Expected<void, Error> bad{unexpected(Error{ErrorKind::unsupported, "ACT/366"})};
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().kind, ErrorKind::unsupported);
}

// A7's requirement, enforced at the type: an Error cannot be built without a
// message. If the message could be defaulted, a 341-site mechanical pass would
// produce contextless errors that type-check fine and tell a user nothing.
TEST(ExpectedType, AnErrorCannotBeBuiltWithoutAMessage) {
  static_assert(!std::is_default_constructible_v<Error>,
                "Error must not be default-constructible; a message is mandatory");
  static_assert(!std::is_constructible_v<Error, ErrorKind>,
                "Error must not be constructible from a kind alone");
  static_assert(std::is_constructible_v<Error, ErrorKind, std::string>);
}

// The gap that let a portability bug reach CI: nothing here used an
// `Expected` in a constant expression, so `std::get_if`'s address-taking --
// which libc++ tolerates and libstdc++ rejects inside constexpr -- compiled
// fine locally and broke on Ubuntu. These static_asserts run at compile time
// on every platform the matrix covers.
TEST(ExpectedType, IsUsableInAConstantExpression) {
  static_assert(Expected<int, Error>{7}.has_value());
  static_assert(*Expected<int, Error>{7} == 7);
  static_assert(Expected<int, Error>{7}.value_or(0) == 7);
  static_assert(static_cast<bool>(Expected<int, Error>{7}));
  // The error side needs a literal-type error to be constexpr, so this checks
  // the branch rather than the message: a default-constructed kind compares.
  constexpr Expected<int, ErrorKind> bad{cdsmodel::unexpected(ErrorKind::no_root)};
  static_assert(!bad.has_value());
  static_assert(bad.error() == ErrorKind::no_root);
  SUCCEED();
}

TEST(ExpectedType, EveryKindHasADistinctName) {
  const ErrorKind kinds[]{ErrorKind::bad_argument, ErrorKind::unsupported, ErrorKind::no_root,
                          ErrorKind::no_convergence};
  for (const ErrorKind a : kinds) {
    EXPECT_FALSE(cdsmodel::to_string(a).empty());
    for (const ErrorKind b : kinds) {
      if (&a != &b && a != b) {
        EXPECT_NE(cdsmodel::to_string(a), cdsmodel::to_string(b));
      }
    }
  }
}

} // namespace
