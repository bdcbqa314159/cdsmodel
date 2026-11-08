#include <cdsmodel/xl/arguments.hpp>

#include <cdsmodel/day_count_string.hpp>

namespace cdsmodel::xl {
namespace {

/// A date from a cell. Excel hands dates over as serial numbers; the port's
/// `Date` counts days from the Unix epoch, Excel's serial counts from 1900
/// (with its own leap-year quirk), so the offset is explicit and named rather
/// than a literal appearing mid-expression.
constexpr long kExcelSerialToUnixDays = -25569;

[[nodiscard]] Expected<Date> read_date(const XlValue &cell, std::string_view what) {
  const Expected<double> serial = cell.as_double();
  if (!serial) {
    return fail(ErrorKind::bad_argument,
                std::string(what) + ": expected a date, got " + cell.describe());
  }
  // Excel dates are whole days here. A fractional serial is a time-of-day,
  // which this model has no concept of -- truncating silently would price a
  // trade from a date the sheet does not show.
  const double whole = *serial;
  if (whole != static_cast<double>(static_cast<long>(whole))) {
    return fail(ErrorKind::bad_argument, std::string(what) +
                                             ": a date must be a whole number of days, got " +
                                             detail::num(whole));
  }
  const long days = static_cast<long>(whole) + kExcelSerialToUnixDays;
  return Date{} + std::chrono::days{days};
}

template <std::size_t N>
[[nodiscard]] Expected<LabelledRange>
block_of(const XlValue &block, const std::array<LabelSpec, N> &labels, std::string_view what) {
  const Expected<XlGrid> grid = block.as_grid();
  if (!grid) {
    return fail(ErrorKind::bad_argument,
                std::string(what) + ": expected a two-column range, got " + block.describe());
  }
  Expected<LabelledRange> read = LabelledRange::read(*grid, labels);
  if (!read) {
    return fail(read.error().kind, std::string(what) + ": " + read.error().message);
  }
  return read;
}

} // namespace

Expected<Valuation> Valuation::read_positional(std::span<const XlValue> cells) {
  if (cells.size() != 3) {
    return fail(ErrorKind::bad_argument, "valuation needs 3 cells");
  }
  const Expected<Date> today = read_date(cells[0], "Today");
  if (!today) {
    return unexpected(today.error());
  }
  const Expected<Date> settle = read_date(cells[1], "Cash Settle Date");
  if (!settle) {
    return unexpected(settle.error());
  }
  const Expected<Date> step_in = read_date(cells[2], "Step In Date");
  if (!step_in) {
    return unexpected(step_in.error());
  }
  return Valuation{*today, *settle, *step_in};
}

Expected<Valuation> Valuation::read_block(const XlValue &block) {
  const Expected<LabelledRange> rows = block_of(block, kLabels, "the valuation block");
  if (!rows) {
    return unexpected(rows.error());
  }
  std::array<XlValue, 3> cells;
  for (std::size_t i = 0; i < kLabels.size(); ++i) {
    const Expected<XlValue> v = rows->value_of(kLabels[i].label);
    if (!v) {
      return unexpected(v.error());
    }
    cells[i] = *v;
  }
  // Converges on the positional reader: one place parses a valuation, whatever
  // shape the sheet used to express it.
  return read_positional(cells);
}

Expected<TradeTerms> TradeTerms::read_positional(std::span<const XlValue> cells) {
  if (cells.size() != 3) {
    return fail(ErrorKind::bad_argument, "trade terms need 3 cells");
  }
  const Expected<Date> start = read_date(cells[0], "Start Date");
  if (!start) {
    return unexpected(start.error());
  }
  const Expected<Date> end = read_date(cells[1], "End Date");
  if (!end) {
    return unexpected(end.error());
  }
  const Expected<double> coupon = cells[2].as_double();
  if (!coupon) {
    return fail(ErrorKind::bad_argument,
                "Coupon Rate: expected a number, got " + cells[2].describe());
  }
  return TradeTerms{*start, *end, *coupon};
}

Expected<TradeTerms> TradeTerms::read_block(const XlValue &block) {
  const Expected<LabelledRange> rows = block_of(block, kLabels, "the trade block");
  if (!rows) {
    return unexpected(rows.error());
  }
  std::array<XlValue, 3> cells;
  for (std::size_t i = 0; i < kLabels.size(); ++i) {
    const Expected<XlValue> v = rows->value_of(kLabels[i].label);
    if (!v) {
      return unexpected(v.error());
    }
    cells[i] = *v;
  }
  return read_positional(cells);
}

Expected<Conventions> Conventions::read_positional(std::span<const XlValue> cells) {
  if (cells.size() != 6) {
    return fail(ErrorKind::bad_argument, "conventions need 6 cells");
  }
  Conventions out;

  const Expected<bool> pay_acc = cells[0].as_bool();
  if (!pay_acc) {
    return fail(ErrorKind::bad_argument, "Pay Accrual On Default: " + pay_acc.error().message);
  }
  out.pay_accrual_on_default = *pay_acc;

  const Expected<std::string> interval_text = cells[1].as_string();
  if (!interval_text) {
    return fail(ErrorKind::bad_argument, "Coupon Interval: " + interval_text.error().message);
  }
  const Expected<DateInterval> interval = parse_date_interval(*interval_text);
  if (!interval) {
    return fail(interval.error().kind,
                "Coupon Interval \"" + *interval_text + "\": " + interval.error().message);
  }
  out.coupon_interval = *interval;

  const Expected<std::string> stub_text = cells[2].as_string();
  if (!stub_text) {
    return fail(ErrorKind::bad_argument, "Stub Type: " + stub_text.error().message);
  }
  const Expected<StubMethod> stub = parse_stub_method(*stub_text);
  if (!stub) {
    return fail(stub.error().kind, "Stub Type \"" + *stub_text + "\": " + stub.error().message);
  }
  out.stub = *stub;

  const Expected<std::string> dcc_text = cells[3].as_string();
  if (!dcc_text) {
    return fail(ErrorKind::bad_argument, "Payment DCC: " + dcc_text.error().message);
  }
  const Expected<DayCount> dcc = parse_day_count(*dcc_text);
  if (!dcc) {
    return fail(dcc.error().kind, "Payment DCC \"" + *dcc_text + "\": " + dcc.error().message);
  }
  out.day_count = *dcc;

  const Expected<std::string> bad_day_text = cells[4].as_string();
  if (!bad_day_text) {
    return fail(ErrorKind::bad_argument, "Bad Day Convention: " + bad_day_text.error().message);
  }
  if (bad_day_text->size() != 1) {
    return fail(ErrorKind::bad_argument,
                "Bad Day Convention \"" + *bad_day_text + "\": expected one of N, F, P, M");
  }
  const Expected<BadDayConv> bad_day = bad_day_conv_from_upstream(bad_day_text->front());
  if (!bad_day) {
    return fail(bad_day.error().kind,
                "Bad Day Convention \"" + *bad_day_text + "\": " + bad_day.error().message);
  }
  out.bad_day = *bad_day;

  // Holidays. A named calendar needs the holiday-file reader, which is a
  // declared non-port (see docs/port-notes.md), so only the weekend-only
  // calendar is available and anything else says so plainly rather than
  // silently pricing against no holidays at all.
  if (!cells[5].is_missing()) {
    const Expected<std::string> holidays = cells[5].as_string();
    if (!holidays) {
      return fail(ErrorKind::bad_argument, "Holidays: " + holidays.error().message);
    }
    if (!holidays->empty() && *holidays != "NONE" && *holidays != "None" && *holidays != "none") {
      return fail(ErrorKind::unsupported,
                  "Holidays \"" + *holidays +
                      "\": named holiday calendars are not available yet; only NONE is. "
                      "Pricing against the wrong calendar silently would be worse.");
    }
  }
  out.holidays = {Calendar::none()};
  return out;
}

Expected<Conventions> Conventions::read_block(const XlValue &block) {
  const Expected<LabelledRange> rows = block_of(block, kLabels, "the conventions block");
  if (!rows) {
    return unexpected(rows.error());
  }
  std::array<XlValue, 6> cells;
  for (std::size_t i = 0; i < kLabels.size(); ++i) {
    if (!rows->has(kLabels[i].label)) {
      continue; // optional and absent; stays XlMissing
    }
    const Expected<XlValue> v = rows->value_of(kLabels[i].label);
    if (!v) {
      return unexpected(v.error());
    }
    cells[i] = *v;
  }
  return read_positional(cells);
}

Expected<MarketData> MarketData::read_positional(std::span<const XlValue> cells,
                                                 const CurveRegistry &curves) {
  if (cells.size() != 3) {
    return fail(ErrorKind::bad_argument, "market data needs 3 cells");
  }
  const Expected<std::string> discount_handle = cells[0].as_string();
  if (!discount_handle) {
    return fail(ErrorKind::bad_argument,
                "Discount Curve: expected a curve handle, got " + cells[0].describe());
  }
  const Expected<ZeroCurve> discount = curves.resolve(*discount_handle);
  if (!discount) {
    return fail(discount.error().kind, "Discount Curve: " + discount.error().message);
  }

  const Expected<std::string> spread_handle = cells[1].as_string();
  if (!spread_handle) {
    return fail(ErrorKind::bad_argument,
                "Spread Curve: expected a curve handle, got " + cells[1].describe());
  }
  const Expected<ZeroCurve> spread = curves.resolve(*spread_handle);
  if (!spread) {
    return fail(spread.error().kind, "Spread Curve: " + spread.error().message);
  }

  const Expected<double> recovery = cells[2].as_double();
  if (!recovery) {
    return fail(ErrorKind::bad_argument,
                "Recovery Rate: expected a number, got " + cells[2].describe());
  }
  if (!(*recovery >= 0.0 && *recovery <= 1.0)) {
    return fail(ErrorKind::bad_argument,
                "Recovery Rate " + detail::num(*recovery) + " is outside 0 to 1");
  }
  return MarketData{*discount, *spread, *recovery};
}

Expected<MarketData> MarketData::read_block(const XlValue &block, const CurveRegistry &curves) {
  const Expected<LabelledRange> rows = block_of(block, kLabels, "the market block");
  if (!rows) {
    return unexpected(rows.error());
  }
  std::array<XlValue, 3> cells;
  for (std::size_t i = 0; i < kLabels.size(); ++i) {
    const Expected<XlValue> v = rows->value_of(kLabels[i].label);
    if (!v) {
      return unexpected(v.error());
    }
    cells[i] = *v;
  }
  return read_positional(cells, curves);
}

Cds make_cds(const TradeTerms &trade, const Conventions &conventions) {
  Cds cds;
  cds.start = trade.start;
  cds.end = trade.end;
  cds.coupon_rate = trade.coupon_rate;
  cds.coupon_interval = conventions.coupon_interval;
  cds.stub = conventions.stub;
  cds.day_count = conventions.day_count;
  cds.bad_day = conventions.bad_day;
  cds.pay_accrual_on_default = conventions.pay_accrual_on_default;
  return cds;
}

} // namespace cdsmodel::xl
