# client — production C++ Roblox client (join, anti-challenge, session).
#
# Transport is RakNet (the transport the native client uses for gameplay;
# RbxTransport/QUIC was only the dummy path and is archived in git — the
# pre-RakNet tree is commit f28c1c8, also on GitLab). See FINDINGS.md
# "RakNet" for the decoded offline handshake.
#
# Targets:
#   rbx_solve     — blob decode + opcode standardization + in-process solve
#   regress9b     — 5-dataset regression vs the native's own answers
#   rbx_join      — HTTPS join (libcurl) + join.json load/save
#   rbx_raknet    — RakNet transport: RUPP framing + offline handshake
#   raknet1818    — live RakNet handshake smoke test against a real server
#
# Build:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./build/regress9b            # expect 5/5 match
cmake_minimum_required(VERSION 3.16)
project(rbxclient CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Roblox API shims / challenge execution live in ../rbx_runtime (challenge_core).
# Guarded: the top-level superbuild may have added it already.
if(NOT TARGET challenge_core)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../rbx_runtime" rbx_runtime)
endif()

find_library(ZSTD_LIBRARY zstd REQUIRED)
find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)

add_library(rbx_solve STATIC
    src/blob.cpp
    src/standardize.cpp
    src/solve.cpp
    src/repl_core.cpp
    src/repl_snapshot.cpp
    src/repl_stream.cpp
    src/joindata.cpp
    src/live_runtime.cpp
)
target_include_directories(rbx_solve PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ZSTD_INCLUDE_DIR}"
)
target_link_libraries(rbx_solve PUBLIC
    challenge_core
    "${ZSTD_LIBRARY}"
)
target_compile_options(rbx_solve PRIVATE -O2)

add_executable(regress9b test/regress_main.cpp)
target_link_libraries(regress9b PRIVATE rbx_solve)
target_compile_options(regress9b PRIVATE -O2)

# --- join: HTTPS game join (transport-independent) ---------------------------
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
find_package(CURL REQUIRED)

add_library(rbx_join STATIC
    src/join.cpp
)
target_include_directories(rbx_join PUBLIC
    "${NLOHMANN_JSON_INCLUDE_DIR}"
)
target_link_libraries(rbx_join PUBLIC
    rbx_solve
    CURL::libcurl
)
target_compile_options(rbx_join PRIVATE -O2)

# --- transport: RakNet (RUPP + offline handshake) ----------------------------
add_library(rbx_raknet STATIC
    src/raknet.cpp
)
target_include_directories(rbx_raknet PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_options(rbx_raknet PRIVATE -O2)

# Live handshake smoke test: raknet1818 --join-json FILE --token HEX
add_executable(raknet1818 test/raknet_main.cpp)
target_link_libraries(raknet1818 PRIVATE rbx_raknet rbx_join)
target_compile_options(raknet1818 PRIVATE -O2)

# --- viewer: raylib (SDL3 platform backend) snapshot mesh soup ------------
# Third-party checkout stays pristine (pinned submodule, read-only); the
# build tree lives under OUR build dir. The headful window needs SDL3 dev
# (raylib PLATFORM SDL); it is absent on CI (ubuntu-24.04 — libsdl3-dev
# only ships in Ubuntu 25.10+) and minimal machines. Without it only the
# GL window is stubbed out: the pure snapshot walk (view_collect.cpp,
# golden-tested) always builds, and --headful fails loudly via the stub.
# -DRBX_VIEWER=OFF forces the stub. No OS backdoors: same CLI, same error
# path either way.
option(RBX_VIEWER "build raylib headful viewer when SDL3 is present" ON)
find_package(SDL3 QUIET)
if(RBX_VIEWER AND SDL3_FOUND)
    if(NOT TARGET raylib)
        set(PLATFORM SDL CACHE STRING "raylib platform (Desktop default)")
        set(BUILD_EXAMPLES OFF CACHE BOOL "no raylib examples" FORCE)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/raylib"
                         raylib-bin)
    endif()
    add_library(rbx_view STATIC
        src/view_collect.cpp
        src/view_session.cpp
        src/view_raylib.cpp
    )
    message(STATUS "viewer: raylib headful enabled (SDL3 present)")
else()
    if(RBX_VIEWER)
        message(STATUS "viewer: headful stub (SDL3 dev not found)")
    else()
        message(STATUS "viewer: headful stub (RBX_VIEWER=OFF)")
    endif()
    add_library(rbx_view STATIC
        src/view_collect.cpp
        src/view_session.cpp
        src/view_stub.cpp
    )
endif()
target_include_directories(rbx_view PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(rbx_view PUBLIC
    rbx_solve
    challenge_core
)
if(TARGET raylib)
    target_link_libraries(rbx_view PUBLIC raylib)
endif()
