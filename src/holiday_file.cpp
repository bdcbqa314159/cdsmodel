#include <cdsmodel/holiday_file.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace cdsmodel {
namespace {

/// The seven directives, in upstream's spelling.
///
/// Upstream's own doc comment misspells one of them -- it documents
/// `# THURDSAY_ALWAYS_HOLIDAY` while the code compares against
/// `# THURSDAY_ALWAYS_HOLIDAY`. The code is what works, so the code is what
/// this follows; anyone copying the comment would have written a line that
/// silently did nothing, which is UPSTREAM-41's hazard by a different route.
struct Directive {
  std::string_view line;
  Weekend bit;
  bool set; // true adds the weekday to the weekend set, false removes it
};

constexpr std::array<Directive, 7> kDirectives{{
    {"# MONDAY_ALWAYS_HOLIDAY", Weekend::Monday, true},
    {"# TUESDAY_ALWAYS_HOLIDAY", Weekend::Tuesday, true},
    {"# WEDNESDAY_ALWAYS_HOLIDAY", Weekend::Wednesday, true},
    {"# THURSDAY_ALWAYS_HOLIDAY", Weekend::Thursday, true},
    {"# FRIDAY_ALWAYS_HOLIDAY", Weekend::Friday, true},
    {"# SATURDAY_NOT_ALWAYS_HOLIDAY", Weekend::Saturday, false},
    {"# SUNDAY_NOT_ALWAYS_HOLIDAY", Weekend::Sunday, false},
}};

[[nodiscard]] std::string_view trimmed(std::string_view s) noexcept {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

[[nodiscard]] std::string upper(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    // Cast through `unsigned char`: `std::toupper` on a negative `char` is
    // undefined, which is UPSTREAM-35 and also what upstream's own
    // COPY_CAPITALIZE macro does.
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

/// A YYYYMMDD line, or nothing.
///
/// Strict where upstream is not. Upstream tests `atol(line) > 16010101`, so
/// `abc` becomes a comment and `19960704x` becomes 1996-07-04 -- the
/// UPSTREAM-39 family, where `atol` cannot distinguish a parse failure from a
/// parsed zero. A holiday file is small, hand-maintained, and load-bearing for
/// every date in a trade, so a typo must not become a missing holiday (D2).
[[nodiscard]] Expected<Date> parse_date_line(std::string_view line, std::size_t number) {
  if (line.size() != 8) {
    return fail(ErrorKind::bad_argument,
                "line " + detail::num(number) + ": \"" + std::string(line) +
                    "\" is not a date -- a holiday must be exactly 8 digits (YYYYMMDD)");
  }
  int value = 0;
  for (const char c : line) {
    if (c < '0' || c > '9') {
      return fail(ErrorKind::bad_argument, "line " + detail::num(number) + ": \"" +
                                               std::string(line) +
                                               "\" is not a date -- it contains a non-digit");
    }
    value = value * 10 + (c - '0');
  }
  const Expected<Date> date = make_date(value / 10000, (value / 100) % 100, value % 100);
  if (!date) {
    return fail(ErrorKind::bad_argument, "line " + detail::num(number) + ": \"" +
                                             std::string(line) + "\" is not a real date -- " +
                                             date.error().message);
  }
  return date;
}

} // namespace

Expected<Calendar> parse_holiday_file(std::string_view contents) {
  std::vector<Date> holidays;
  auto weekends = static_cast<unsigned>(Weekend::Standard);

  std::size_t line_number = 0;
  std::size_t position = 0;
  while (position <= contents.size()) {
    const std::size_t newline = contents.find('\n', position);
    const std::string_view raw = contents.substr(
        position, newline == std::string_view::npos ? std::string_view::npos : newline - position);
    position = newline == std::string_view::npos ? contents.size() + 1 : newline + 1;
    ++line_number;

    const std::string_view line = trimmed(raw);
    if (line.empty()) {
      continue;
    }

    if (line.front() == '#') {
      const std::string normalised = upper(line);
      const auto found =
          std::find_if(kDirectives.begin(), kDirectives.end(), [&](const Directive &d) {
            return normalised.compare(0, d.line.size(), d.line) == 0;
          });
      if (found == kDirectives.end()) {
        continue; // an ordinary comment
      }

      // D1 / UPSTREAM-41. Upstream builds an upper-cased copy of the line and
      // then compares the ORIGINAL, so its directives are case-SENSITIVE and a
      // lower-case one is dropped in silence -- measured: Saturday stays a
      // holiday, with no diagnostic, and every adjusted date is wrong.
      //
      // Reproducing that would mean reproducing a silently wrong calendar.
      // Accepting any case would accept input upstream rejects. Refusing a
      // line that LOOKS like a directive and is not exactly one does neither.
      if (line.compare(0, found->line.size(), found->line) != 0) {
        return fail(ErrorKind::bad_argument,
                    "line " + detail::num(line_number) + ": \"" + std::string(line) +
                        "\" looks like the directive " + std::string(found->line) +
                        " but the spelling must match exactly, including case. "
                        "Upstream would ignore this line without saying so.");
      }

      if (found->set) {
        weekends |= static_cast<unsigned>(found->bit);
      } else {
        weekends &= ~static_cast<unsigned>(found->bit);
      }
      continue;
    }

    const Expected<Date> date = parse_date_line(line, line_number);
    if (!date) {
      return unexpected(date.error());
    }
    // Upstream's check is `>=`, so an equal date is out of order too. Its
    // message names neither the line nor the date; this one names both.
    if (!holidays.empty() && !(*date > holidays.back())) {
      return fail(ErrorKind::bad_argument,
                  "line " + detail::num(line_number) + ": \"" + std::string(line) +
                      "\" is not after the previous holiday -- dates must strictly increase");
    }
    holidays.push_back(*date);
  }

  return Calendar::create(holidays, static_cast<Weekend>(weekends));
}

} // namespace cdsmodel
