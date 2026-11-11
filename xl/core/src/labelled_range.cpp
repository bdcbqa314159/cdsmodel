#include <cdsmodel/xl/labelled_range.hpp>

#include <algorithm>
#include <cctype>

namespace cdsmodel::xl {

std::string normalise_label(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  bool pending_space = false;
  for (const unsigned char c : raw) {
    if (std::isspace(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

Expected<LabelledRange> LabelledRange::read(const XlGrid &grid,
                                            std::span<const LabelSpec> expected) {
  if (grid.columns < 2) {
    return fail(ErrorKind::bad_argument,
                "a conventions block needs two columns (label, value), got " +
                    detail::num(grid.columns));
  }
  if (grid.rows == 0) {
    return fail(ErrorKind::bad_argument, "the conventions block is empty");
  }

  std::vector<std::string> known;
  known.reserve(expected.size());
  for (const LabelSpec &spec : expected) {
    known.push_back(normalise_label(spec.label));
  }

  LabelledRange out;
  for (std::size_t row = 0; row < grid.rows; ++row) {
    const XlValue &label_cell = grid.at(row, 0);
    // A blank label row is skipped, not refused: spreadsheet blocks commonly
    // carry a spacer or a heading row, and refusing those would make the
    // feature annoying for no safety gain. A blank label with a VALUE beside
    // it is different -- that is a row someone meant to fill in.
    if (label_cell.is_missing()) {
      if (!grid.at(row, 1).is_missing()) {
        return fail(ErrorKind::bad_argument, "row " + detail::num(row + 1) +
                                                 " of the conventions block has a value "
                                                 "but no label");
      }
      continue;
    }

    const Expected<std::string> raw = label_cell.as_string();
    if (!raw) {
      return fail(ErrorKind::bad_argument, "row " + detail::num(row + 1) +
                                               " of the conventions block: the label is " +
                                               label_cell.describe());
    }
    const std::string label = normalise_label(*raw);
    if (label.empty()) {
      continue;
    }

    // C10, first half: an unknown label is reported, never ignored.
    if (std::find(known.begin(), known.end(), label) == known.end()) {
      std::string allowed;
      for (const LabelSpec &spec : expected) {
        allowed += (allowed.empty() ? "" : ", ");
        allowed += std::string(spec.label);
      }
      return fail(ErrorKind::bad_argument, "\"" + *raw +
                                               "\" is not a recognised convention. "
                                               "Expected one of: " +
                                               allowed);
    }

    // C10, second half: a duplicate is reported too. Taking the first or the
    // last silently would mean two rows disagree and the sheet still prices.
    const bool already = std::any_of(out.entries_.begin(), out.entries_.end(),
                                     [&](const Entry &e) { return e.label == label; });
    if (already) {
      return fail(ErrorKind::bad_argument,
                  "\"" + *raw + "\" appears more than once in the conventions block");
    }

    out.entries_.push_back(Entry{label, grid.at(row, 1)});
  }

  for (const LabelSpec &spec : expected) {
    if (!spec.required) {
      continue;
    }
    if (!out.has(spec.label)) {
      return fail(ErrorKind::bad_argument,
                  "the conventions block is missing \"" + std::string(spec.label) + "\"");
    }
  }
  return out;
}

bool LabelledRange::has(std::string_view label) const {
  const std::string want = normalise_label(label);
  return std::any_of(entries_.begin(), entries_.end(),
                     [&](const Entry &e) { return e.label == want; });
}

Expected<XlValue> LabelledRange::value_of(std::string_view label) const {
  const std::string want = normalise_label(label);
  for (const Entry &e : entries_) {
    if (e.label == want) {
      return e.value;
    }
  }
  return fail(ErrorKind::bad_argument,
              "\"" + std::string(label) + "\" was not supplied in the conventions block");
}

Expected<LabelledTable> LabelledTable::read(const XlGrid &grid,
                                            std::span<const LabelSpec> columns) {
  if (grid.rows == 0) {
    return fail(ErrorKind::bad_argument, "the table is empty -- expected a header row");
  }

  std::vector<std::string> known;
  known.reserve(columns.size());
  for (const LabelSpec &spec : columns) {
    known.push_back(normalise_label(spec.label));
  }

  LabelledTable out;
  out.rows_ = grid.rows - 1; // the header row is not data
  for (std::size_t col = 0; col < grid.columns; ++col) {
    const XlValue &header = grid.at(0, col);
    // An unlabelled column is ignored -- a spacer between tables, say -- exactly
    // as LabelledRange skips a blank label row.
    if (header.is_missing()) {
      continue;
    }
    const Expected<std::string> raw = header.as_string();
    if (!raw) {
      return fail(ErrorKind::bad_argument, "column " + detail::num(col + 1) +
                                               " of the table: the header is " + header.describe());
    }
    const std::string label = normalise_label(*raw);
    if (label.empty()) {
      continue;
    }

    // C10, column-wise: an unknown header is reported, never ignored.
    if (std::find(known.begin(), known.end(), label) == known.end()) {
      std::string allowed;
      for (const LabelSpec &spec : columns) {
        allowed += (allowed.empty() ? "" : ", ");
        allowed += std::string(spec.label);
      }
      return fail(ErrorKind::bad_argument,
                  "\"" + *raw + "\" is not a recognised column. Expected one of: " + allowed);
    }

    // C10: a duplicated header is reported too, for the same reason a duplicated
    // label is -- two columns disagree and the sheet still computes.
    const bool already = std::any_of(out.columns_.begin(), out.columns_.end(),
                                     [&](const Column &c) { return c.label == label; });
    if (already) {
      return fail(ErrorKind::bad_argument,
                  "\"" + *raw + "\" appears more than once as a column header");
    }

    Column resolved;
    resolved.label = label;
    resolved.cells.reserve(out.rows_);
    for (std::size_t row = 1; row < grid.rows; ++row) {
      resolved.cells.push_back(grid.at(row, col));
    }
    out.columns_.push_back(std::move(resolved));
  }

  for (const LabelSpec &spec : columns) {
    if (!spec.required) {
      continue;
    }
    if (!out.has(spec.label)) {
      return fail(ErrorKind::bad_argument,
                  "the table is missing the \"" + std::string(spec.label) + "\" column");
    }
  }
  return out;
}

bool LabelledTable::has(std::string_view label) const {
  const std::string want = normalise_label(label);
  return std::any_of(columns_.begin(), columns_.end(),
                     [&](const Column &c) { return c.label == want; });
}

Expected<std::vector<XlValue>> LabelledTable::column(std::string_view label) const {
  const std::string want = normalise_label(label);
  for (const Column &c : columns_) {
    if (c.label == want) {
      return c.cells;
    }
  }
  return fail(ErrorKind::bad_argument,
              "the \"" + std::string(label) + "\" column was not supplied in the table");
}

} // namespace cdsmodel::xl
