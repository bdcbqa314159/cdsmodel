#include <cdsmodel/xl/handles.hpp>

namespace cdsmodel::xl {
namespace {
constexpr char kSeparator = '#';
} // namespace

std::string CurveRegistry::store(const std::string &name, ZeroCurve curve) {
  const auto found = entries_.find(name);
  const std::uint64_t generation = found == entries_.end() ? 1 : found->second.generation + 1;
  entries_.insert_or_assign(name, Entry{generation, std::move(curve)});
  return name + kSeparator + std::to_string(generation);
}

Expected<ZeroCurve> CurveRegistry::resolve(const std::string &handle) const {
  const auto hash = handle.rfind(kSeparator);
  if (hash == std::string::npos) {
    return fail(ErrorKind::bad_argument,
                "\"" + handle + "\" is not a curve handle (expected name#generation)");
  }
  const std::string name = handle.substr(0, hash);
  const std::string suffix = handle.substr(hash + 1);

  const auto found = entries_.find(name);
  if (found == entries_.end()) {
    return fail(ErrorKind::bad_argument, "no curve named \"" + name + "\" exists");
  }

  std::uint64_t wanted = 0;
  for (const char c : suffix) {
    if (c < '0' || c > '9') {
      return fail(ErrorKind::bad_argument, "\"" + handle + "\" has a malformed generation suffix");
    }
    wanted = wanted * 10 + static_cast<std::uint64_t>(c - '0');
  }

  // The check upstream's cache cannot make. Resolving a stale handle to the
  // CURRENT object is the dangerous outcome: the sheet keeps pricing and the
  // numbers quietly change under it.
  if (wanted != found->second.generation) {
    return fail(ErrorKind::bad_argument, "the handle \"" + handle + "\" is stale -- \"" + name +
                                             "\" has been rebuilt since (now generation " +
                                             std::to_string(found->second.generation) +
                                             "). Recalculate the cell that built it.");
  }
  return found->second.curve;
}

} // namespace cdsmodel::xl
