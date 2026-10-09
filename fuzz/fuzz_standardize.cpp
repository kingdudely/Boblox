// fuzz_standardize.cpp — libFuzzer harness for the wire-bytecode
// standardizer (Phase K): proto-table parse + opcode-rewrite walk.
//
// Invariants under test: no crashes (ASan/UBSan clean), no hangs (offsets
// only advance inside the buffer; impossible sizes fail at the door), no
// uncaught exceptions (the proto-count reserve is capped).
#include "standardize.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint8_t> buf(data, data + size);
    rbxclient::Stats st;
    std::string err;
    rbxclient::standardize(buf, &st, &err);
    return 0;
}
