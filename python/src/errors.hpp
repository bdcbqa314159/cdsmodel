#pragma once

// Artifact #5, D3/D20/D21: `Expected<T>` becomes a raised Python exception.
//
// D20 is the subtle half. D10 releases the GIL around every library call, and
// touching the Python C API without the GIL is undefined behaviour -- a crash
// under load, not a failing test. So the guarded body never calls into Python:
// it throws `Failure`, a plain C++ exception, and the translator registered
// below converts it *after* unwinding has run the guard's destructor and
// re-acquired the GIL.
//
// D21: `nb::exception<T>` keys on the C++ type, and all four kinds share one
// carrier, so four of them could not discriminate. The four Python types are
// built explicitly and one translator switches on the kind.

#include <nanobind/nanobind.h>

#include <cdsmodel/expected.hpp>

#include <exception>
#include <string>
#include <utility>

namespace cdsmodel::py {

/// The carrier. Derives from `std::exception` because `nb::exception<T>`
/// requires `what()`.
struct Failure : std::exception {
  ErrorKind kind;
  std::string message;

  Failure(ErrorKind k, std::string m) : kind(k), message(std::move(m)) {}
  [[nodiscard]] const char *what() const noexcept override { return message.c_str(); }
};

/// The four exception types, in `ErrorKind` order. Populated by
/// `register_errors` and read by the translator.
inline nanobind::object g_exceptions[4];

[[nodiscard]] inline int index_of(ErrorKind kind) noexcept {
  switch (kind) {
  case ErrorKind::bad_argument:
    return 0;
  case ErrorKind::unsupported:
    return 1;
  case ErrorKind::no_root:
    return 2;
  case ErrorKind::no_convergence:
    return 3;
  }
  return 0;
}

/// Unwrap or throw. D5: the kind and the message pass through unchanged --
/// a caller forwarding a failure must not rename it.
template <typename T> [[nodiscard]] T unwrap(Expected<T> result) {
  if (!result) {
    throw Failure{result.error().kind, std::string{result.error().message}};
  }
  return std::move(*result);
}

void register_errors(nanobind::module_ &m);

} // namespace cdsmodel::py
