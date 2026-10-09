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
