# Boblox — top-level superbuild.
#
# Builds the Roblox runtime + network client together and registers the whole
# test battery with CTest:
#
#   git submodule update --init --recursive        (first time only)
#   cmake --preset release
#   cmake --build --preset release -j
#   ctest --preset release
#
# The per-project entry points (rbx_runtime/, client/) still configure and
# build standalone; this superbuild is additive.
cmake_minimum_required(VERSION 3.21)
project(boblox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clangd / clang-tidy

include(CTest) # provides BUILD_TESTING (default ON) + enable_testing()

add_subdirectory(rbx_runtime)
add_subdirectory(client)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# On-demand regeneration of the derived opcode tables (JSON is the source of
# truth; ctest `tables-parity` fails if they drift):
#   cmake --build build --target gen_tables
add_custom_target(gen_tables
    COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_tables_inc.py"
    COMMENT "Regenerating client/src/tables.inc from tools/roblox_bc_tables.json"
    VERBATIM)
