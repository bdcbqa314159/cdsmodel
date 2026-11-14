# xl/sdk -- artifact #3, layer 3 of 4. Windows and the Excel SDK only.
#
# Compiles the SDK's own xlcall.cpp and framework.c rather than linking
# xlcall32.lib: xlcall.cpp resolves the callback with
# GetProcAddress(hmodule, "MdCallBack12") from the running Excel process, so
# there is no import library and no bitness to mismatch.
project(cdsmodel_xl_sdk LANGUAGES C CXX)

add_library(cdsmodel_xl_sdk STATIC
            src/oper.cpp
            src/excel4_stub.cpp
            ${CDSMODEL_XLSDK_SOURCES})
add_library(cdsmodel::xl_sdk ALIAS cdsmodel_xl_sdk)

target_include_directories(cdsmodel_xl_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  "${CDSMODEL_XLSDK_INCLUDE}")

target_link_libraries(cdsmodel_xl_sdk PUBLIC cdsmodel_xl_core)

# The SDK's own sources are not ours and are not held to our warning bar --
# the same policy the ISDA oracle gets, for the same reason. (COMPILE_FLAGS is a
# literal string, not a generator expression, so the compiler is branched on.)
if(MSVC)
  set_source_files_properties(${CDSMODEL_XLSDK_SOURCES} PROPERTIES COMPILE_FLAGS "/W0")
  target_compile_options(cdsmodel_xl_sdk PRIVATE /W4 /WX)
else()
  set_source_files_properties(${CDSMODEL_XLSDK_SOURCES} PROPERTIES COMPILE_FLAGS "-w")
  target_compile_options(cdsmodel_xl_sdk PRIVATE -Wall -Wextra -Wpedantic)
endif()
