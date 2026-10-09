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

# the C++ regress binary resolves its datasets relative to the repo root
add_test(NAME solver-regress-cpp COMMAND "$<TARGET_FILE:regress9b>")
set_tests_properties(solver-regress-cpp PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

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

add_executable(unit_runtime unit/unit_runtime.cpp)
target_link_libraries(unit_runtime PRIVATE challenge_core doctest)
add_test(NAME unit-runtime COMMAND unit_runtime)
