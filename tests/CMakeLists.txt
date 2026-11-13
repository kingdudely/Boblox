# CTest registration for the whole battery + doctest unit-test binaries.
# The suites themselves live next to the subsystem they test; this directory
# only wires them into `ctest` (see tests/run_all.sh for the standalone
# non-superbuild flow).

# header-only doctest (submodule, pinned)
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE "${CMAKE_SOURCE_DIR}/third_party/doctest")

# --- existing regression/parity suites --------------------------------------

# solver-regress-py drives challenge_runner as a subprocess; point it at the
# superbuild's binary explicitly (default remains rbx_runtime/build/...).
add_test(NAME solver-regress-py
         COMMAND "${CMAKE_COMMAND}" -E env "RBX_RUNNER=$<TARGET_FILE:challenge_runner>"
                 python3 "${CMAKE_SOURCE_DIR}/tools/regress9b.py")

# the C++ regress binary takes the repo root explicitly (its default is a
# legacy local path)
add_test(NAME solver-regress-cpp COMMAND "$<TARGET_FILE:regress9b>" "${CMAKE_SOURCE_DIR}")

add_test(NAME handshake-parity
         COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/ch_diff.py"
                 "${CMAKE_SOURCE_DIR}/run/cpp_tx/tx001.bin")

add_test(NAME message-parity
         COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/msg_parity.py"
                 "${CMAKE_SOURCE_DIR}/run/join_full.json"
                 "${CMAKE_SOURCE_DIR}/run/cpp_msgs")

# --- unit tests (doctest) -----------------------------------------------------

find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

add_executable(unit_runtime unit/unit_runtime.cpp)
target_link_libraries(unit_runtime PRIVATE challenge_core doctest)
add_test(NAME unit-runtime COMMAND unit_runtime)

# two-profile sandbox split: Challenge profile frozen to the historical six
add_executable(unit_profile unit/unit_profile.cpp)
target_link_libraries(unit_profile PRIVATE challenge_core doctest)
add_test(NAME unit-profile COMMAND unit_profile)

# Replication batch encoding (0x83), pinned to the labeled native capture.
add_executable(unit_batch unit/unit_batch.cpp)
target_link_libraries(unit_batch PRIVATE rbx_solve doctest)
add_test(NAME unit-batch COMMAND unit_batch)

# The upstream stream: native 0xA6 cadence, step continuity, ControlState.
add_executable(unit_send unit/unit_send.cpp)
target_link_libraries(unit_send PRIVATE rbx_raknet doctest)
add_test(NAME unit-send COMMAND unit_send)

# The executor surface: environment, code, metatables, filesystem, crypt.
add_executable(unit_exec unit/unit_exec.cpp)
# exec.cpp's http_request asks with rbx::userAgent() (rbx_join), so the one
# identity function is linked in rather than duplicated in the test.
target_link_libraries(unit_exec PRIVATE challenge_core rbx_solve rbx_join doctest)
add_test(NAME unit-exec COMMAND unit_exec)

# Phase J: the instance world (Engine profile only) — tree, properties,
# signals, the task scheduler on the virtual clock, workspace, Vector3.
add_executable(unit_engine unit/unit_engine.cpp)
target_include_directories(unit_engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../client/src")
target_link_libraries(unit_engine PRIVATE challenge_core rbx_solve rbx_join rbx_raknet rbx_view doctest)
target_compile_definitions(unit_engine PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-engine COMMAND unit_engine)

# golden per-stage vectors through the C++ pipeline
# RakNet join-message helpers (the 0x8a ticket key is signed per place id)
add_executable(unit_join unit/unit_join.cpp)
target_include_directories(unit_join PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../client/src"
    "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(unit_join PRIVATE rbx_raknet doctest)
add_test(NAME unit-join COMMAND unit_join)

# Local feature presets / exact lists (Luau, render, GUI, physics mode).
add_executable(unit_features unit/unit_features.cpp)
target_include_directories(unit_features PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../client/src"
    "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(unit_features PRIVATE doctest)
add_test(NAME unit-features COMMAND unit_features)

# Jolt world + virtual character, and the 2016 CameraScript constants.
add_executable(unit_physics unit/unit_physics.cpp)
target_include_directories(unit_physics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../client/src")
target_link_libraries(unit_physics PRIVATE rbx_local_player rbx_physics doctest)
add_test(NAME unit-physics COMMAND unit_physics)

add_executable(unit_camera unit/unit_camera.cpp)
target_include_directories(unit_camera PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../client/src")
target_link_libraries(unit_camera PRIVATE rbx_camera rbx_physics doctest)
add_test(NAME unit-camera COMMAND unit_camera)

# ScreenGui -> NanoVG-neutral draw commands (UDim2, anchor, clip, text).
add_executable(unit_gui unit/unit_gui.cpp)
target_include_directories(unit_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../client/src")
target_link_libraries(unit_gui PRIVATE challenge_core doctest)
add_test(NAME unit-gui COMMAND unit_gui)

# RBX_API_AUDIT: the opt-in Luau miss/stub/event inventory contract.
add_executable(unit_audit unit/unit_audit.cpp)
target_include_directories(unit_audit PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(unit_audit PRIVATE challenge_core doctest)
add_test(NAME unit-audit COMMAND unit_audit)

add_executable(unit_vectors unit/unit_vectors.cpp)
target_include_directories(unit_vectors PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(unit_vectors PRIVATE rbx_solve doctest)
target_compile_definitions(unit_vectors PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-vectors COMMAND unit_vectors)

# session message builders vs Python-reference golden bytes (RbxTransport;
# only exists when -DRBX_TRANSPORT=ON, the default)
if(TARGET rbx_session)
    add_executable(unit_messages unit/unit_messages.cpp)
    target_link_libraries(unit_messages PRIVATE rbx_session doctest)
    target_compile_definitions(unit_messages PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME unit-messages COMMAND unit_messages)
endif()

add_executable(unit_replicate unit/unit_replicate.cpp)
target_include_directories(unit_replicate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../client/src")
target_link_libraries(unit_replicate PRIVATE rbx_solve doctest)
target_compile_definitions(unit_replicate PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-replicate COMMAND unit_replicate)

# RUPP framing vs a native-captured datagram
add_executable(unit_rupp unit/unit_rupp.cpp)
target_include_directories(unit_rupp PRIVATE "${CMAKE_SOURCE_DIR}/client/src")
target_link_libraries(unit_rupp PRIVATE doctest)
target_compile_definitions(unit_rupp PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-rupp COMMAND unit_rupp)

# Phase K: libFuzzer-found parser crashes pinned as permanent regressions
# (extract/decode/standardize/frame-walk must reject them cleanly).
add_executable(unit_fuzzregress unit/unit_fuzzregress.cpp)
target_link_libraries(unit_fuzzregress PRIVATE rbx_solve doctest)
target_compile_definitions(unit_fuzzregress PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-fuzzregress COMMAND unit_fuzzregress)

# the generated DataModel registry stays in sync with the pinned API dump
add_test(NAME instances-parity
         COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_instances.py" --check)

# --- generator parity (source of truth <-> derived files) ---------------------

# vectors stay in sync with the tracked capture fixtures + py builders
add_test(NAME vectors-py COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_vectors.py" --check)

# client/src/solve/tables.inc stays in sync with tools/roblox_bc_tables.json
add_test(NAME tables-parity COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_tables_inc.py" --check)
