# client — production C++ Roblox client (join, anti-challenge, session).
#
# Why C++ (see FINDINGS.md): the native client speaks OpenSSL's own QUIC
# stack + Roblox TLS custom extensions; a C++ client linking OpenSSL 3.5
# matches that handshake fingerprint, and Luau/zstd/xxhash link natively so
# the 0x9B solve is in-process (no subprocess).
#
# Targets:
#   rbx_solve    — blob decode + opcode standardization + in-process solve
#   regress9b    — 5-dataset regression vs the native's own answers
#   rbx_transport — RUPP + QUIC (ngtcp2 + OpenSSL) native-parity transport
#   rbx_session  — HTTPS join (libcurl) + RbxTransport session burst/solve
#   hs1818       — handshake smoke test against a real game server
#   rbxplay      — full local-solver client (join -> session -> 9B answer)
#
# Build:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./build/regress9b            # expect 5/5 match
cmake_minimum_required(VERSION 3.16)
project(rbxclient CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Roblox API shims / challenge execution live in ../luau_runner (challenge_core).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../luau_runner" luau_runner)

find_library(ZSTD_LIBRARY zstd REQUIRED)
find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)

find_package(OpenSSL REQUIRED)   # QUIC transport (handshake parity with native)

add_library(rbx_solve STATIC
    src/blob.cpp
    src/standardize.cpp
    src/solve.cpp
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

# --- transport: ngtcp2 (built in-tree, see FINDINGS.md) ---------------------
set(NGTCP2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ngtcp2")
set(NGTCP2_BUILD "${NGTCP2_ROOT}/build-host")

find_package(Threads REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

add_library(rbx_transport STATIC
    src/quic_client.cpp
)
target_include_directories(rbx_transport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${NGTCP2_ROOT}/lib/includes"
    "${NGTCP2_ROOT}/crypto/includes"
    "${NGTCP2_BUILD}/lib/includes"   # generated ngtcp2/version.h
)
target_link_libraries(rbx_transport PUBLIC
    "${NGTCP2_BUILD}/crypto/ossl/libngtcp2_crypto_ossl.a"
    "${NGTCP2_BUILD}/lib/libngtcp2.a"
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
target_compile_options(rbx_transport PRIVATE -O2)

# Handshake smoke test: hs1818 <join.json> [--seconds N] [--tx-dump DIR]
add_executable(hs1818 test/hs_main.cpp)
target_include_directories(hs1818 PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(hs1818 PRIVATE rbx_transport)
target_compile_options(hs1818 PRIVATE -O2)

# --- session: HTTPS join + RbxTransport message flow -------------------------
find_package(CURL REQUIRED)

add_library(rbx_session STATIC
    src/join.cpp
    src/session.cpp
)
target_include_directories(rbx_session PUBLIC
    "${NLOHMANN_JSON_INCLUDE_DIR}"
)
target_link_libraries(rbx_session PUBLIC
    rbx_transport
    rbx_solve
    CURL::libcurl
)
target_compile_options(rbx_session PRIVATE -O2)

# Full client: rbxplay [--place N] [--seconds N] [--cookie-file F] ...
add_executable(rbxplay test/play_main.cpp)
target_link_libraries(rbxplay PRIVATE rbx_session)
target_compile_options(rbxplay PRIVATE -O2)
