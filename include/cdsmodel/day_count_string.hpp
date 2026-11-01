#pragma once

// Port of upstream yearfrac.c -- parsing and formatting day count convention
// names. Three functions, and all three have surprises in them.
//
// The parser is substring-based and deliberately permissive: upstream's own
// comment says "fairly permissive ... For example, "Actual/360", "A", etc."
// A consequence it does not mention is that unrelated text parses happily --
// "GARBAGE365" is ACT/365, "A360" is ACT/360. That leniency is PRESERVED
// here, not tightened: it is observable behaviour at a public API, and
// callers in the wild may well depend on it. A strict parser can be added
// alongside if one is ever wanted; silently rejecting input that used to be
// accepted is not a port, it is a breaking change.

#include <cdsmodel/day_count.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace cdsmodel {

/// Codes upstream parses and formats but refuses to calculate with.
/// `JpmcdsDayCountValid` rejects both, and so does `from_upstream_code`.
///
/// Note JPMCDS_DEFAULT is -1 -- numerically identical to upstream's FAILURE.
/// A function returning a day count code therefore cannot distinguish "the
/// default convention" from "I failed", which is the same sentinel-space
/// collision as JpmcdsDate returning -1 for an invalid date.
inline constexpr long kDayCountDefaultCode = -1; ///< JPMCDS_DEFAULT
inline constexpr long kDayCountNoneCode = 0;     ///< parsed from "NONE"

/// Canonical name of a convention, as `JpmcdsFormatDayCountConv` renders it.
///
/// Upstream returned `char *` pointing at a string literal -- writing through
/// it is undefined -- and logged to the global error log as a side effect of
/// formatting. This returns a view and logs nothing.
[[nodiscard]] constexpr std::string_view to_string(DayCount dcc) noexcept {
  switch (dcc) {
  case DayCount::Act365:
    return "Actual/365";
  case DayCount::Act365Fixed:
    return "Actual/365F";
  case DayCount::Act360:
    return "Actual/360";
  case DayCount::Thirty360:
    return "30/360";
  case DayCount::ThirtyE360:
    return "30E/360";
  case DayCount::EffectiveRate:
    return "Effective";
  }
  return "unrecognized";
}

/// Format a raw upstream code, including the non-calculable ones.
///
/// Reproduces `JpmcdsFormatDayCountConv` exactly, including an asymmetry
/// worth knowing about: "NONE" parses to code 0, but 0 has no `case` in
/// upstream's formatter, so it formats back as "unrecognized". Parse and
/// format do not round-trip through "NONE".
[[nodiscard]] constexpr std::string_view format_day_count_code(long code) noexcept {
  if (code == kDayCountDefaultCode) {
    return "Default";
  }
  if (const std::optional<DayCount> dcc = from_upstream_code(code)) {
    return to_string(*dcc);
  }
  return "unrecognized";
}

namespace detail {

inline constexpr std::size_t kMaxDccChars = 32; ///< upstream MAX_DCC_CHARS

/// Upstream's `JpmcdsStringPreprocess`, which does less than its callers
/// believe: it strips LEADING whitespace only, uppercases, and truncates to
/// 32 characters. Internal whitespace survives, so "ACT / 360" keeps its
/// spaces and only parses because the substring search does not care.
struct Preprocessed {
  std::array<char, kMaxDccChars + 1> chars{};
  std::size_t size = 0;

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return std::string_view{chars.data(), size};
  }
  [[nodiscard]] constexpr bool contains(std::string_view needle) const noexcept {
    return view().find(needle) != std::string_view::npos;
  }
};

[[nodiscard]] constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

[[nodiscard]] constexpr Preprocessed preprocess(std::string_view s) noexcept {
  Preprocessed out;
  std::size_t i = 0;
  while (i < s.size() && is_space(s[i])) {
    ++i;
  }
  for (; i < s.size() && out.size < kMaxDccChars; ++i) {
    const char c = s[i];
    out.chars[out.size++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  return out;
}

} // namespace detail

/// Parse to a raw upstream code, preserving full fidelity including
/// `kDayCountDefaultCode` and `kDayCountNoneCode`. Needed at a C API
/// boundary; most callers want `parse_day_count` instead.
///
/// The branch structure mirrors upstream's exactly, including the fact that
/// the first branch is entered for any string containing an 'A' and, if none
/// of its inner tests match, falls out of the whole if/else chain without
/// trying the later branches. That is how "DEFAULT" -- which contains an 'A'
/// -- skips the 30/360 and "EFF" branches and still reaches the keyword
/// comparison below.
[[nodiscard]] constexpr std::optional<long> parse_day_count_code(std::string_view s) noexcept {
  const detail::Preprocessed p = detail::preprocess(s);

  if (p.contains("A")) {
    if (p.contains("/A")) {                      // must precede the "365" test, as upstream notes
      return to_upstream_code(DayCount::Act365); // Act/Act is the Act/365 code
    }
    if (p.contains("365")) {
      if (p.contains("F")) {
        return to_upstream_code(DayCount::Act365Fixed);
      }
      return to_upstream_code(DayCount::Act365);
    }
    if (p.contains("360")) {
      return to_upstream_code(DayCount::Act360);
    }
    // deliberate fall-through, see the note above
  } else if ((p.contains("30") || p.contains("B")) && p.contains("360")) {
    if (p.contains("E")) {
      return to_upstream_code(DayCount::ThirtyE360);
    }
    return to_upstream_code(DayCount::Thirty360);
  } else if (p.contains("EFF")) {
    return to_upstream_code(DayCount::EffectiveRate);
  }

  if (p.view() == "DEFAULT") {
    return kDayCountDefaultCode;
  }
  if (p.view() == "NONE") {
    return kDayCountNoneCode;
  }
  return std::nullopt;
}

/// Parse a convention usable for calculation.
///
/// Returns nullopt both for unrecognised input and for "DEFAULT"/"NONE",
/// which upstream parses but `JpmcdsDayCountValid` rejects. Collapsing those
/// two outcomes is deliberate: a caller that wants a convention to compute
/// with cannot use either, so the distinction only matters for interop, which
/// `parse_day_count_code` serves.
[[nodiscard]] constexpr std::optional<DayCount> parse_day_count(std::string_view s) noexcept {
  const std::optional<long> code = parse_day_count_code(s);
  if (!code) {
    return std::nullopt;
  }
  return from_upstream_code(*code);
}

} // namespace cdsmodel
