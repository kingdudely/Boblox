// fuzz_frames.cpp — libFuzzer harness for the chan1 frame-walk (Phase K):
// findChallengeFrame scans compactVarint-framed session data for the first
// complete 0x9B challenge message (the pure core of Session::tryChallenge).
//
// Invariants under test: no crashes, no hangs (off always advances), and
// the hit contract — an in-bounds message with a complete 13-byte header
// starting with 0x9B.
#include "blob.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    size_t off = 0, len = 0;
    if (rbxclient::findChallengeFrame(data, size, &off, &len)) {
        // Pin the contract: off/len delimit a complete header in bounds.
        // (off <= size and len <= size - off - k by construction, so off +
        // len cannot wrap — the assert documents it, not defends it.)
        assert(off + len <= size);
        assert(len >= 13);
        assert(data[off] == 0x9B);
        // End-to-end along the production path: the found slice feeds
        // extractChallenge (which may still reject it — the inner blob
        // length is attacker-controlled — but must never crash on it).
        rbxclient::Challenge ch;
        std::string err;
        rbxclient::extractChallenge(data + off, len, ch, &err);
    }
    return 0;
}
