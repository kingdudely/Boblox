// repl_propstat.cpp — per-prop 0x03 item statistics split by chain outcome.
//
// Usage: repl_propstat <chan1.bin> [topN]
//
// For every 0x03 item: prop id, schema tag, first value byte and the byte
// that follows our span (the byte the walker stopped on). Grouping by
// (prop, first byte) and splitting closed vs stopped shows whether a
// one-byte value prefix is a property trait (all instances of the prop
// carry it) or a message trait.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

struct Stat {
    int closed = 0, stopped = 0;
    int tag = -1;
    int first = -1;
    int next = -1;
    std::string cls, name;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: repl_propstat <chan1.bin> [topN]\n");
        return 2;
    }
    const int topN = argc > 2 ? std::atoi(argv[2]) : 40;
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
    std::map<uint16_t, Stat> st;
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
            if (it.type != 0x03)
                continue;
            Stat& s = st[it.prop03.prop];
            s.tag = int(it.prop03.tag);
            s.first = it.prop03.value.empty() ? -1 : int(it.prop03.value[0]);
            const size_t end = w.spans[i].end;
            s.next = end < m.body.size() ? int(m.body[end]) : -1;
            if (w.complete)
                ++s.closed;
            else
                ++s.stopped;
            auto tg = props.tag.find(it.prop03.prop);
            (void)tg;
            auto own = props.owner_class.find(it.prop03.prop);
            if (own != props.owner_class.end() &&
                own->second < schema.classes.size())
                s.cls = schema.classes[own->second].name;
            auto nm = props.member_name.find(it.prop03.prop);
            if (nm != props.member_name.end())
                s.name = nm->second;
        }
    }
    std::printf("%-6s %-4s %-5s %-5s %-5s %-6s %-6s %s\n", "prop", "tag",
                "first", "next", "closed", "stopped", "total", "member");
    int shown = 0;
    for (const auto& kv : st) {
        if (kv.second.stopped == 0)
            continue;
        std::printf("%-6u 0x%02x 0x%02x  0x%02x  %-6d %-7d %-6d %s.%s\n",
                    unsigned(kv.first), unsigned(kv.second.tag),
                    unsigned(kv.second.first), unsigned(kv.second.next),
                    kv.second.closed, kv.second.stopped,
                    kv.second.closed + kv.second.stopped,
                    kv.second.cls.c_str(), kv.second.name.c_str());
        if (++shown >= topN)
            break;
    }
    std::printf("\n-- tag counts among stopped props --\n");
    std::map<int, int> tagcnt;
    for (const auto& kv : st)
        if (kv.second.stopped)
            ++tagcnt[kv.second.tag];
    for (const auto& kv : tagcnt)
        std::printf("tag 0x%02x: %d props\n", unsigned(kv.first), kv.second);
    return 0;
}
