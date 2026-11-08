#pragma once

/// C6, C7 and H7 -- a registry for objects a cell cannot hold.
///
/// A cell holds a number or a string, so a curve or a calendar has to be
/// named. Upstream did this with a process-wide `static TObject *cache` linked
/// list keyed by `stricmp`, which means two workbooks share one namespace and
/// a stale handle silently resolves to whatever now has that name. This port
/// spent 41 files removing that kind of mutable global state, so the registry
/// here is an OBJECT the add-in owns, keys compare exactly, and every stored
/// value carries a generation so a stale handle is DETECTED rather than
/// honoured.
///
/// Templated rather than written twice. The duplication would not be
/// incidental: the generation rule is one piece of knowledge -- resolving a
/// stale handle to the current object is the dangerous outcome, because the
/// sheet keeps pricing while the numbers change under it -- and having it in
/// two places means two places to get it wrong.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cdsmodel::xl {

/// Stores values under names and hands back handles that can go stale.
template <typename T> class Registry {
public:
  /// What this registry holds, for error messages: "curve", "calendar".
  explicit Registry(std::string_view noun) : noun_(noun) {}

  /// Stores `value` under `name` and returns the handle a cell should hold.
  /// Overwriting an existing name bumps its generation, which invalidates
  /// every handle already in a sheet.
  [[nodiscard]] std::string store(const std::string &name, T value) {
    const auto found = entries_.find(name);
    const std::uint64_t generation = found == entries_.end() ? 1 : found->second.generation + 1;
    entries_.insert_or_assign(name, Entry{generation, std::move(value)});
    return name + kSeparator + std::to_string(generation);
  }

  [[nodiscard]] Expected<T> resolve(const std::string &handle) const {
    const auto hash = handle.rfind(kSeparator);
    if (hash == std::string::npos) {
      return fail(ErrorKind::bad_argument,
                  "\"" + handle + "\" is not a " + noun_ + " handle (expected name#generation)");
    }
    const std::string name = handle.substr(0, hash);
    const std::string suffix = handle.substr(hash + 1);

    const auto found = entries_.find(name);
    if (found == entries_.end()) {
      return fail(ErrorKind::bad_argument, "no " + noun_ + " named \"" + name + "\" exists");
    }

    std::uint64_t wanted = 0;
    for (const char c : suffix) {
      if (c < '0' || c > '9') {
        return fail(ErrorKind::bad_argument,
                    "\"" + handle + "\" has a malformed generation suffix");
      }
      wanted = wanted * 10 + static_cast<std::uint64_t>(c - '0');
    }

    // The check upstream's cache cannot make.
    if (wanted != found->second.generation) {
      return fail(ErrorKind::bad_argument, "the handle \"" + handle + "\" is stale -- \"" + name +
                                               "\" has been rebuilt since (now generation " +
                                               std::to_string(found->second.generation) +
                                               "). Recalculate the cell that built it.");
    }
    return found->second.value;
  }

  /// C7: `xlAutoClose` empties this, so no handle survives a reload.
  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  static constexpr char kSeparator = '#';
  struct Entry {
    std::uint64_t generation = 0;
    T value;
  };
  std::string noun_;
  std::unordered_map<std::string, Entry> entries_;
};

/// The two registries the add-in keeps.
class CurveRegistry : public Registry<ZeroCurve> {
public:
  CurveRegistry() : Registry<ZeroCurve>("curve") {}
};

class CalendarRegistry : public Registry<Calendar> {
public:
  CalendarRegistry() : Registry<Calendar>("calendar") {}
};

} // namespace cdsmodel::xl
