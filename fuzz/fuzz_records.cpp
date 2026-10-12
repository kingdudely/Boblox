// fuzz_records.cpp — libFuzzer harness for the JoinData schema + instance
// record parsers (pure, VM-free: replicate.cpp only).
//
// Invariants under test: no crashes, no hangs, no overruns on arbitrary
// bytes. Every malformed input must return false with *err (off always
// advances inside Cursor; all takes are bounds-checked; allocations stay
// proportional to the input — strings/values copy input slices, the record
// vector grows at most one entry per consumed byte).
#include "replicate.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    rbxclient::JoinSchema schema;
    std::string err;
    const bool sok =
        rbxclient::parseJoinSchema(data, size, schema, &err);
    // Records against the parsed schema when valid; with an empty schema
    // every record must fail fast on the class-range check.
    std::vector<rbxclient::JoinRecord> recs;
    std::string err2;
    (void)rbxclient::parseJoinRecords(data, size, schema, recs, &err2);
    // Records-only error paths over the second half.
    if (size > 1) {
        std::vector<rbxclient::JoinRecord> recs2;
        std::string err3;
        (void)rbxclient::parseJoinRecords(data + size / 2, size - size / 2,
                                          schema, recs2, &err3);
    }
    (void)sok;
    return 0;
}
