// repl_coverage.cpp — offline replication coverage for a saved chan1 stream.
//
// Usage: repl_coverage <chan1.bin> [join97.raw]
//
// Walks every frame, parses the 0x97 schema (from the capture, or from
// join97.raw when the capture has none), walks every 0x83 chain with the
// schema-derived propTagsForSchema table, and parses every 0x16 batch by
// flags. Reports
// exact-closes, faithful stops (pinned type:count), hard fails (first few
// with offsets), batch breakdown, and per-type / per-prop / per-tag use.
// Exit 0 always (coverage is measured, never gated); humans gate releases.
//
// Build (matches the ad-hoc cov tools):
//   g++ -std=c++17 -O2 tools/repl_coverage.cpp -Itools -Iclient/src -o \
//       /tmp/opencode/repl_coverage build/client/librbx_solve.a -lzstd -pthread
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: repl_coverage <chan1.bin> [join97.raw]\n");
        return 2;
    }
    using repltool::CapMsg;
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<CapMsg> msgs = repltool::splitFrames(d);

    // Schema: 0x97 inner from the capture, else the join97.raw fallback
    // (same place/version joins share the schema; cross-game use REQUIRES
    // the capture's own 0x97 — the fallback is Crossroads-only).
    std::vector<uint8_t> inner97;
    {
        std::string e;
        if (!repltool::extractInner97(msgs, inner97, &e)) {
            // extractInner97 fails on missing AND corrupt 0x97; keep the
            // original diagnostics split (outer vs absent).
            bool saw97 = false;
            for (const auto& m : msgs)
                if (!m.body.empty() && m.body[0] == 0x97) {
                    saw97 = true;
                    break;
                }
            if (saw97) {
                std::printf("97-OUTER-FAIL %s\n", e.c_str());
                return 0;
            }
        }
    }
    if (inner97.empty() && argc > 2)
        inner97 = repltool::slurpFile(argv[2]);
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
    const rbxclient::PropTagTable pt = rbxclient::propTagsForSchema(s);

    int pure = 0, gap = 0, hard = 0, trailclosed = 0, trailopen = 0,
        resyncok = 0;
    std::map<int, int> stops;
    std::map<int, int> resynckinds;
    std::map<int, int> type_use;
    std::map<uint16_t, int> prop_use;
    std::map<uint8_t, int> tag_use;
    // Top-level frame census: every message id accounted (nothing silent).
    std::map<int, int> top_use;
    int top8d = 0, top8d_fail = 0;
    int nb = 0, nbr = 0, nbss = 0, nbna = 0, nbe = 0, nbf = 0;
    std::map<int, int> batchflags;
    std::vector<std::string> fails;
    for (auto& m : msgs) {
        if (m.body.empty())
            continue;
        top_use[m.body[0]]++;
        if (m.body[0] == 0x8D) {
            rbxclient::RegionChunk rc;
            std::string e;
            if (rbxclient::parseRegionChunk(m.body.data(), m.body.size(),
                                            rc, &e))
                top8d++;
            else {
                top8d_fail++;
                if (fails.size() < 8)
                    fails.push_back("8d @" + std::to_string(m.off) + ": " +
                                    e);
            }
            continue;
        }
        if (m.body[0] != 0x83)
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
        if (w.complete && w.end_off == m.body.size()) {
            pure++;
            continue;
        }
        if (w.complete) {
            // Premature 00 end with trailing bytes: probe past the false
            // marker — if the rest walks exact-close, every byte is
            // accounted (one short-read upstream, prefix + rest applied).
            std::vector<uint8_t> t;
            t.push_back(0x83);
            t.insert(t.end(), m.body.begin() + w.end_off, m.body.end());
            rbxclient::Walk83 w2;
            std::string e2;
            if (rbxclient::walk83ItemsEx(t.data(), t.size(), s, &pt, w2,
                                         &e2) &&
                w2.complete && w2.end_off == t.size()) {
                trailclosed++;
                continue;
            }
            trailopen++;
            continue;
        }
        // Segment probe: skip 1..64 bytes at the stop; if the rest walks
        // exact-close, the skipped span is one atomic unknown item
        // (prefix + rest applied, span counted). Native-throwing first
        // bytes here are faithful drops (0xa3 = 24B and 0xf4 = 15B skewed
        // items, proven by deep resync); anything else is a gap.
        bool resync = false;
        for (int skip = 1; skip <= 64 && !resync; ++skip) {
            if (w.stop_off + size_t(skip) >= m.body.size())
                break;
            std::vector<uint8_t> t;
            t.push_back(0x83);
            t.insert(t.end(), m.body.begin() + w.stop_off + skip,
                     m.body.end());
            rbxclient::Walk83 w2;
            std::string e2;
            if (rbxclient::walk83ItemsEx(t.data(), t.size(), s, &pt, w2,
                                         &e2) &&
                w2.complete && w2.end_off == t.size())
                resync = true;
        }
        if (resync) {
            resyncok++;
            resynckinds[w.stop_type]++;
        } else {
            gap++;
            stops[w.stop_type]++;
            if (fails.size() < 8 && stops[w.stop_type] <= 2)
                fails.push_back("stop " + std::to_string(w.stop_type) +
                                " @" + std::to_string(m.off));
        }
    }
    std::printf("%s: chains pure=%d trailclosed=%d trailopen=%d resync=%d gap=%d hard=%d batches=%d(rec=%d ss=%d na=%d empty=%d fail=%d)\n",
                argv[1], pure, trailclosed, trailopen, resyncok, gap, hard,
                nb, nbr, nbss, nbna, nbe, nbf);
    std::printf("top:");
    for (auto& kv : top_use)
        std::printf(" %02x:%d", kv.first, kv.second);
    std::printf(" | 8d ok=%d fail=%d\n", top8d, top8d_fail);
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
    if (!resynckinds.empty()) {
        std::printf("resync:");
        for (auto& kv : resynckinds)
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
