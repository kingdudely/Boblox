// repl_stop_probe.cpp — diagnose 0x83 chain stops from a saved chan1 capture.
//
// Usage: repl_stop_probe <chan1.bin> [stop-type-filter]
//
// For every 0x83 chain that does not close, prints: chain length, stop offset,
// stop byte, the last parsed item (type/class), and — when that item is a
// spawn — every property with its tag and decoded value length, plus the raw
// bytes from the last item start to the stop. A stop byte outside the native
// item-type range (0x01..0x29) can never be an item type, so it proves the
// previous item's length was wrong; the prop dump names the culprit.
//
// Build (same ad-hoc recipe as the other repl_* tools):
//   g++ -std=c++17 -O2 tools/repl_stop_probe.cpp -Itools -Iclient/src -o \
//       /tmp/repl_stop_probe build/client/librbx_solve.a -lzstd -pthread
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

const char* type_name(uint8_t t) {
    switch (t) {
    case 0x00: return "end";
    case 0x01: return "ref";
    case 0x02: return "spawn";
    case 0x03: return "prop";
    case 0x04: return "u32";
    case 0x05: return "timing";
    case 0x06: return "timing2";
    case 0x07: return "event";
    case 0x09: return "u8checked";
    case 0x0b: return "batch";
    case 0x0d: return "delta";
    case 0x10: return "smallu32";
    case 0x11: return "appearance";
    case 0x13: return "guidpair";
    case 0x14: return "triplet+u32";
    case 0x15: return "ref+strings";
    case 0x18: return "triplet+u32b";
    case 0x19: return "3f32+f64+u32";
    case 0x1a: return "u32+triplet";
    case 0x1b: return "reflist";
    case 0x1d: return "refb";
    case 0x1e: return "refc";
    case 0x1f: return "u8s+blob";
    case 0x20: return "blob";
    case 0x23: return "clock";
    case 0x24: return "u32b";
    case 0x25: return "batch+ref";
    case 0x26: return "zigzag";
    case 0x27: return "u32+15f32";
    case 0x28: return "u32c";
    case 0x29: return "reflist-bool";
    default: return "?";
    }
}

