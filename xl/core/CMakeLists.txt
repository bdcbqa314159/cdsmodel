# xl/core -- artifact #3, layer 2 of 4. Platform-INDEPENDENT by design (D8).
#
# `cpp_fdm_world` keeps its whole Excel layer behind `#ifdef WIN32`. This layer
# deliberately sits below that line: it holds the marshalling rules, the
# labelled-range reader and (later) the dispatch table, all of which are
# ordinary C++ and all of which are where the bugs are. Putting them here means
# the existing three-OS CI matrix covers them.
project(cdsmodel_xl_core LANGUAGES CXX)

add_library(cdsmodel_xl_core STATIC
            src/value.cpp
            src/labelled_range.cpp)
add_library(cdsmodel::xl_core ALIAS cdsmodel_xl_core)

target_include_directories(cdsmodel_xl_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

# Links the domain library for Expected<T> and the date/day-count types. The
# dependency runs one way only: cdsmodel knows nothing about Excel.
target_link_libraries(cdsmodel_xl_core PUBLIC cdsmodel)

if(MSVC)
  target_compile_options(cdsmodel_xl_core PRIVATE /W4)
else()
  target_compile_options(cdsmodel_xl_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
