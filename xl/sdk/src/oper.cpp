#include <cdsmodel/xl/oper.hpp>

#ifdef _WIN32

#include "framework.h"

#include <cstring>

namespace cdsmodel::xl {
namespace {

/// An Excel string is length-counted, not NUL-terminated: `val.str[0]` holds
/// the character count. Getting this wrong reads past the end, which is how
/// UPSTREAM-38 happened on the C side.
[[nodiscard]] std::string string_from(const XCHAR *counted) {
  if (counted == nullptr) {
    return {};
  }
  const std::size_t length = static_cast<std::size_t>(counted[0]);
  std::string out;
  out.reserve(length);
  for (std::size_t i = 1; i <= length; ++i) {
    const XCHAR c = counted[i];
    // Narrow deliberately and visibly: anything outside ASCII becomes '?'
    // rather than being truncated silently. Every string this add-in reads is
    // a convention name, a day count or a handle, all ASCII by construction.
    out.push_back(c < 128 ? static_cast<char>(c) : '?');
  }
  return out;
}

} // namespace

XlValue from_oper(const LPXLOPER12 oper) {
  if (oper == nullptr) {
    return XlValue{XlError{"a null argument"}};
  }
  // xltypeMulti and the rest can arrive with bits set alongside the type, so
  // the type is masked before comparing.
  switch (oper->xltype & ~(xlbitXLFree | xlbitDLLFree)) {
  case xltypeNum:
    return XlValue{oper->val.num};
  case xltypeBool:
    return XlValue{oper->val.xbool != 0};
  case xltypeStr:
    return XlValue{string_from(oper->val.str)};
  case xltypeMissing:
  case xltypeNil:
    return XlValue{};
  case xltypeErr:
    return XlValue{XlError{"an Excel error value (" + std::to_string(oper->val.err) + ")"}};
  case xltypeInt:
    return XlValue{static_cast<double>(oper->val.w)};
  case xltypeMulti: {
    XlGrid grid;
    grid.rows = static_cast<std::size_t>(oper->val.array.rows);
    grid.columns = static_cast<std::size_t>(oper->val.array.columns);
    grid.cells.reserve(grid.rows * grid.columns);
    for (std::size_t i = 0; i < grid.rows * grid.columns; ++i) {
      grid.cells.push_back(from_oper(&oper->val.array.lparray[i]));
    }
    return XlValue{std::move(grid)};
  }
  default:
    return XlValue{XlError{"an argument of a type this add-in does not read"}};
  }
}

LPXLOPER12 temp_string(std::string_view text) {
  auto *out = static_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
  if (out == nullptr) {
    return nullptr;
  }
  // Excel's limit for a counted string. Truncating is the only option that
  // does not lose the cell entirely, and our messages are well short of it.
  const std::size_t length = text.size() > 32767 ? 32767 : text.size();
  auto *buffer = static_cast<XCHAR *>(GetTempMemory((length + 1) * sizeof(XCHAR)));
  if (buffer == nullptr) {
    return nullptr;
  }
  buffer[0] = static_cast<XCHAR>(length);
  for (std::size_t i = 0; i < length; ++i) {
    buffer[i + 1] = static_cast<XCHAR>(static_cast<unsigned char>(text[i]));
  }
  out->xltype = xltypeStr;
  out->val.str = buffer;
  return out;
}

LPXLOPER12 to_oper(const XlValue &value) {
  if (const auto *d = std::get_if<double>(&value.storage())) {
    auto *out = static_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
    if (out == nullptr) {
      return nullptr;
    }
    out->xltype = xltypeNum;
    out->val.num = *d;
    return out;
  }
  if (const auto *b = std::get_if<bool>(&value.storage())) {
    auto *out = static_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
    if (out == nullptr) {
      return nullptr;
    }
    out->xltype = xltypeBool;
    out->val.xbool = *b ? 1 : 0;
    return out;
  }
  if (const auto *s = std::get_if<std::string>(&value.storage())) {
    return temp_string(*s);
  }
  if (const auto *e = std::get_if<XlError>(&value.storage())) {
    return temp_string(e->text);
  }
  if (const auto *grid = std::get_if<XlGrid>(&value.storage())) {
    // Five functions return a grid: ParSpreads, DatesAndRates, FeeLegFlows,
    // DefaultAccrual and CDS.CalendarDates. Excel wants one XLOPER12 of type
    // xltypeMulti whose `lparray` holds rows*columns opers in ROW-MAJOR order,
    // which is the order XlGrid already stores them in.
    auto *out = static_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
    if (out == nullptr) {
      return nullptr;
    }
    const std::size_t count = grid->rows * grid->columns;
    if (count == 0) {
      // Excel cannot display an empty array. Every producer here already
      // substitutes a message rather than returning one, so this is a
      // belt-and-braces path, not a reachable one.
      return temp_string("the result is empty");
    }
    auto *cells = static_cast<LPXLOPER12>(GetTempMemory(count * sizeof(XLOPER12)));
    if (cells == nullptr) {
      return nullptr;
    }
    for (std::size_t i = 0; i < count; ++i) {
      // Each cell is converted on its own and then COPIED into the array:
      // `to_oper` hands back a pointer into temp memory, and Excel needs the
      // values laid out contiguously rather than as pointers to them.
      const LPXLOPER12 one = to_oper(grid->cells[i]);
      if (one == nullptr) {
        return nullptr;
      }
      cells[i] = *one;
    }
    out->xltype = xltypeMulti;
    out->val.array.rows = static_cast<RW>(grid->rows);
    out->val.array.columns = static_cast<COL>(grid->columns);
    out->val.array.lparray = cells;
    return out;
  }
  return temp_string("this add-in cannot return that type");
}

} // namespace cdsmodel::xl

#endif // _WIN32
