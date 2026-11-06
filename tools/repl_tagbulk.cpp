// repl_tagbulk.cpp — list inline spawn bulks that are NOT Source-keyed.
//
// Usage: repl_tagbulk <capture.bin> [max]
//
// The Tags property (schema tag 46) replicates as a 16-byte content hash, and
// the spawn repeats that hash as an inline content bulk whose payload is the
// tag data itself. This tool prints each such spawn: class, Name, the matched
// key tag, and the payload as text plus hex, so the blob's encoding can be
// inferred from real servers instead of guessed.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string hex(const std::vector<uint8_t>& d, size_t max = 64) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < d.size() && i < max; ++i) {
        s.push_back(k[d[i] >> 4]);
        s.push_back(k[d[i] & 15]);
    }
    return s;
}

std::string text(const std::vector<uint8_t>& d) {
    std::string s;
    for (uint8_t c : d) {
        if (c >= 0x20 && c < 0x7f)
            s.push_back(char(c));
        else
            s.push_back('.');
    }
    return s;
}

// Named string-ish prop from either loop (tag 49 Name is [len][bytes]).
const std::vector<uint8_t>* prop_with_tag(const rbxclient::SpawnRecord& sp,
                                          uint8_t tag) {
    for (const auto& p : sp.props_a)
        if (p.tag == tag)
            return &p.value;
    for (const auto& p : sp.props_b)
        if (p.tag == tag)
            return &p.value;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: repl_tagbulk <capture.bin> [max]\n");
        return 2;
    }
    const size_t max = argc > 2 ? size_t(std::strtoul(argv[2], nullptr, 0)) : 40;
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

    size_t chains = 0, stops = 0, hard = 0, shown = 0, total = 0;
    for (const auto& m : msgs) {
        if (m.body.empty() || m.body[0] != 0x83)
            continue;
        if (m.body.size() >= 2 && m.body[1] == 0x16)
            continue;
        ++chains;
        rbxclient::Walk83 w;
        std::string e;
        if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), schema,
                                      &props, w, &e, true)) {
            ++hard;
            continue;
        }
        if (!w.complete)
            ++stops;
        for (const auto& it : w.items) {
            if (it.type != 0x02 || !it.spawn.has_bulk)
                continue;
            if (it.spawn.bulk.key_tag == 47)
                continue;
            ++total;
            if (shown++ >= max)
                continue;
            const auto& sp = it.spawn;
            std::string name;
            if (const auto* n = prop_with_tag(sp, 49); n && !n->empty()) {
                const size_t k = (*n)[0] <= n->size() - 1 ? (*n)[0] : 0;
                name.assign(n->begin() + 1, n->begin() + 1 + k);
            }
            std::printf("key_tag=%u cls=%s name=%s payload=%zuB text='%s' hex=%s\n",
                        unsigned(sp.bulk.key_tag),
                        sp.cls < schema.classes.size()
                            ? schema.classes[sp.cls].name.c_str()
                            : "?",
                        name.c_str(), sp.bulk.payload.size(),
                        text(sp.bulk.payload).c_str(),
                        hex(sp.bulk.payload).c_str());
        }
    }
    std::printf("chains=%zu stops=%zu hard=%zu tag_bulks=%zu\n", chains, stops,
                hard, total);
    return 0;
}
