#include "errors.hpp"

#include <nanobind/stl/string.h>

namespace nb = nanobind;

namespace cdsmodel::py {

void register_errors(nb::module_ &m) {
  // D3: each also derives from the builtin a Python caller reaches for first,
  // so `except ValueError` works for someone expecting Pythonic behaviour and
  // `except CdsModelError` for someone who wants the library's taxonomy.
  nb::object base = nb::exception<Failure>(m, "CdsModelError", PyExc_Exception);

  auto derive = [&](const char *name, PyObject *builtin) {
    nb::tuple bases = nb::make_tuple(base, nb::borrow(builtin));
    nb::object exc = nb::steal(
        PyErr_NewException((std::string{"cdsmodel."} + name).c_str(), bases.ptr(), nullptr));
    m.attr(name) = exc;
    return exc;
  };

  g_exceptions[0] = derive("BadArgument", PyExc_ValueError);
  g_exceptions[1] = derive("Unsupported", PyExc_NotImplementedError);
  g_exceptions[2] = derive("NoRoot", PyExc_ArithmeticError);
  g_exceptions[3] = derive("NoConvergence", PyExc_ArithmeticError);

  // One translator, switching on the kind (D21). It runs with the GIL held,
  // after any call_guard has been destroyed.
  nb::register_exception_translator(
      [](const std::exception_ptr &p, void *) {
        try {
          std::rethrow_exception(p);
        } catch (const Failure &f) {
          PyErr_SetString(g_exceptions[index_of(f.kind)].ptr(), f.message.c_str());
        }
      },
      nullptr);
}

} // namespace cdsmodel::py
