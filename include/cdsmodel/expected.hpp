#pragma once

/// A1 -- the error channel. See specs/01_error_channel.md.
///
/// `std::expected` is C++23; this project is a C++20 port and stays one, so
/// this is a minimal mirror. Its API is a **strict subset** of
/// `std::expected`'s, which is what gives the duplication an end date: when
/// the project moves to C++23 for its own reasons, `Expected` and `unexpected`
/// become aliases and everything below is deleted. Nothing here may grow a
/// member `std::expected` lacks -- tests/expected_test.cpp compiles the same
/// call sequence against both and fails if it does.
///
/// Deliberately ABSENT: `value()`, which throws in the standard. This project
/// puts no exceptions on the core API, and omitting a member still leaves a
/// subset. Call sites check `has_value()` then use `*e`, exactly as the
/// `std::optional` code this replaces already did.

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace cdsmodel {

/// Why a call failed. Four kinds, each traceable to an upstream diagnostic --
/// see the taxonomy table in specs/01_error_channel.md. There are four and not
/// 452 because upstream checked a status 447 times and never once branched on
/// a reason: the payload is text for a human, not a code for a `switch`.
enum class ErrorKind {
  /// The caller passed something unusable: out of range, empty, mis-sized,
  /// non-finite, or a malformed aggregate. The large majority of origins.
  /// Upstream analogue: `"Lower bound (…) >= higher bound (…)"`.
  bad_argument,

  /// A value this port deliberately declines to represent -- an unknown enum
  /// code, or a day count the rate conversion does not support. Distinct from
  /// `bad_argument` because it is a capability limit, not a caller mistake.
  unsupported,

  /// Nothing was wrong with the input and no answer exists: the bounds do not
  /// bracket a root, or a degenerate year fraction leaves nothing to solve.
  /// Upstream analogue: `"Function values at bounds imply no root exists"`.
  no_root,

  /// An answer may exist but the iteration gave up.
  /// Upstream analogue: `"Maximum number of iterations exceeded"`.
  no_convergence,
};

[[nodiscard]] constexpr std::string_view to_string(ErrorKind kind) noexcept {
  switch (kind) {
  case ErrorKind::bad_argument:
    return "bad argument";
  case ErrorKind::unsupported:
    return "unsupported";
  case ErrorKind::no_root:
    return "no root";
  case ErrorKind::no_convergence:
    return "no convergence";
  }
  return "unknown"; // unreachable for a valid enumerator
}

/// A kind plus a message naming the offending value.
///
/// There is no default constructor and none from a kind alone, which is A7
/// enforced by the type rather than by review: a 341-site mechanical migration
/// would otherwise happily produce contextless errors that compile cleanly and
/// tell a user nothing. The message is owned because upstream's 452
/// diagnostics interpolate runtime values, so the context *is* the value.
struct Error {
  ErrorKind kind;
  std::string message;

  Error() = delete;
  Error(ErrorKind k, std::string m) : kind(k), message(std::move(m)) {}

  [[nodiscard]] friend bool operator==(const Error &, const Error &) = default;
};

/// Mirror of `std::unexpected`. Aliased away with `Expected` on the move to
/// C++23.
template <typename E> class unexpected {
public:
  constexpr explicit unexpected(E e) : error_(std::move(e)) {}

  [[nodiscard]] constexpr const E &error() const & noexcept { return error_; }
  [[nodiscard]] constexpr E &&error() && noexcept { return std::move(error_); }

private:
  E error_;
};

template <typename E> unexpected(E) -> unexpected<E>;

/// A value or the reason there is not one.
///
/// Implemented over `std::variant` rather than a hand-managed union: the point
/// of this type is to carry error information reliably, and a bespoke
/// lifetime implementation would be the one piece of the port with no oracle
/// behind it.
///
/// Accessors go through `std::get<I>`, never `std::get_if<I>`. `get_if` takes
/// the variant's ADDRESS, and libstdc++ rejects `&v != 0` inside a constant
/// expression -- so a `static_assert` on an `Expected` compiles under libc++
/// and fails under libstdc++. Found by CI on Ubuntu after the macOS build was
/// green; `tests/stub_test.cpp` has the constexpr uses that caught it.
template <typename T, typename E = Error> class Expected {
public:
  using value_type = T;
  using error_type = E;

  constexpr Expected(T v) : slot_(std::in_place_index<0>, std::move(v)) {}
  constexpr Expected(unexpected<E> u) : slot_(std::in_place_index<1>, std::move(u).error()) {}

  [[nodiscard]] constexpr bool has_value() const noexcept { return slot_.index() == 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] constexpr const T &operator*() const & noexcept { return std::get<0>(slot_); }
  [[nodiscard]] constexpr T &operator*() & noexcept { return std::get<0>(slot_); }
  [[nodiscard]] constexpr T &&operator*() && noexcept { return std::move(std::get<0>(slot_)); }

  [[nodiscard]] constexpr const T *operator->() const noexcept {
    return std::addressof(std::get<0>(slot_));
  }
  [[nodiscard]] constexpr T *operator->() noexcept { return std::addressof(std::get<0>(slot_)); }

  [[nodiscard]] constexpr const E &error() const & noexcept { return std::get<1>(slot_); }
  [[nodiscard]] constexpr E &&error() && noexcept { return std::move(std::get<1>(slot_)); }

  template <typename U> [[nodiscard]] constexpr T value_or(U &&fallback) const & {
    return has_value() ? **this : static_cast<T>(std::forward<U>(fallback));
  }

  /// Chains a call that may itself fail. On failure the ORIGINAL error is
  /// forwarded with its kind and message untouched -- D5: propagation is not a
  /// kind, and a wrapper invented at each of 149 layers would bury the one
  /// reason the caller needs.
  template <typename F> [[nodiscard]] constexpr auto and_then(F &&f) const & {
    using Result = std::remove_cvref_t<std::invoke_result_t<F, const T &>>;
    if (!has_value()) {
      return Result{unexpected<E>(error())};
    }
    return std::forward<F>(f)(**this);
  }

  /// Comparisons `std::expected` also provides, so adding them keeps the
  /// subset property. They are what lets `EXPECT_EQ(f(x), 3.5)` read the same
  /// as it did against `std::optional`, which is most of why this migration
  /// did not have to rewrite the test suite.
  [[nodiscard]] friend constexpr bool operator==(const Expected &a, const Expected &b) {
    if (a.has_value() != b.has_value()) {
      return false;
    }
    return a.has_value() ? *a == *b : a.error() == b.error();
  }

  /// Constrained away from `Expected` and `unexpected` so it does not go
  /// ambiguous with the two exact overloads above when both sides are
  /// `Expected` -- which is what `EXPECT_EQ(a, b)` does.
  template <typename U>
    requires(!std::is_same_v<std::remove_cvref_t<U>, Expected> &&
             !std::is_same_v<std::remove_cvref_t<U>, unexpected<E>>)
  [[nodiscard]] friend constexpr bool operator==(const Expected &a, const U &v) {
    return a.has_value() && *a == v;
  }

  [[nodiscard]] friend constexpr bool operator==(const Expected &a, const unexpected<E> &u) {
    return !a.has_value() && a.error() == u.error();
  }

private:
  std::variant<T, E> slot_;
};

/// Void specialisation, for the sites that only report whether something held.
template <typename E> class Expected<void, E> {
public:
  using value_type = void;
  using error_type = E;

  constexpr Expected() : slot_(std::in_place_index<0>) {}
  constexpr Expected(unexpected<E> u) : slot_(std::in_place_index<1>, std::move(u).error()) {}

  [[nodiscard]] constexpr bool has_value() const noexcept { return slot_.index() == 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] constexpr const E &error() const & noexcept { return std::get<1>(slot_); }
  [[nodiscard]] constexpr E &&error() && noexcept { return std::move(std::get<1>(slot_)); }

  template <typename F> [[nodiscard]] constexpr auto and_then(F &&f) const & {
    using Result = std::remove_cvref_t<std::invoke_result_t<F>>;
    if (!has_value()) {
      return Result{unexpected<E>(error())};
    }
    return std::forward<F>(f)();
  }

private:
  std::variant<std::monostate, E> slot_;
};

namespace detail {

/// Formats a number for an error message in upstream's `%2.6e` style, so a
/// port diagnostic and the oracle's diagnostic for the same failure read
/// alike (D4). `std::format` is deliberately not used: it is C++20 but its
/// library support is uneven across this project's three CI compilers, and a
/// message helper is not worth a portability question.
[[nodiscard]] inline std::string num(double v) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof buf, "%2.6e", v);
  return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string("?");
}

/// One constrained template rather than an overload per width: a set of
/// `int`/`long`/`size_t` overloads is ambiguous for every other integral type,
/// which is a compile error at the message site and nowhere near the cause.
template <typename I>
  requires std::is_integral_v<I>
[[nodiscard]] std::string num(I v) {
  return std::to_string(v);
}

} // namespace detail

/// Shorthand for the overwhelmingly common construction at an origin site.
[[nodiscard]] inline unexpected<Error> fail(ErrorKind kind, std::string message) {
  return unexpected<Error>(Error{kind, std::move(message)});
}

} // namespace cdsmodel
