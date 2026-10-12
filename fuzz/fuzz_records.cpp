// fuzz_records.cpp — libFuzzer harness for the JoinData schema + instance
// record parsers plus the live streaming items (spawn/event/0x83 chain):
// pure, VM-free (replicate.cpp only).
//
// Invariants under test: no crashes, no hangs, no overruns on arbitrary
// bytes. Every malformed input must return false with *err (off always
// advances inside Cursor; all takes are bounds-checked; allocations stay
// proportional to the input — strings/values copy input slices, the record
// vector grows at most one entry per consumed byte; the chain walker
// advances past every type byte and stops (never loops) on deferred
// types, and spawn/event items consume >= 1 byte or fail).
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
    // Streaming items against the same schema (empty schema fails fast on
    // the class/event range checks). Both pad modes for the spawn head.
    {
        rbxclient::SpawnRecord sp;
        size_t used = 0;
        std::string e4, e5;
        (void)rbxclient::parseSpawnItem(data, size, schema, false, sp, &used,
                                        &e4);
        (void)rbxclient::parseSpawnItem(data, size, schema, true, sp, &used,
                                        &e5);
        rbxclient::EventItem ev;
        std::string e6;
        (void)rbxclient::parseEventItem(data, size, schema, ev, &used, &e6);
        rbxclient::Walk83 w;
        std::string e7;
        (void)rbxclient::walk83Items(data, size, schema, w, &e7);
        // Table-driven walk (0x03 triples via the default tag map; typeless
        // script-data bulks via the fallback) + batch inner entry lists.
        const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
        rbxclient::Walk83 w2;
        std::string e8;
        (void)rbxclient::walk83ItemsEx(data, size, schema, &pt, w2, &e8);
        rbxclient::PropTriple triple;
        std::string e9;
        (void)rbxclient::parsePropTriple(data, size, pt, triple, &used, &e9);
        rbxclient::BulkItem bulk;
        std::string e10;
        (void)rbxclient::parseBulkItem(data, size, bulk, &used, &e10);
        std::vector<rbxclient::SharedString> ss;
        std::string e11;
        (void)rbxclient::parseSharedStrings(data, size, ss, &e11);
        rbxclient::Item10 v10;
        std::string e12;
        (void)rbxclient::parseItem10(data, size, v10, &used, &e12);
        rbxclient::Item20 v20;
        std::string e13;
        (void)rbxclient::parseItem20(data, size, v20, &used, &e13);
    }
    (void)sok;
    return 0;
}
