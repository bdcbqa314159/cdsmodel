#pragma once

/// C10 of specs/03_xll.md -- reading a two-column label/value block.
///
/// This is what replaces the repeated six arguments. Measured on upstream's
/// own XLL: across 8 priced functions there are 111 arguments covering ~38
/// distinct concepts, and the recurring block -- `Holidays` in 8 of 8,
/// `Coupon Interval`, `Stub Type`, `Payment DCC` and `Bad Day Convention` in
/// 7 of 8 -- is a conventions struct with nowhere to live. In a grid it lives
/// in cells a reader can see:
///
///     Coupon Interval    3M
///     Stub Type          f/s
///     Payment DCC        ACT/360
///
/// The rule that matters is C10: an unknown or duplicated label is an ERROR
/// naming it, never a default quietly used. A silently ignored `Payment DCC`
/// row would price a different trade than the sheet appears to say, which is
/// worse than the 17 positional arguments this replaces.

#include <cdsmodel/expected.hpp>
#include <cdsmodel/xl/value.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cdsmodel::xl {

/// One recognised label and where its value goes.
struct LabelSpec {
  std::string_view label;
  bool required = true;
};

/// A label/value block, read and validated.
class LabelledRange {
public:
  /// Reads `grid` against `expected`.
  ///
  /// Rows may appear in ANY order -- that is the point of labelling them.
  /// Label matching is case-insensitive and ignores surrounding whitespace,
  /// because a human types these; it is NOT fuzzy, because "close enough"
  /// matching is how a typo becomes a different trade.
  [[nodiscard]] static Expected<LabelledRange> read(const XlGrid &grid,
                                                    std::span<const LabelSpec> expected);

  /// The value for `label`, or an error if it was optional and absent.
  [[nodiscard]] Expected<XlValue> value_of(std::string_view label) const;

  /// Whether an optional label was supplied at all.
  [[nodiscard]] bool has(std::string_view label) const;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  struct Entry {
    std::string label; // normalised
    XlValue value;
  };
  std::vector<Entry> entries_;
};

/// Normalises a label for comparison: trimmed, lower-cased, and internal runs
/// of whitespace collapsed to one space -- so "Payment  DCC" and "payment dcc"
/// are the same label, while "Payment DC" is not and is reported.
[[nodiscard]] std::string normalise_label(std::string_view raw);

} // namespace cdsmodel::xl
