// repl_coverage.cpp — offline replication coverage for a saved chan1 stream.
//
// Usage: repl_coverage <chan1.bin> [join97.raw]
//
// Walks every frame, parses the 0x97 schema (from the capture, or from
// join97.raw when the capture has none), walks every 0x83 chain with the
// shipped defaultPropTags, and parses every 0x16 batch by flags. Reports
// exact-closes, faithful stops (pinned type:count), hard fails (first few
// with offsets), batch breakdown, and per-type / per-prop / per-tag use.
// Exit 0 always (coverage is measured, never gated); humans gate releases.
//
// Build (matches the ad-hoc cov tools):
//   g++ -std=c++17 -O2 tools/repl_coverage.cpp -Iclient/src -o \
//       /tmp/opencode/repl_coverage build/client/librbx_solve.a -lzstd -pthread
#include "replicate.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

uint64_t cv(const uint8_t* p, size_t n, size_t* k) {
    size_t nn = size_t(1) << (p[0] >> 6);
    if (nn > n)
        return UINT64_MAX;
    uint64_t v = p[0] & 0x3F;
    for (size_t i = 1; i < nn; i++)
        v = (v << 8) | p[i];
    *k = nn;
    return v;
}

std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: repl_coverage <chan1.bin> [join97.raw]\n");
        return 2;
    }
    const std::vector<uint8_t> d = slurp(argv[1]);
    struct Msg {
        uint8_t id;
        std::vector<uint8_t> body;
        size_t off;
    };
    std::vector<Msg> msgs;
    size_t off = 0;
    while (off < d.size()) {
        size_t k = 0;
        uint64_t ln = cv(d.data() + off, d.size() - off, &k);
        if (k == 0 || ln == 0 || off + k + ln > d.size()) {
            std::printf("FRAMEFAIL @%zu\n", off);
            return 0;
        }
        Msg m;
        m.id = d[off + k];
        m.body.assign(d.data() + off + k, d.data() + off + k + ln);
        m.off = off;
        msgs.push_back(std::move(m));
        off += k + ln;
    }

    // Schema: 0x97 inner from the capture, else the join97.raw fallback
    // (same place/version joins share the schema; cross-game use REQUIRES
    // the capture's own 0x97 — the fallback is Crossroads-only).
    std::vector<uint8_t> inner97;
    for (auto& m : msgs)
        if (m.id == 0x97) {
            std::string e;
            if (!rbxclient::parseJoinOuter(m.body.data(), m.body.size(),
                                           inner97, &e)) {
                std::printf("97-OUTER-FAIL %s\n", e.c_str());
                return 0;
            }
            break;
        }
    if (inner97.empty() && argc > 2)
        inner97 = slurp(argv[2]);
    if (inner97.empty()) {
        std::printf("no 0x97 snapshot in %s (pass join97.raw)\n", argv[1]);
        return 0;
    }
    rbxclient::JoinSchema s;
    {
        std::string e;
        if (!rbxclient::parseJoinSchema(inner97.data(), inner97.size(), s,
                                        &e)) {
            std::printf("SCHEMA-FAIL %s\n", e.c_str());
            return 0;
        }
    }
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();

    int pure = 0, part = 0, hard = 0;
    std::map<int, int> stops;
    std::map<int, int> type_use;
    std::map<uint16_t, int> prop_use;
    std::map<uint8_t, int> tag_use;
    int nb = 0, nbr = 0, nbss = 0, nbna = 0, nbe = 0, nbf = 0;
    std::map<int, int> batchflags;
    std::vector<std::string> fails;
    for (auto& m : msgs) {
        if (m.id != 0x83)
            continue;
        if (m.body.size() >= 3 && m.body[1] == 0x16) {
            nb++;
            batchflags[m.body[2]]++;
            std::vector<uint8_t> bin;
            std::string e;
            if (m.body[2] == 5) {
                // NetAsset batch: raw body after the 3B header.
                std::vector<rbxclient::NetAssetEntry> na;
                if (rbxclient::parseNetAssetBatch(m.body.data() + 3,
                                                  m.body.size() - 3, na, &e))
                    nbna += int(na.size());
                else {
                    nbf++;
                    if (fails.size() < 8)
                        fails.push_back("nabatch @" +
                                        std::to_string(m.off) + ": " + e);
                }
                continue;
            }
            if (!rbxclient::parseDeltaBatch(m.body.data(), m.body.size(),
                                            bin, &e)) {
                nbf++;
                if (fails.size() < 8)
                    fails.push_back("batch @" + std::to_string(m.off) +
                                    ": " + e);
                continue;
            }
            if (bin.empty()) {
                nbe++;
                continue;
            }
            if (m.body[2] == 1) {
                std::vector<rbxclient::SharedString> ss;
                if (rbxclient::parseSharedStrings(bin.data(), bin.size(), ss,
                                                  &e))
                    nbss++;
                else {
                    nbf++;
                    if (fails.size() < 8)
                        fails.push_back("shared @" + std::to_string(m.off) +
                                        ": " + e);
                }
            } else {
                std::vector<rbxclient::JoinRecord> r;
                if (rbxclient::parseJoinRecords(bin.data(), bin.size(), s, r,
                                                &e))
                    nbr++;
                else {
                    nbf++;
                    if (fails.size() < 8)
                        fails.push_back("records @" + std::to_string(m.off) +
                                        ": " + e);
                }
            }
            continue;
        }
        rbxclient::Walk83 w;
        std::string e;
        if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), s, &pt,
                                      w, &e)) {
            hard++;
            if (fails.size() < 8)
                fails.push_back("hard @" + std::to_string(m.off) + ": " + e);
            continue;
        }
        for (auto& it : w.items) {
            type_use[it.type]++;
            if (it.type == 0x03) {
                prop_use[it.prop03.prop]++;
                tag_use[it.prop03.tag]++;
            }
        }
        if (w.complete && w.end_off == m.body.size())
            pure++;
        else {
            part++;
            stops[w.stop_type]++;
            if (fails.size() < 8 && stops[w.stop_type] <= 2)
                fails.push_back("stop " + std::to_string(w.stop_type) +
                                " @" + std::to_string(m.off));
        }
    }
    std::printf("%s: chains pure=%d part=%d hard=%d batches=%d(rec=%d ss=%d na=%d empty=%d fail=%d)\n",
                argv[1], pure, part, hard, nb, nbr, nbss, nbna, nbe, nbf);
    std::printf("schema classes=%zu prop_bound=%llu ev_bound=%llu\n",
                s.classes.size(), (unsigned long long)s.prop_bound,
                (unsigned long long)s.ev_bound);
    if (!batchflags.empty()) {
        std::printf("batchflags:");
        for (auto& kv : batchflags)
            std::printf(" %02x:%d", kv.first, kv.second);
        std::printf("\n");
    }
    if (!stops.empty()) {
        std::printf("stops:");
        for (auto& kv : stops)
            std::printf(" %02x:%d", kv.first, kv.second);
        std::printf("\n");
    }
    if (!type_use.empty()) {
        std::printf("types:");
        for (auto& kv : type_use)
            std::printf(" %02x:%d", kv.first, kv.second);
        std::printf("\n");
    }
    if (!prop_use.empty()) {
        std::printf("props:");
        for (auto& kv : prop_use)
            std::printf(" %u:%d", kv.first, kv.second);
        std::printf("\ntags:");
        for (auto& kv : tag_use)
            std::printf(" %u:%d", kv.first, kv.second);
        std::printf("\n");
    }
    for (auto& f : fails)
        std::printf("  %s\n", f.c_str());
    return 0;
}
