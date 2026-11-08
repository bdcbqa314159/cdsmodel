# B2 of specs/02_consumability.md -- a consumer that knows nothing.
#
# Deliberately minimal and deliberately NOT part of the main build: it is
# configured separately, against an install prefix, so that "did it install"
# and "does the installed thing work" stay distinct questions (D3).
cmake_minimum_required(VERSION 3.20)
project(cdsmodel_consumer LANGUAGES CXX)

find_package(cdsmodel REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE cdsmodel::cdsmodel)

# The consumer compiles under the same strictness the library does, because a
# header that only compiles cleanly in a lax build is not actually clean.
if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /WX)
else()
  target_compile_options(consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
