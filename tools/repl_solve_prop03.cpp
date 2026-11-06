// repl_solve_prop03.cpp — derive 0x03 prop-update wire shapes from closure.
//
// Usage: repl_solve_prop03 <chan1.bin> [max_value_len]
//
// For every 0x83 chain that does not close, find the last 0x03 prop item and
// try alternative shapes for that prop id:
//   * every value tag 0..70 (prefix on and off), and
//   * every fixed value length 0..max (framing only).
// A shape is accepted when the whole chain then walks to its end marker (or
// a clean length-framed EOF). Closure is a strong signal: the end marker has
// to land exactly, so a wrong length almost always desyncs later.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

// A chain counts as solved only when it stopped at a real 0x00 end marker
// (final byte), never on length-framed EOF: EOF "closure" is the parse
// landing exactly at the buffer end, which a wrong length can hit by luck.
bool walk(const std::vector<uint8_t>& body, const rbxclient::JoinSchema& schema,
          const rbxclient::PropTagTable& props, rbxclient::Walk83* out) {
    std::string e;
    if (!rbxclient::walk83ItemsEx(body.data(), body.size(), schema, &props,
                                  *out, &e, true))
        return false;
    return out->complete && !body.empty() && body.back() == 0x00;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: repl_solve_prop03 <capture> [maxlen]\n");
        return 2;
    }
    const int maxlen = argc > 2 ? int(std::strtol(argv[2], nullptr, 0)) : 64;
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
    const rbxclient::PropTagTable base = rbxclient::propTagsForSchema(schema);

    // prop id -> solved shapes and counts.
    std::map<int, std::map<std::string, int>> solved;
    int unresolved = 0, stops = 0;
    for (const auto& m : msgs) {
        if (m.body.empty() || m.body[0] != 0x83 || m.body.size() < 2 ||
            m.body[1] == 0x16)
            continue;
        rbxclient::Walk83 w;
        {
            std::string e;
            if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), schema,
                                          &base, w, &e, true))
                continue;
        }
        if (w.complete) continue;
        ++stops;
        // Last parsed item, if it is a prop update.
        int prop = -1;
        if (!w.items.empty() && w.items.back().type == 0x03)
            prop = int(w.items.back().prop03.prop);
        if (prop < 0) {
            ++unresolved;
            continue;
        }
        bool any = false;
        for (int tag = 0; tag <= 70 && !any; ++tag) {
            for (int pfx = 0; pfx <= 1 && !any; ++pfx) {
                rbxclient::PropTagTable t = base;
                t.tag[uint16_t(prop)] = uint8_t(tag);
                t.fixed_len.erase(uint16_t(prop));
                t.alt_len.erase(uint16_t(prop));
                if (pfx) t.prefix[uint16_t(prop)] = true;
                else t.prefix.erase(uint16_t(prop));
                rbxclient::Walk83 w2;
                if (!walk(m.body, schema, t, &w2) || !w2.complete) continue;
                char key[64];
                std::snprintf(key, sizeof key, "tag=0x%02x pfx=%d", tag, pfx);
                ++solved[prop][key];
                any = true;
            }
        }
        for (int len = 0; len <= maxlen && !any; ++len) {
            rbxclient::PropTagTable t = base;
            t.fixed_len[uint16_t(prop)] = size_t(len);
            t.alt_len.erase(uint16_t(prop));
            rbxclient::Walk83 w2;
            if (!walk(m.body, schema, t, &w2) || !w2.complete) continue;
            char key[64];
            std::snprintf(key, sizeof key, "fixed=%d", len);
            ++solved[prop][key];
            any = true;
        }
        if (!any) {
            ++unresolved;
            std::printf("UNRESOLVED chain len=%zu stop=0x%02x prop=%d\n",
                        m.body.size(), unsigned(w.stop_type), prop);
        }
    }
    std::printf("\nstops=%d unresolved=%d\n", stops, unresolved);
    for (const auto& kv : solved) {
        auto own = base.owner_class.find(uint16_t(kv.first));
        auto nm = base.member_name.find(uint16_t(kv.first));
        const char* cls = "?";
        if (own != base.owner_class.end() && own->second < schema.classes.size())
            cls = schema.classes[own->second].name.c_str();
        std::printf("prop %d (%s.%s, schema tag 0x%02x):", kv.first, cls,
                    nm != base.member_name.end() ? nm->second.c_str() : "?",
                    base.tag.count(uint16_t(kv.first))
                        ? unsigned(base.tag.at(uint16_t(kv.first)))
                        : 0u);
        for (const auto& sk : kv.second)
            std::printf(" [%s x%d]", sk.first.c_str(), sk.second);
        std::printf("\n");
    }
    return 0;
}
