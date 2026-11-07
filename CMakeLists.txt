cmake_minimum_required(VERSION 3.28)
project(cdsmodel VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CDSMODEL_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

# UndefinedBehaviorSanitizer PRINTS and CONTINUES by default, exiting 0. A
# sanitizer job that only fails on AddressSanitizer errors therefore ignores
# every pure-UB finding -- signed overflow, bad shifts, invalid enum loads.
# This makes UBSan fatal, and is applied ONLY to the port and its tests, never
# to cds_reference: upstream's own undefined behaviour is catalogued, not
# fixed, and must stay observable without aborting the suite.
option(CDSMODEL_FATAL_UBSAN "Abort on UndefinedBehaviorSanitizer findings in port code" OFF)
set(CDSMODEL_UBSAN_FATAL_FLAGS "")
if(CDSMODEL_FATAL_UBSAN AND NOT MSVC)
  set(CDSMODEL_UBSAN_FATAL_FLAGS -fno-sanitize-recover=undefined)
endif()
set(CDSMODEL_UPSTREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/isda_cds_model_c_v1.8.3"
    CACHE PATH "Local copy of the upstream ISDA CDS Standard Model C sources")

# =========================================================================
# The oracle -- OPTIONAL and never redistributed.
#
# Upstream is only available from cdsmodel.com after accepting the ISDA CDS
# Standard Model Public License and providing an email address, so it is not
# committed here. When a local copy is present we build it and run live
# differential tests against it. When it is absent the port still builds and
# still verifies itself against committed reference vectors -- which section
# 4(b) of the upstream licence explicitly exempts from its terms, being
# "data derived from calculations" rather than source or an executable.
# =========================================================================
if(EXISTS "${CDSMODEL_UPSTREAM_DIR}/lib/src/cds.c")
  set(CDSMODEL_HAVE_UPSTREAM TRUE)
  message(STATUS "cdsmodel: upstream oracle found -- live differential tests enabled")
else()
  set(CDSMODEL_HAVE_UPSTREAM FALSE)
  message(STATUS "cdsmodel: no upstream oracle at ${CDSMODEL_UPSTREAM_DIR}")
  message(STATUS "cdsmodel: building against committed reference vectors only")
  message(STATUS "cdsmodel: see README.md 'Obtaining the reference implementation'")
endif()

if(CDSMODEL_HAVE_UPSTREAM)
  file(GLOB REF_SOURCES "${CDSMODEL_UPSTREAM_DIR}/lib/src/*.c"
                        "${CDSMODEL_UPSTREAM_DIR}/lib/src/*.cpp")

  # clang 16+ promotes these 2009-era C idioms from warning to error, and -w
  # alone does not demote them. Suppress rather than edit the oracle.
  if(MSVC)
    set(LEGACY_C_FLAGS /w)
  else()
    set(LEGACY_C_FLAGS -w
        -Wno-error=implicit-function-declaration -Wno-implicit-function-declaration
        -Wno-error=incompatible-pointer-types -Wno-error=int-conversion
        -Wno-error=return-mismatch -Wno-deprecated-non-prototype)
  endif()

  add_library(cds_reference STATIC ${REF_SOURCES})
  # lib/include/isda is upstream's public surface. lib/src carries a few
  # private headers that differential tests need -- zcswdate.h declares
  # TSwapDates, which no public header exposes.
  target_include_directories(cds_reference PUBLIC
                             "${CDSMODEL_UPSTREAM_DIR}/lib/include/isda"
                             "${CDSMODEL_UPSTREAM_DIR}/lib/src")
  target_compile_definitions(cds_reference PUBLIC VERSION="1.8.3")
  # LINUX is upstream linux.mk's own flag; it gates only the stricmp alias.
  if(NOT WIN32)
    target_compile_definitions(cds_reference PUBLIC UNIX LINUX)
  endif()
  target_compile_options(cds_reference PRIVATE ${LEGACY_C_FLAGS})
  set_target_properties(cds_reference PROPERTIES C_STANDARD 99)
endif()

# =========================================================================
# The C++20 port. Grows one module at a time; each module must match the
# oracle exactly before the next one starts.
# =========================================================================
add_library(cdsmodel STATIC src/version.cpp)
add_library(cdsmodel::cdsmodel ALIAS cdsmodel)
target_compile_features(cdsmodel PUBLIC cxx_std_20)
target_include_directories(cdsmodel PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
if(MSVC)
  target_compile_options(cdsmodel PRIVATE /W4)
else()
  target_compile_options(cdsmodel PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_compile_options(cdsmodel PUBLIC ${CDSMODEL_UBSAN_FATAL_FLAGS})

# Each public header must compile on its own. A missing #include that happens
# to be satisfied transitively by whatever a .cpp included first is a latent
# break for any consumer who includes the header before anything else -- and
# it is invisible to the test suite, which always includes several headers.
file(GLOB CDSMODEL_PUBLIC_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/cdsmodel/*.hpp")
set(CDSMODEL_HEADER_SELFTESTS "")
foreach(header ${CDSMODEL_PUBLIC_HEADERS})
  get_filename_component(stem "${header}" NAME_WE)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/header_selftest/${stem}.cpp")
  file(WRITE "${generated}" "#include <cdsmodel/${stem}.hpp>\n")
  list(APPEND CDSMODEL_HEADER_SELFTESTS "${generated}")
endforeach()
add_library(cdsmodel_header_selftest OBJECT ${CDSMODEL_HEADER_SELFTESTS})
target_link_libraries(cdsmodel_header_selftest PRIVATE cdsmodel)
if(MSVC)
  target_compile_options(cdsmodel_header_selftest PRIVATE /W4)
else()
  target_compile_options(cdsmodel_header_selftest PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================================================================
# Tests
# =========================================================================
if(CDSMODEL_BUILD_TESTS)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2 GIT_SHALLOW TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # Tier 1 -- always runs, anywhere, including CI with no oracle present.
  # Port semantics plus replay of committed reference vectors.
  add_executable(cdsmodel_tests
                 tests/version_test.cpp
                 tests/bsearch_test.cpp
                 tests/date_test.cpp
                 tests/day_count_test.cpp
                 tests/day_count_string_test.cpp
                 tests/date_interval_test.cpp
                 tests/calendar_test.cpp
                 tests/bad_day_test.cpp
                 tests/business_days_test.cpp
                 tests/date_list_test.cpp
                 tests/zero_curve_test.cpp
                 tests/timeline_test.cpp
                 tests/contingent_leg_test.cpp
                 tests/fee_leg_test.cpp
                 tests/stub_test.cpp
                 tests/schedule_test.cpp
                 tests/cds_test.cpp
                 tests/root_find_test.cpp
                 tests/bootstrap_test.cpp
                 tests/cdsone_test.cpp
                 tests/defaulted_test.cpp
                 tests/date_sup_test.cpp
                 tests/interpolation_test.cpp
                 tests/rate_conversion_test.cpp
                 tests/zero_curve_builder_test.cpp
                 tests/cash_flow_list_test.cpp
                 tests/cash_flow_bootstrap_test.cpp
                 tests/date_schedule_test.cpp
                 tests/cash_flow_schedule_test.cpp
                 tests/bad_day_map_test.cpp
                 tests/swap_dates_test.cpp
                 tests/interp_curve_test.cpp
                 tests/curve_discount_test.cpp
                 tests/curve_rates_test.cpp
                 tests/coupon_schedule_test.cpp
                 tests/float_rate_test.cpp
                 tests/stream_cash_flows_test.cpp
                 tests/swap_bootstrap_flows_test.cpp
                 tests/adversarial_test.cpp)
  target_link_libraries(cdsmodel_tests PRIVATE cdsmodel GTest::gtest_main)
  target_include_directories(cdsmodel_tests PRIVATE tests)
  include(GoogleTest)
  gtest_discover_tests(cdsmodel_tests)

  # Tier 2 -- only where a local oracle exists. Calls the port and the 2009 C
  # side by side, in-process, and compares.
  if(CDSMODEL_HAVE_UPSTREAM)
    add_executable(cdsmodel_differential_tests
                   tests/bsearch_differential_test.cpp
                   tests/date_differential_test.cpp
                   tests/day_count_differential_test.cpp
                   tests/day_count_string_differential_test.cpp
                   tests/date_interval_differential_test.cpp
                   tests/calendar_differential_test.cpp
                   tests/bad_day_differential_test.cpp
                   tests/business_days_differential_test.cpp
                   tests/date_list_differential_test.cpp
                   tests/zero_curve_differential_test.cpp
                   tests/timeline_differential_test.cpp
                   tests/contingent_leg_differential_test.cpp
                   tests/fee_leg_differential_test.cpp
                   tests/schedule_differential_test.cpp
                   tests/cds_differential_test.cpp
                   tests/root_find_differential_test.cpp
                   tests/bootstrap_differential_test.cpp
                   tests/cdsone_differential_test.cpp
                   tests/defaulted_differential_test.cpp
                   tests/date_sup_differential_test.cpp
                   tests/interpolation_differential_test.cpp
                   tests/rate_conversion_differential_test.cpp
                   tests/zero_curve_builder_differential_test.cpp
                   tests/cash_flow_list_differential_test.cpp
                   tests/cash_flow_bootstrap_differential_test.cpp
                   tests/date_schedule_differential_test.cpp
                   tests/stub_differential_test.cpp
                   tests/cash_flow_schedule_differential_test.cpp
                   tests/bad_day_map_differential_test.cpp
                   tests/swap_dates_differential_test.cpp
                   tests/interp_curve_differential_test.cpp
                   tests/curve_discount_differential_test.cpp
                   tests/curve_rates_differential_test.cpp
                   tests/coupon_schedule_differential_test.cpp
                   tests/float_rate_differential_test.cpp
                   tests/stream_cash_flows_differential_test.cpp
                   tests/swap_bootstrap_flows_differential_test.cpp)
    target_link_libraries(cdsmodel_differential_tests
                          PRIVATE cdsmodel cds_reference GTest::gtest_main)
    target_include_directories(cdsmodel_differential_tests PRIVATE tests)
    gtest_discover_tests(cdsmodel_differential_tests)

    # Regenerates tests/golden/bsearch_cases.hpp from the live oracle, so the
    # committed vectors are reproducible rather than magic numbers.
    foreach(gen bsearch date day_count day_count_string date_interval calendar bad_day
                 business_days date_list zero_curve timeline contingent_leg
                 fee_leg schedule cds root_find bootstrap cdsone defaulted
                 date_sup interpolation rate_conversion
                 zero_curve_builder cash_flow_list
                 cash_flow_bootstrap date_schedule stub
                 cash_flow_schedule bad_day_map swap_dates
                 interp_curve curve_discount curve_rates coupon_schedule
                 stream_cash_flows swap_bootstrap_flows)
      add_executable(gen_${gen}_golden tools/gen_${gen}_golden.cpp)
      target_link_libraries(gen_${gen}_golden PRIVATE cdsmodel cds_reference)
      target_include_directories(gen_${gen}_golden PRIVATE tests)
      target_compile_options(gen_${gen}_golden PRIVATE ${LEGACY_C_FLAGS})
    endforeach()

    # Demonstrates the catalogued upstream defects; produces the transcript
    # quoted by docs/defect-report.md. Not a test -- it reports, not asserts.
    add_executable(defect_evidence tools/defect_evidence.cpp)
    target_link_libraries(defect_evidence PRIVATE cdsmodel cds_reference)
    target_compile_options(defect_evidence PRIVATE ${LEGACY_C_FLAGS})

    # End-to-end: upstream's own example output, byte-exact.
    add_executable(cds_reference_example
                   "${CDSMODEL_UPSTREAM_DIR}/examples/c/src/main.c")
    target_link_libraries(cds_reference_example PRIVATE cds_reference)
    target_compile_options(cds_reference_example PRIVATE ${LEGACY_C_FLAGS})
    if(NOT WIN32)
      target_link_libraries(cds_reference_example PRIVATE m)
    endif()
    add_test(NAME oracle_example
             COMMAND ${CMAKE_COMMAND}
                     -DEXE=$<TARGET_FILE:cds_reference_example>
                     -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/reference_example.txt
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_golden.cmake)
  endif()
endif()
