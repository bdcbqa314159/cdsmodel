#pragma once

/// C6 and C7 of specs/03_xll.md -- a registry for objects a cell cannot hold.
///
/// A cell holds a number or a string, so a curve has to be named. Upstream did
/// this with a process-wide `static TObject *cache` linked list keyed by
/// `stricmp`, which means two workbooks share one namespace and a stale handle
/// silently resolves to whatever now has that name. This port spent 41 files
/// removing exactly that kind of mutable global state, so the registry here is
/// an OBJECT the add-in owns, keys are compared exactly, and every stored
/// value carries a generation so a stale handle is detected rather than
/// honoured.

#include <cdsmodel/expected.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace cdsmodel::xl {

/// The name a cell holds, e.g. `"USD.disc#3"`. The generation suffix is what
/// makes a stale handle detectable: rebuilding a curve under the same name
/// produces a new handle, and the old one stops resolving instead of quietly
/// returning different numbers.
class CurveRegistry {
public:
  /// Stores `curve` under `name` and returns the handle a cell should hold.
  /// Overwriting an existing name bumps its generation.
  [[nodiscard]] std::string store(const std::string &name, ZeroCurve curve);

  /// Resolves a handle, or explains why it cannot be.
  [[nodiscard]] Expected<ZeroCurve> resolve(const std::string &handle) const;

  /// C7: `xlAutoClose` empties this, so no handle survives a reload.
  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  struct Entry {
    std::uint64_t generation = 0;
    ZeroCurve curve;
  };
  std::unordered_map<std::string, Entry> entries_;
};

} // namespace cdsmodel::xl
