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
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cdsmodel::xl {

/// Stores values under names and hands back handles that can go stale.
///
/// **Thread-safe, and holds values by `shared_ptr<const T>`.** Both of those
/// were once comments saying why they did not need to be true; they are now
/// true instead.
///
/// The lock matters because Excel decides when and on which thread to
/// recalculate. Today every function registers with the `"1"` macro type --
/// the calculation thread only -- but appending `"$"` to a type-code string
/// marks a function thread-safe, and that is a one-character change somebody
/// will eventually make. Making the registry safe now costs a mutex on a path
/// that does a hash lookup anyway.
///
/// The `shared_ptr` solves two things at once. A `ZeroCurve` holds a vector of
/// points, so returning by value copied the whole curve on every cell
/// reference -- ten thousand cells against a five-hundred-point curve meant
/// ten thousand copies. And a reference would have dangled: `store` can
/// overwrite an entry or rehash the map while a caller still holds one.
/// Sharing an immutable value does neither.
template <typename T> class Registry {
public:
  /// What `resolve` hands back: an immutable value shared with the registry.
  using Shared = std::shared_ptr<const T>;

  /// What this registry holds, for error messages: "curve", "calendar".
  explicit Registry(std::string_view noun) : noun_(noun) {}

  /// Stores `value` under `name` and returns the handle a cell should hold.
  /// Overwriting an existing name bumps its generation, which invalidates
  /// every handle already in a sheet.
  [[nodiscard]] std::string store(const std::string &name, T value) {
    auto shared = std::make_shared<const T>(std::move(value));
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = entries_.find(name);
    const std::uint64_t generation = found == entries_.end() ? 1 : found->second.generation + 1;
    entries_.insert_or_assign(name, Entry{generation, std::move(shared)});
    return name + kSeparator + std::to_string(generation);
  }

  /// Resolves a handle to a shared, immutable value.
  ///
  /// The caller's `shared_ptr` keeps the object alive even if `store`
  /// subsequently overwrites the name or rehashes the map, so a cell can hold
  /// the result across another cell's recalculation.
  [[nodiscard]] Expected<Shared> resolve(const std::string &handle) const {
    const auto hash = handle.rfind(kSeparator);
    if (hash == std::string::npos) {
      return fail(ErrorKind::bad_argument,
                  "\"" + handle + "\" is not a " + noun_ + " handle (expected name#generation)");
    }
    const std::string name = handle.substr(0, hash);
    const std::string suffix = handle.substr(hash + 1);

    std::uint64_t wanted = 0;
    for (const char c : suffix) {
      if (c < '0' || c > '9') {
        return fail(ErrorKind::bad_argument,
                    "\"" + handle + "\" has a malformed generation suffix");
      }
      wanted = wanted * 10 + static_cast<std::uint64_t>(c - '0');
    }

    // Everything above is parsing and needs no lock. Only the lookup does.
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = entries_.find(name);
    if (found == entries_.end()) {
      return fail(ErrorKind::bad_argument, "no " + noun_ + " named \"" + name + "\" exists");
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
  ///
  /// Any `shared_ptr` a caller still holds keeps its own object alive, which
  /// is what makes clearing safe mid-recalculation rather than a use-after-
  /// free waiting to happen.
  void clear() {
    const std::lock_guard<std::mutex> guard(mutex_);
    entries_.clear();
  }

  [[nodiscard]] std::size_t size() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return entries_.size();
  }

private:
  static constexpr char kSeparator = '#';
  struct Entry {
    std::uint64_t generation = 0;
    Shared value;
  };
  std::string noun_;
  mutable std::mutex mutex_;
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
