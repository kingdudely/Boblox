// repl_prop03.cpp — census of 0x03 prop-update items across a capture.
//
// Usage: repl_prop03 <chan1.bin> [tag]
//
// Walks every 0x83 chain with the schema-derived prop table and reports, for
// each (tag, first value byte) pair, how often it occurs. The point is to see
// whether a tag's update shape carries a leading format byte the snapshot
// shape does not (the live f32 case: value looks shifted by one).
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: repl_prop03 <chan1.bin> [tag]\n");
        return 2;
    }
    const int only_tag = argc > 2 ? int(std::strtol(argv[2], nullptr, 0)) : -1;
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

    std::map<std::pair<int, int>, int> hist; // (tag, first byte) -> count
    int items = 0, chains = 0, strict_closed = 0, strict_stopped = 0,
        eof_closed = 0;
    for (const auto& m : msgs) {
        if (m.body.empty() || m.body[0] != 0x83 || m.body.size() < 2 ||
            m.body[1] == 0x16)
            continue;
        ++chains;
        rbxclient::Walk83 w;
        std::string e;
        if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), schema,
                                      &props, w, &e, true))
            continue;
        if (!w.complete) ++strict_stopped;
        else if (!m.body.empty() && m.body.back() == 0x00) ++strict_closed;
        else ++eof_closed;
        for (size_t ii = 0; ii < w.items.size(); ++ii) {
            const auto& it = w.items[ii];
            if (it.type != 0x03)
                continue;
            ++items;
            const int tag = int(it.prop03.tag);
            const int first = it.prop03.value.empty()
                                  ? -1
                                  : int(it.prop03.value[0]);
            ++hist[std::make_pair(tag, first)];
            if (only_tag >= 0 && tag != only_tag)
                continue;
            if (hist[std::make_pair(tag, first)] <= 6) {
                std::printf("tag=0x%02x prop=%u first=0x%02x vlen=%zu val=",
                            unsigned(tag), unsigned(it.prop03.prop),
                            first < 0 ? 0 : unsigned(first),
                            it.prop03.value.size());
                for (size_t i = 0; i < it.prop03.value.size() && i < 24; ++i)
                    std::printf("%02x", it.prop03.value[i]);
                // Bytes after the item's span (next item start): shows the
                // true value tail when our length is short. Items and spans
                // are pushed one-for-one, so ii indexes both.
                std::printf(" next=");
                const size_t endoff =
                    ii < w.spans.size() ? w.spans[ii].end : 0;
                if (endoff)
                    for (size_t i = endoff;
                         i < m.body.size() && i < endoff + 16; ++i)
                        std::printf("%02x", m.body[i]);
                std::printf("\n");
            }
        }
    }
    std::printf("\nchains=%d propitems=%d strict_closed=%d strict_stopped=%d"
                " eof_closed=%d\n",
                chains, items, strict_closed, strict_stopped, eof_closed);
    for (const auto& kv : hist)
        std::printf("tag=0x%02x first=0x%02x: %d\n", unsigned(kv.first.first),
                    kv.first.second < 0 ? 0 : unsigned(kv.first.second),
                    kv.second);
    return 0;
}
