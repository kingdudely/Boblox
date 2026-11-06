// repl_prop_lookup.cpp — print schema identity for prop ids from a capture's 0x97.
//
// Usage: repl_prop_lookup <chan1.bin> <prop-id> [...]
//
// Prints class/member/tag/prefix/fixed/alt for each id, so a 0x03 prop-update
// stop can be named instead of guessed.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: repl_prop_lookup <chan1.bin> <id> [...]\n");
        return 2;
    }
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
    const rbxclient::PropTagTable t = rbxclient::propTagsForSchema(schema);
    for (int a = 2; a < argc; ++a) {
        const uint16_t pid =
            uint16_t(std::strtoul(argv[a], nullptr, 0) & 0xFFFFu);
        auto tag = t.tag.find(pid);
        auto own = t.owner_class.find(pid);
        auto nm = t.member_name.find(pid);
        std::printf("prop %u:", unsigned(pid));
        if (tag == t.tag.end()) {
            std::printf(" (not in table)\n");
            continue;
        }
        const char* cls = "?";
        if (own != t.owner_class.end() && own->second < schema.classes.size())
            cls = schema.classes[own->second].name.c_str();
        std::printf(" %s.%s tag=0x%02x prefix=%d", cls,
                    nm != t.member_name.end() ? nm->second.c_str() : "?",
                    unsigned(tag->second), t.prefix.count(pid) ? 1 : 0);
        auto fl = t.fixed_len.find(pid);
        if (fl != t.fixed_len.end())
            std::printf(" fixed=%zu", fl->second);
        auto al = t.alt_len.find(pid);
        if (al != t.alt_len.end())
            std::printf(" alt=%zu", al->second);
        std::printf("\n");
    }
    return 0;
}
