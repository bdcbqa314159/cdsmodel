# xl/interface -- artifact #3, layer 4 of 4. The add-in.
#
# A SHARED library with SUFFIX ".xll", which is all an XLL is -- the same
# approach as cpp_fdm_world.
project(cdsmodel_xll LANGUAGES CXX)

add_library(cdsmodel_xll SHARED src/exports.cpp)
target_link_libraries(cdsmodel_xll PRIVATE cdsmodel_xl_sdk cdsmodel_xl_core cdsmodel)
set_target_properties(cdsmodel_xll PROPERTIES
                      OUTPUT_NAME "cdsmodel"
                      SUFFIX ".xll")
if(MSVC)
  target_compile_options(cdsmodel_xll PRIVATE /W4)
else()
  target_compile_options(cdsmodel_xll PRIVATE -Wall -Wextra -Wpedantic)
endif()
