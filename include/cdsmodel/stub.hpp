#pragma once

// Port of upstream stub.c -- where the stub goes when a schedule does not
// divide evenly into whole periods, and how its string form is parsed.
//
// Upstream's TStubMethod is two independent TBooleans, `stubAtEnd` and
// `longStub`, and every caller has to remember which boolean means what. The
// port splits them into two enums so that `position` and `length` cannot be
// swapped at a call site: StubMethod{StubLength::Long} does not compile.
//
// The parser carries one defect and one documentation divergence; both are
// recorded below.

#include <optional>
#include <string_view>

namespace cdsmodel {

/// Which end of the schedule the odd period sits at.
enum class StubPosition { Front, Back };

/// Whether the odd period absorbs an adjacent regular one.
///
/// Short: the stub is the leftover, shorter than a regular period.
/// Long: the leftover is merged into its neighbour, giving one period longer
/// than regular instead of two periods one of which is short.
enum class StubLength { Short, Long };

/// Replaces upstream's `TStubMethod`.
struct StubMethod {
  StubPosition position = StubPosition::Front;
  StubLength length = StubLength::Short;

  friend constexpr bool operator==(const StubMethod &, const StubMethod &) = default;
};

/// Parse a stub method. Replaces `JpmcdsStringToStubMethod`.
///
/// Accepted form is LOCATION[/LENGTH], where only the first character of each
/// field is examined: LOCATION is F or B, LENGTH is S or L. So "f/s", "F/S",
/// "front/short" and "f/sausage" all parse identically, and so do "b" and
/// "back". That leniency is upstream's and is PRESERVED -- the one caller in
/// the shipped distribution passes the lowercase "f/s".
///
/// Two behaviours worth naming, both preserved:
///
///  * "f" succeeds (absent LENGTH defaults to Short) but "f/" FAILS. Upstream
///    distinguishes a missing field, where the parser hands back a null
///    pointer, from an empty one, where it hands back "" and the switch on
///    its first character falls through to the error branch. The asymmetry is
///    observable at a public API, so it stays.
///
///  * Upstream's own error message documents the format as
///    "LOCATION/TYPE/[LENGTH]" -- three fields. The parser splits on a single
///    '/' and reads field two as LENGTH, so there is no TYPE field and any
///    third field is silently ignored: "f/n/l" is read as LENGTH='n' and
///    rejected, never reaching the 'l'. The code is the specification here;
///    the message describes an interface that was never implemented.
///
/// UPSTREAM-9 is NOT reproduced. The malformed-LENGTH branch formats that
/// same two-%s message with a single argument, so vsprintf consumes an
/// indeterminate varargs slot and dereferences it as a string -- an
/// out-of-bounds read on the only input class that reaches the branch. The
/// port returns nullopt and formats nothing, so the defect cannot occur.
[[nodiscard]] constexpr std::optional<StubMethod>
parse_stub_method(std::string_view name) noexcept {
  // Upstream uses toupper() on a char, which is undefined for negative values
  // -- any byte above 0x7F on a platform with signed char. Comparing both
  // cases directly is locale-independent and has no such domain.
  const auto match = [](char c, char lower, char upper) { return c == lower || c == upper; };

  const std::size_t slash = name.find('/');
  const std::string_view location = name.substr(0, slash);
  if (location.empty()) {
    return std::nullopt;
  }

  StubMethod stub;
  if (match(location.front(), 'f', 'F')) {
    stub.position = StubPosition::Front;
  } else if (match(location.front(), 'b', 'B')) {
    stub.position = StubPosition::Back;
  } else {
    return std::nullopt;
  }

  if (slash == std::string_view::npos) {
    return stub; // no LENGTH field at all -- upstream defaults to short
  }
  const std::string_view length = name.substr(slash + 1);
  if (length.empty()) {
    return std::nullopt; // "f/" -- present but empty, which upstream rejects
  }
  if (match(length.front(), 's', 'S')) {
    stub.length = StubLength::Short;
  } else if (match(length.front(), 'l', 'L')) {
    stub.length = StubLength::Long;
  } else {
    return std::nullopt;
  }
  return stub;
}

} // namespace cdsmodel
