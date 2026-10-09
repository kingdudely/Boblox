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

add_test(NAME extension-codec
         COMMAND node "${CMAKE_SOURCE_DIR}/test/codec.test.mjs")

# --- unit tests (doctest) -----------------------------------------------------

find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

add_executable(unit_runtime unit/unit_runtime.cpp)
target_link_libraries(unit_runtime PRIVATE challenge_core doctest)
add_test(NAME unit-runtime COMMAND unit_runtime)

# two-profile sandbox split: Challenge profile frozen to the historical six
add_executable(unit_profile unit/unit_profile.cpp)
target_link_libraries(unit_profile PRIVATE challenge_core doctest)
add_test(NAME unit-profile COMMAND unit_profile)

# golden per-stage vectors through the C++ pipeline
add_executable(unit_vectors unit/unit_vectors.cpp)
target_include_directories(unit_vectors PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(unit_vectors PRIVATE rbx_solve doctest)
target_compile_definitions(unit_vectors PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-vectors COMMAND unit_vectors)

# session message builders vs Python-reference golden bytes
add_executable(unit_messages unit/unit_messages.cpp)
target_link_libraries(unit_messages PRIVATE rbx_session doctest)
target_compile_definitions(unit_messages PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-messages COMMAND unit_messages)

# RUPP framing vs a native-captured datagram
add_executable(unit_rupp unit/unit_rupp.cpp)
target_include_directories(unit_rupp PRIVATE "${CMAKE_SOURCE_DIR}/client/src")
target_link_libraries(unit_rupp PRIVATE doctest)
target_compile_definitions(unit_rupp PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-rupp COMMAND unit_rupp)

# --- generator parity (source of truth <-> derived files) ---------------------

# vectors stay in sync with the tracked capture fixtures + py builders
add_test(NAME vectors-py COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_vectors.py" --check)

# client/src/tables.inc stays in sync with tools/roblox_bc_tables.json
add_test(NAME tables-parity COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_tables_inc.py" --check)
