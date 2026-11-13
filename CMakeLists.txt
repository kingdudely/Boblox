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
project(boblox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clangd / clang-tidy

include(CTest) # provides BUILD_TESTING (default ON) + enable_testing()

# Jolt Physics 5.6 (MIT).  We build only the library; its samples/viewer/tests
# are skipped automatically because it is a subproject.  GPU compute backends
# are irrelevant to us and would otherwise probe for a Vulkan/DX12 SDK at
# configure time.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/jolt/Build ${CMAKE_BINARY_DIR}/jolt)

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
    COMMENT "Regenerating client/src/solve/tables.inc from tools/roblox_bc_tables.json"
    VERBATIM)
