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
  src/module.cpp)

target_link_libraries(_core PRIVATE cdsmodel)
target_compile_features(_core PRIVATE cxx_std_20)

# COMPONENT python, with `install.components = ["python"]` in pyproject.toml:
# without it `cmake --install` also stages the library's own install rules and
# the wheel ends up carrying include/, lib/libcdsmodel.a and lib/cmake/ --
# which land directly in site-packages and collide with other packages.
install(TARGETS _core LIBRARY DESTINATION cdsmodel COMPONENT python)
