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
#include "solve/replicate.h"

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
        rbxclient::Item04 v04;
        std::string e14;
        (void)rbxclient::parseItem04(data, size, v04, &used, &e14);
        rbxclient::Item13 p13;
        std::string e15;
        (void)rbxclient::parseItem13(data, size, p13, &used, &e15);
        rbxclient::Item15 i15;
        std::string e16;
        (void)rbxclient::parseItem15(data, size, i15, &used, &e16);
        rbxclient::Item29 l29;
        std::string e17;
        (void)rbxclient::parseItem29(data, size, l29, &used, &e17);
        rbxclient::Item09 v09;
        std::string e19;
        (void)rbxclient::parseItem09(data, size, v09, &used, &e19);
        rbxclient::Item0b h0b;
        std::string e20;
        (void)rbxclient::parseItem0b(data, size, h0b, &used, &e20);
        rbxclient::Item14 v14;
        std::string e21;
        (void)rbxclient::parseItem14(data, size, v14, &used, &e21);
        rbxclient::Item18 v18;
        std::string e22;
        (void)rbxclient::parseItem18(data, size, v18, &used, &e22);
        rbxclient::Item19 v19;
        std::string e23;
        (void)rbxclient::parseItem19(data, size, v19, &used, &e23);
        rbxclient::Item1a v1a;
        std::string e24;
        (void)rbxclient::parseItem1a(data, size, v1a, &used, &e24);
        rbxclient::Item1d r1d;
        std::string e25;
        (void)rbxclient::parseItem1d(data, size, r1d, &used, &e25);
        rbxclient::Item1e r1e;
        std::string e26;
        (void)rbxclient::parseItem1e(data, size, r1e, &used, &e26);
        rbxclient::Item1f b1f;
        std::string e27;
        (void)rbxclient::parseItem1f(data, size, b1f, &used, &e27);
        rbxclient::Item24 v24;
        std::string e28;
        (void)rbxclient::parseItem24(data, size, v24, &used, &e28);
        rbxclient::Item25 v25;
        std::string e29;
        (void)rbxclient::parseItem25(data, size, v25, &used, &e29);
        rbxclient::Item26 v26;
        std::string e30;
        (void)rbxclient::parseItem26(data, size, false, v26, &used, &e30);
        (void)rbxclient::parseItem26(data, size, true, v26, &used, &e30);
        rbxclient::Item27 v27;
        std::string e31;
        (void)rbxclient::parseItem27(data, size, v27, &used, &e31);
        rbxclient::Item28 v28;
        std::string e32;
        (void)rbxclient::parseItem28(data, size, v28, &used, &e32);
        rbxclient::Item11 a11;
        std::string e33;
        (void)rbxclient::parseItem11(data, size, a11, &used, &e33);
        rbxclient::RegionChunk rc;
        std::string e34;
        (void)rbxclient::parseRegionChunk(data, size, rc, &e34);
        std::vector<rbxclient::NetAssetEntry> na;
        std::string e18;
        (void)rbxclient::parseNetAssetBatch(data, size, na, &e18);
    }
    (void)sok;
    return 0;
}
