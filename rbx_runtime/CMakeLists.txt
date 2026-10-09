# rbx_runtime — Roblox challenge-program runner.
#
# This project is intentionally SEPARATE from the Luau checkout so that the
# Luau fork can be replaced/upgraded independently:
#   * Luau source:  LUAU_DIR (default /home/john/luau, pristine upstream)
#   * Roblox APIs:  src/ (this project)
#
# Targets:
#   challenge_core   — static lib (sandbox + program execution, runner.h);
#                      linked by the C++ client for in-process solving.
#   challenge_runner — thin CLI over challenge_core (same flags/exit codes
#                      as before the refactor; probe10/regress9b unaffected).
#
# Rebuild:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
cmake_minimum_required(VERSION 3.16)
project(challenge_runner CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED LUAU_DIR)
    set(LUAU_DIR "/home/john/luau" CACHE PATH "Path to a pristine Luau checkout")
endif()

set(LUAU_BUILD "${LUAU_DIR}/build" CACHE PATH "Luau build dir" FORCE)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(LUAU_BUILD "${LUAU_DIR}/build-dbg")
endif()
if(NOT EXISTS "${LUAU_BUILD}/libLuau.VM.a")
    message(FATAL_ERROR
        "Luau libraries not found in ${LUAU_BUILD}.\n"
        "Build them first:\n"
        "  cmake -S ${LUAU_DIR} -B ${LUAU_BUILD} -DCMAKE_BUILD_TYPE=Release\n"
        "  cmake --build ${LUAU_BUILD} --target Luau.VM Luau.Compiler -j")
endif()

# add_library(challenge_core STATIC
#     src/runner.cpp          — execution (load -> call -> answer)
#     src/sandbox.cpp         — environment installer (openlibs + API modules)
#     src/api/registry.cpp    — ordered API module list (edit to add APIs)
#     src/api/services.cpp    — rbx.Services table (game:GetService dispatch)
#     src/api/*.cpp           — one file per Roblox API surface
# )
add_library(challenge_core STATIC
    src/runner.cpp
    src/sandbox.cpp
    src/api/registry.cpp
    src/api/services.cpp
    src/api/random.cpp
    src/api/game.cpp
    src/api/runservice.cpp
    src/api/user_settings.cpp
    src/api/os.cpp
    src/api/newproxy.cpp
)
target_include_directories(challenge_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${LUAU_DIR}/VM/include"
    "${LUAU_DIR}/Common/include"
)
target_link_libraries(challenge_core PUBLIC
    "${LUAU_BUILD}/libLuau.VM.a"
    "${LUAU_BUILD}/libLuau.Compiler.a"
    "${LUAU_BUILD}/libLuau.Ast.a"
    "${LUAU_BUILD}/libLuau.Bytecode.a"
    "${LUAU_BUILD}/libLuau.Common.a"
)
target_compile_options(challenge_core PRIVATE -O2)

add_executable(challenge_runner src/main.cpp)
target_link_libraries(challenge_runner PRIVATE challenge_core)
target_compile_options(challenge_runner PRIVATE -O2)
