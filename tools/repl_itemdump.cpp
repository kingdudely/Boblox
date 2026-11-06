// repl_itemdump.cpp — hexdump 0x83 chain items around a prop id.
//
// Usage: repl_itemdump <chan1.bin> <prop-id> [count]
//
// Prints each message body from 24 bytes before the matching item to 12
// bytes after it, with the item's own span marked, so a byte the walker
// reads short (or one it over-reads) is visible in context instead of
// inferred from a length.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void hexline(const std::vector<uint8_t>& b, size_t from, size_t to,
                    size_t mark_from, size_t mark_to) {
    for (size_t i = from; i < to && i < b.size(); ++i) {
        const bool mark = i >= mark_from && i < mark_to;
        std::printf(mark ? "[%02x]" : " %02x ", unsigned(b[i]));
        if ((i - from) % 8 == 7)
            std::printf("\n");
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: repl_itemdump <chan1.bin> <prop-id> [n]\n");
        return 2;
    }
    const int want = int(std::strtoul(argv[2], nullptr, 0) & 0xFFFFu);
    const int limit = argc > 3 ? int(std::strtol(argv[3], nullptr, 0)) : 4;
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<repltool::CapMsg> msgs = repltool::splitFrames(d);
    std::vector<uint8_t> inner;
    std::string err;
    if (!repltool::extractInner97(msgs, inner, &err)) {
        std::printf("no 0x97: %s\n", err.c_str());
        return 2;
    }
    rbxclient::JoinSchema schema;
    if (!rbxclient::parseJoinSchema(inner.data(), inner.size(), schema, &err)) {
        std::printf("schema: %s\n", err.c_str());
        return 2;
    }
    const rbxclient::PropTagTable props = rbxclient::propTagsForSchema(schema);
    int shown = 0;
    for (const auto& m : msgs) {
        if (m.body.empty() || m.body[0] != 0x83 || m.body.size() < 2 ||
            m.body[1] == 0x16)
            continue;
        rbxclient::Walk83 w;
        std::string e;
        if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), schema,
                                      &props, w, &e, true))
            continue;
        for (size_t i = 0; i < w.items.size() && i < w.spans.size(); ++i) {
            const auto& it = w.items[i];
            if (it.type != 0x03 || it.prop03.prop != want)
                continue;
            const auto& sp = w.spans[i];
            const size_t from = sp.off > 24 ? sp.off - 24 : 0;
            const size_t to = sp.end + 12 < m.body.size() ? sp.end + 12
                                                          : m.body.size();
            std::printf("item prop=%u tag=0x%02x span=[%zu,%zu) msg=%zuB "
                        "complete=%d stop=%02x@%zu\n",
                        unsigned(want), unsigned(it.prop03.tag), sp.off,
                        sp.end, m.body.size(), int(w.complete),
                        unsigned(w.stop_type), w.stop_off);
            std::printf("items:");
            for (size_t k = 0; k < w.spans.size() && k < 12; ++k)
                std::printf(" %02x@%zu-%zu", unsigned(w.spans[k].type),
                            w.spans[k].off, w.spans[k].end);
            std::printf("\n");
            hexline(m.body, from, to, sp.off, sp.end);
            if (++shown >= limit)
                return 0;
        }
    }
    std::printf("prop %u not found in a parsed 0x03 item\n", want);
    return 0;
}
