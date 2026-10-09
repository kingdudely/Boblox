# fuzz/ — libFuzzer harnesses for the session-data parsers (Phase K):
# blob/standardize/frame-walk. Standalone mini-project (NOT part of the
# superbuild): the parsers under test are VM-free, so the fuzz build needs
# only clang + zstd — no OpenSSL, ngtcp2, curl, or Luau.
#
#   tools/fuzz_all.sh [seconds]   # build + run all three (default 120s each)
#
# Clang only: libFuzzer is an LLVM facility (gcc cannot build these).
cmake_minimum_required(VERSION 3.21)
project(boblox_fuzz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "fuzz/ requires Clang (libFuzzer); got ${CMAKE_CXX_COMPILER_ID}")
endif()

find_library(ZSTD_LIBRARY zstd REQUIRED)
find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)

set(CLIENT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../client/src")

# The code under test, ASan-instrumented like the harnesses.
add_library(fuzz_parsers STATIC
    "${CLIENT_SRC}/blob.cpp"
    "${CLIENT_SRC}/standardize.cpp"
)
target_include_directories(fuzz_parsers PUBLIC "${CLIENT_SRC}" "${ZSTD_INCLUDE_DIR}")
target_link_libraries(fuzz_parsers PUBLIC "${ZSTD_LIBRARY}")
target_compile_options(fuzz_parsers PUBLIC
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
target_link_options(fuzz_parsers PUBLIC -fsanitize=address,undefined)

foreach(t blob standardize frames)
    add_executable(fuzz_${t} fuzz_${t}.cpp)
    target_link_libraries(fuzz_${t} PRIVATE fuzz_parsers)
    # -fsanitize=fuzzer provides the main(); link it only into the harnesses.
    target_compile_options(fuzz_${t} PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_options(fuzz_${t} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
