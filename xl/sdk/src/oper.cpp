#include <cdsmodel/xl/oper.hpp>

// `CDSMODEL_XL_TYPECHECK` compiles this on a non-Windows machine against the
// real `xlcall.h` plus the minimal shim in `tools/winshim`. It exists because
// this file is otherwise the only code in the project no compiler has ever
// seen -- see tools/typecheck_xl.sh. Defining `_WIN32` instead would make the
// standard library believe it is on Windows, which breaks libc++ immediately.
#if defined(_WIN32) || defined(CDSMODEL_XL_TYPECHECK)

// framework.h needs windows.h too, which oper.hpp has already pulled in.
#include "framework.h"

#include <cstring>

namespace cdsmodel::xl {
namespace {

// `GetTempMemory` returns `LPSTR` -- a `char *`, not a `void *` -- so every
// allocation here needs `reinterpret_cast` rather than `static_cast`. Found by
// typechecking against the real framework.h off Windows; `static_cast` is
// simply ill-formed between unrelated pointer types and would have failed the
// first Windows build.

/// An Excel string is length-counted, not NUL-terminated: `val.str[0]` holds
/// the character count. Getting this wrong reads past the end, which is how
/// UPSTREAM-38 happened on the C side. The counted-string walk lives here
/// because it is `XCHAR`-specific; the narrowing rule it feeds is
/// `xl::ascii_from_utf16` in xl/core, where CI can test it (C2).
[[nodiscard]] std::string string_from(const XCHAR *counted) {
  if (counted == nullptr) {
    return {};
  }
  const std::size_t length = static_cast<std::size_t>(counted[0]);
  std::u16string units;
  units.reserve(length);
  for (std::size_t i = 1; i <= length; ++i) {
    // XCHAR and char16_t are both 16-bit code units, so this widens losslessly;
    // no narrowing decision is made here -- that is ascii_from_utf16's job.
    units.push_back(static_cast<char16_t>(counted[i]));
  }
  return ascii_from_utf16(units);
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
  case xltypeSRef:
  case xltypeRef: {
    // A cell or range argument arrives as a REFERENCE, not its values, because
    // the functions register their opers as type "U" -- XLOPER12 passed by
    // reference. Excel does not coerce it for us; the add-in must. Without this,
    // a multi-cell argument like a dates column is "a type this add-in does not
    // read" and only inline literals work -- which is the first thing the real
    // Excel build surfaced that no CI leg could. xlCoerce turns a range into an
    // xltypeMulti and a single cell into its scalar; we read that and free what
    // Excel allocated for it.
    XLOPER12 coerced;
    if (Excel12(xlCoerce, &coerced, 1, oper) != xlretSuccess) {
      return XlValue{XlError{"could not read that cell reference"}};
    }
    XlValue out = from_oper(&coerced);
    Excel12(xlFree, nullptr, 1, &coerced);
    return out;
  }
  default:
    return XlValue{XlError{"an argument of a type this add-in does not read"}};
  }
}

LPXLOPER12 temp_string(std::string_view text) {
  auto *out = reinterpret_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
  if (out == nullptr) {
    return nullptr;
  }
  // Excel's limit for a counted string. Truncating is the only option that
  // does not lose the cell entirely, and our messages are well short of it.
  const std::size_t length = text.size() > 32767 ? 32767 : text.size();
  auto *buffer = reinterpret_cast<XCHAR *>(GetTempMemory((length + 1) * sizeof(XCHAR)));
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
    auto *out = reinterpret_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
    if (out == nullptr) {
      return nullptr;
    }
    out->xltype = xltypeNum;
    out->val.num = *d;
    return out;
  }
  if (const auto *b = std::get_if<bool>(&value.storage())) {
    auto *out = reinterpret_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
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
    auto *out = reinterpret_cast<LPXLOPER12>(GetTempMemory(sizeof(XLOPER12)));
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
    auto *cells = reinterpret_cast<LPXLOPER12>(GetTempMemory(count * sizeof(XLOPER12)));
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

#endif // _WIN32 || CDSMODEL_XL_TYPECHECK
