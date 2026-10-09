// fuzz_blob.cpp — libFuzzer harness for the 0x9B blob parsers (Phase K).
//
// Covers extractChallenge + decodeBlob chained exactly like the production
// solve path (solve.cpp), plus decodeBlob probed directly on the raw input
// (blobs without a valid 0x9B header).
//
// Invariants under test: no crashes (ASan/UBSan clean), no hangs, no
// uncaught exceptions (bad_alloc/length_error from attacker-sized fields),
// bounded memory (decodeBlob caps the zstd output size).
#include "blob.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;

    // Production chain: header first, then the extracted blob.
    rbxclient::Challenge ch;
    if (rbxclient::extractChallenge(data, size, ch, &err)) {
        std::vector<uint8_t> wire;
        rbxclient::decodeBlob(ch.blob.data(), ch.blob.size(), wire, &err);
    }

    // Raw probe: the blob decoder on headerless input.
    {
        std::vector<uint8_t> wire;
        rbxclient::decodeBlob(data, size, wire, &err);
    }
    return 0;
}