std::string hex(const uint8_t* d, size_t n, size_t max = 48) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n && i < max; ++i) {
        s.push_back(k[d[i] >> 4]);
        s.push_back(k[d[i] & 15]);
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: repl_stop_probe <chan1.bin> [type]\n");
        return 2;
    }
    const int only = argc > 2 ? int(std::strtol(argv[2], nullptr, 0)) : -1;
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<repltool::CapMsg> msgs = repltool::splitFrames(d);
    std::vector<uint8_t> inner97;
    std::string err;
    if (!repltool::extractInner97(msgs, inner97, &err)) {
        std::printf("no 0x97: %s\n", err.c_str());
        return 2;
    }
    rbxclient::JoinSchema schema;
    if (!rbxclient::parseJoinSchema(inner97.data(), inner97.size(), schema,
                                    &err)) {
        std::printf("schema: %s\n", err.c_str());
        return 2;
    }
    const rbxclient::PropTagTable props = rbxclient::propTagsForSchema(schema);

    std::map<int, int> stop_hist;
    int chains = 0, closed = 0, stopped = 0, hard = 0, shown = 0,
        batches = 0, dumped = 0;
    for (const auto& m : msgs) {
        if (m.body.empty() || m.body[0] != 0x83)
            continue;
        // 0x83/0x16 is a compressed delta batch, not a plain item chain: the
        // live client routes it to parseDeltaBatch before the walker (see
        // LiveRuntime::onChain). Walking it here would report garbage stops.
        if (m.body.size() >= 2 && m.body[1] == 0x16) {
            ++batches;
            continue;
        }
        ++chains;
        rbxclient::Walk83 w;
        std::string e;
        if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), schema,
                                      &props, w, &e, true)) {
            ++hard;
            if (hard <= 4)
                std::printf("HARD len=%zu %s\n", m.body.size(), e.c_str());
            continue;
        }
        if (w.complete) {
            ++closed;
            continue;
        }
        ++stopped;
        ++stop_hist[int(w.stop_type)];
        if (only >= 0 && int(w.stop_type) != only)
            continue;
        if (shown++ >= 40)
            continue;
        std::printf("\nSTOP len=%zu off=%zu type=0x%02x (%s) items=%zu\n",
                    m.body.size(), w.stop_off, unsigned(w.stop_type),
                    type_name(w.stop_type), w.items.size());
        std::printf("  head: %s\n", hex(m.body.data(), 48).c_str());
        std::printf("  spans:");
        for (size_t si = w.spans.size() > 10 ? w.spans.size() - 10 : 0;
             si < w.spans.size(); ++si)
            std::printf(" %zu:0x%02x-%zu", w.spans[si].off,
                        unsigned(w.spans[si].type), w.spans[si].end);
        std::printf("\n");
        if (!w.items.empty()) {
            const rbxclient::ChainItem& last = w.items.back();
            std::printf("  last item type=0x%02x", unsigned(last.type));
            if (last.type == 0x02) {
                const auto& sp = last.spawn;
                std::printf(" class=%s id=%u props=%zu",
                            sp.cls < schema.classes.size()
                                ? schema.classes[sp.cls].name.c_str()
                                : "?",
                            unsigned(sp.id), sp.props_b.size() +
                                                 sp.props_a.size());
                for (const auto& p : last.spawn.props_a)
                    std::printf("\n    a %s tag=0x%02x len=%zu", p.name.c_str(),
                                unsigned(p.tag), p.value.size());
                for (const auto& p : last.spawn.props_b)
                    std::printf("\n    b %s tag=0x%02x len=%zu", p.name.c_str(),
                                unsigned(p.tag), p.value.size());
            } else if (last.type == 0x03) {
                const auto& pt = last.prop03;
                std::printf(" id=%u prop=%u tag=0x%02x vlen=%zu",
                            unsigned(pt.id), unsigned(pt.prop),
                            unsigned(pt.tag), pt.value.size());
                std::printf("\n    value: %s",
                            hex(pt.value.data(), pt.value.size(), 64)
                                .c_str());
            }
            std::printf("\n");
        }
        const char* dump_env = std::getenv("RBX_DUMP_STOP");
        const int dump_want =
            dump_env ? int(std::strtol(dump_env, nullptr, 0)) : -1;
        const size_t stop_at =
            w.spans.empty() ? size_t(0) : w.spans.back().off;
        if (dump_want >= 0 && dumped == dump_want) {
            std::FILE* f = std::fopen("/tmp/stop_chain.hex", "w");
            if (f) {
                std::fprintf(f, "%s",
                             hex(m.body.data() + stop_at,
                                 m.body.size() - stop_at, 1 << 20)
                                 .c_str());
                std::fclose(f);
            }
            std::FILE* g = std::fopen("/tmp/stop_body.hex", "w");
            if (g) {
                std::fprintf(g, "%s",
                             hex(m.body.data(), m.body.size(), 1 << 20)
                                 .c_str());
                std::fclose(g);
            }
            std::printf("  dumped %zu bytes from %zu to /tmp/stop_chain.hex\n",
                        m.body.size() - stop_at, stop_at);
        }
        ++dumped;
        std::printf("  bytes from %zu: %s\n", stop_at,
                    hex(m.body.data() + stop_at,
                        m.body.size() - stop_at, 96)
                        .c_str());
        // Raw bytes of the last three parsed spans: the true item grammar
        // shows up directly when the final value length is wrong.
        for (size_t si = w.spans.size() > 3 ? w.spans.size() - 3 : 0;
             si < w.spans.size(); ++si) {
            const auto& sp = w.spans[si];
            if (sp.end <= sp.off)
                continue;
            std::printf("  raw %zu:0x%02x-%zu: %s\n", sp.off, unsigned(sp.type),
                        sp.end,
                        hex(m.body.data() + sp.off, sp.end - sp.off, 48)
                            .c_str());
        }
    }
    std::printf("\nchains=%d closed=%d stopped=%d hard=%d batches(0x16)=%d\n",
                chains, closed, stopped, hard, batches);
    for (const auto& kv : stop_hist)
        std::printf("stop 0x%02x (%s): %d\n", unsigned(kv.first),
                    type_name(uint8_t(kv.first)), kv.second);
    return 0;
}
