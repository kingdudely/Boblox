# client — production C++ Roblox client (join, anti-challenge, session).
#
# Two transports are built side by side, deliberately independent:
#   rbx_raknet    — RakNet (RUPP + offline handshake), the live path.
#   rbx_transport — RbxTransport: QUIC via ngtcp2, the pre-RakNet path,
#                   restored verbatim from f28c1c8. Shares only rupp.h
#                   with RakNet; -DRBX_TRANSPORT=OFF drops it and ngtcp2.
# See FINDINGS.md "RakNet" for the decoded offline handshake.
#
# Targets:
#   rbx_solve     — blob decode + opcode standardization + in-process solve
#   regress9b     — 5-dataset regression vs the native's own answers
#   rbx_join      — HTTPS join (libcurl) + join.json load/save
#   rbx_raknet    — RakNet transport: RUPP framing + offline handshake
#   raknet1818    — live RakNet handshake smoke test against a real server
#   rbx_transport — RbxTransport/QUIC (ngtcp2 + OpenSSL), native parity
#   rbx_session   — RbxTransport session burst/solve (blob, 0x9B answers)
#   hs1818        — RbxTransport handshake smoke test
#   rbxplay       — full local-solver client over RbxTransport
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

# --- transport: RakNet (RUPP + handshake + session crypto) -------------------
# SessionCrypto = X25519 + BLAKE2b-512 KDF + ChaCha20-Poly1305, all from
# libcrypto (the native client links the same primitives).
find_package(OpenSSL REQUIRED)
add_library(rbx_raknet STATIC
    src/raknet.cpp
    src/session_crypto.cpp
    src/join_msg.cpp
)
target_include_directories(rbx_raknet PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(rbx_raknet PUBLIC OpenSSL::Crypto)
target_compile_options(rbx_raknet PRIVATE -O2)

# The join-message builders are nlohmann-based (join script / join reply).
target_include_directories(rbx_raknet PUBLIC "${NLOHMANN_JSON_INCLUDE_DIR}")

# Live handshake + join smoke test: raknet1818 --place-id 1818 --roblosecurity …
add_executable(raknet1818 test/raknet_main.cpp)
target_link_libraries(raknet1818 PRIVATE rbx_raknet rbx_join)
target_compile_options(raknet1818 PRIVATE -O2)

# Known-answer tests: the captured native 0x90/0x8a/0x92 messages.
# Vectors live in run/ (raknet90_template.bin, raknet90_body.bin,
# raknet8a_template.bin, raknet8a_message.bin, raknet92_message.bin).
add_executable(join_msg_test test/join_msg_test.cpp)
target_link_libraries(join_msg_test PRIVATE rbx_raknet)
target_compile_options(join_msg_test PRIVATE -O2)
add_test(NAME join_msg_raknet COMMAND join_msg_test
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

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

# --- transport: RbxTransport (QUIC via ngtcp2) — restored, opt-out ----------
# The pre-RakNet client, kept alive from f28c1c8 (also on GitLab). It is a
# self-contained stack: its own library, its own executables, and the only
# thing it shares with RakNet is the RUPP header (rupp.h). Nothing under
# rbx_raknet links it, so -DRBX_TRANSPORT=OFF leaves the RakNet path exactly
# as it is.
#
# ngtcp2 is a pinned submodule (third_party/ngtcp2) and its checkout is
# NEVER touched: sources are read from the submodule, but the build tree
# lives under OUR build dir (CMAKE_CURRENT_BINARY_DIR/ngtcp2-host), so a
# fresh clone needs no manual steps and the submodule stays pristine
# (byte-identical to upstream, trivially replaceable).
option(RBX_TRANSPORT "build the restored RbxTransport/QUIC stack (ngtcp2)" ON)
if(RBX_TRANSPORT)
    set(NGTCP2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ngtcp2")
    set(NGTCP2_BUILD "${CMAKE_CURRENT_BINARY_DIR}/ngtcp2-host")

    include(ExternalProject)
    find_package(Threads REQUIRED)
    set(NGTCP2_CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DENABLE_SHARED_LIB=OFF
        -DBUILD_TESTING=OFF
    )
    if(DEFINED OPENSSL_ROOT_DIR)
        list(APPEND NGTCP2_CMAKE_ARGS -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR})
    endif()
    ExternalProject_Add(ngtcp2_host
        SOURCE_DIR "${NGTCP2_ROOT}"
        BINARY_DIR "${NGTCP2_BUILD}"
        CMAKE_ARGS ${NGTCP2_CMAKE_ARGS}
        UPDATE_COMMAND "" # never touch the submodule (no fetch/pull)
        INSTALL_COMMAND "" # use the build-tree libs directly, no install
        BUILD_BYPRODUCTS
            "${NGTCP2_BUILD}/lib/libngtcp2.a"
            "${NGTCP2_BUILD}/crypto/ossl/libngtcp2_crypto_ossl.a"
    )

    # Imported targets so dependents build ngtcp2 first, automatically.
    # NOTE: the generated-headers dir is created here (empty) so CMake's
    # configure-time existence check on INTERFACE_INCLUDE_DIRECTORIES passes;
    # ngtcp2's own build fills it (ngtcp2/version.h) before anything compiles.
    file(MAKE_DIRECTORY "${NGTCP2_BUILD}/lib/includes")
    add_library(ngtcp2_lib STATIC IMPORTED)
    set_target_properties(ngtcp2_lib PROPERTIES
        IMPORTED_LOCATION "${NGTCP2_BUILD}/lib/libngtcp2.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${NGTCP2_ROOT}/lib/includes;${NGTCP2_BUILD}/lib/includes")
    add_dependencies(ngtcp2_lib ngtcp2_host)
    add_library(ngtcp2_crypto STATIC IMPORTED)
    set_target_properties(ngtcp2_crypto PROPERTIES
        IMPORTED_LOCATION "${NGTCP2_BUILD}/crypto/ossl/libngtcp2_crypto_ossl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${NGTCP2_ROOT}/crypto/includes")
    add_dependencies(ngtcp2_crypto ngtcp2_host)

    add_library(rbx_transport STATIC
        src/quic_client.cpp
    )
    target_include_directories(rbx_transport PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(rbx_transport PUBLIC
        ngtcp2_crypto
        ngtcp2_lib
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    target_compile_options(rbx_transport PRIVATE -O2)

    # Handshake smoke test: hs1818 <join.json> (runs until interrupted;
    # RBX_TX_DUMP set = dump TX)
    add_executable(hs1818 test/hs_main.cpp)
    target_include_directories(hs1818 PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
    target_link_libraries(hs1818 PRIVATE rbx_transport)
    target_compile_options(hs1818 PRIVATE -O2)

    # Session: RbxTransport message flow (blob walk, 0x9B answer). The HTTPS
    # join lives in rbx_join and is shared with the RakNet path.
    add_library(rbx_session STATIC
        src/send.cpp
        src/session.cpp
    )
    target_include_directories(rbx_session PUBLIC
        "${NLOHMANN_JSON_INCLUDE_DIR}"
    )
    target_link_libraries(rbx_session PUBLIC
        rbx_transport
        rbx_solve
        rbx_join
    )
    target_compile_options(rbx_session PRIVATE -O2)

    # Full client: rbxplay --place N --roblosecurity COOKIE (until interrupted)
    add_executable(rbxplay test/play_main.cpp)
    target_link_libraries(rbxplay PRIVATE rbx_session rbx_view)
    target_compile_options(rbxplay PRIVATE -O2)
else()
    message(STATUS "transport: RbxTransport/QUIC disabled (RBX_TRANSPORT=OFF)")
endif()
