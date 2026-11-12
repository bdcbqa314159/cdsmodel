# The nanobind extension. Built only when CDSMODEL_BUILD_PYTHON is on, which
# scikit-build-core sets; a plain `cmake --preset debug` never descends here.
find_package(Python 3.12 REQUIRED
             COMPONENTS Interpreter Development.Module Development.SABIModule)

execute_process(
  COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
find_package(nanobind CONFIG REQUIRED)

# STABLE_ABI: one cp312-abi3 wheel per platform (D1).
# NB_STATIC:  nanobind linked in, so the wheel carries no second shared object.
nanobind_add_module(_core STABLE_ABI NB_STATIC
  src/module.cpp
  src/types.cpp
  src/errors.cpp
  src/curves.cpp
  src/calendars.cpp
  src/instruments.cpp
  src/pricing.cpp
  src/credit_curve.cpp)

target_link_libraries(_core PRIVATE cdsmodel)

# Must match gen_python_fixtures exactly -- see cdsmodel_pin_fp_contract in
# the top-level CMakeLists. Without it the extension and the generator are
# two differently-optimised builds of the same inline code, and on GCC they
# fuse multiply-adds differently: one ULP apart, which P3 compares exactly.
cdsmodel_pin_fp_contract(_core)
target_compile_features(_core PRIVATE cxx_std_20)

# COMPONENT python, with `install.components = ["python"]` in pyproject.toml:
# without it `cmake --install` also stages the library's own install rules and
# the wheel ends up carrying include/, lib/libcdsmodel.a and lib/cmake/ --
# which land directly in site-packages and collide with other packages.
install(TARGETS _core LIBRARY DESTINATION cdsmodel COMPONENT python)
