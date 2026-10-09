# rbx_runtime — the Roblox scripting environment (hosted Luau VM + Roblox
# globals + sandbox + program execution).
#
# Dependencies: git submodules under third_party/, built automatically by this
# project — no prebuilt libraries, no machine-specific paths:
#   third_party/luau  — pristine upstream Luau, pinned (NEVER modified)
#
# Roblox API shims live in src/ (see src/api/api.h for the "add an API"
# recipe) — never in the client, never in the Luau checkout.
#
# Targets:
#   challenge_core   — static lib (public API: runner.h — execution + Options);
#                      linked by the C++ client for in-process solving.
#   challenge_runner — thin CLI over challenge_core (same flags/exit codes
#                      as before the refactor; probe10/regress9b unaffected).
#
# Rebuild:
#   git submodule update --init third_party/luau   (first time only)
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
cmake_minimum_required(VERSION 3.16)
project(challenge_runner CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Luau (submodule) --------------------------------------------------------
# Override with -DLUAU_DIR=/path/to/luau to experiment with a different Luau
# checkout; the pinned submodule is the default and what CI/regressions use.
if(NOT DEFINED LUAU_DIR)
    set(LUAU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/luau")
endif()
if(NOT EXISTS "${LUAU_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Luau not found at ${LUAU_DIR}.\n"
        "Fetch the pinned submodule:\n"
        "  git submodule update --init third_party/luau\n"
        "or point -DLUAU_DIR at a Luau checkout.")
endif()

# Build only the libraries we link (skip Luau's REPL and test suite). These
# must be cache entries: Luau's own cmake_minimum_required(3.10) would let
# option() clobber plain variables.
set(LUAU_BUILD_CLI OFF CACHE BOOL "Build Luau CLI" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "Build Luau tests" FORCE)
# EXCLUDE_FROM_ALL: build only the Luau targets we actually link, not the
# whole Analysis/CodeGen/Require family.
add_subdirectory("${LUAU_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/luau" EXCLUDE_FROM_ALL)

# --- challenge_core ----------------------------------------------------------
# add_library(challenge_core STATIC
#     src/runner.cpp          — execution (load -> call -> answer)
#     src/sandbox.cpp         — environment installer (openlibs + registry)
#     src/api/registry.cpp    — ordered API module list (edit to add APIs)
#     src/api/services.cpp    — rbx.Services table (game:GetService dispatch)
#     src/api/*.cpp           — one file per Roblox API surface
# )
add_library(challenge_core STATIC
    src/runner.cpp
    src/sandbox.cpp
    src/engine.cpp
    src/api/registry.cpp
    src/api/services.cpp
    src/api/random.cpp
    src/api/game.cpp
    src/api/runservice.cpp
    src/api/user_settings.cpp
    src/api/os.cpp
    src/api/newproxy.cpp
    src/api/engine.cpp
    src/instance/class_registry.cpp
    src/instance/instance.cpp
    src/instance/signal.cpp
    src/instance/scheduler.cpp
    src/instance/vector3.cpp
    src/instance/classes.cpp
    src/instance/classes/part.cpp
    src/instance/classes/workspace.cpp
)
target_include_directories(challenge_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
# Linking the Luau CMake targets carries their include dirs and compile flags.
target_link_libraries(challenge_core PUBLIC
    Luau.VM
    Luau.Compiler
    Luau.Ast
    Luau.Bytecode
    Luau.Common
)
target_compile_options(challenge_core PRIVATE -O2)

add_executable(challenge_runner src/main.cpp)
target_link_libraries(challenge_runner PRIVATE challenge_core)
target_compile_options(challenge_runner PRIVATE -O2)
