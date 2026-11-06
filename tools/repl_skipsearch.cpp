// repl_skipsearch.cpp — diagnose one 0x16 spawn-batch record.
//
// Usage:
//   repl_skipsearch <batch.bin> <chan1.bin> <rec_off>            # walk props
//   repl_skipsearch <batch.bin> <chan1.bin> <rec_off> scan       # next starts
//   repl_skipsearch <batch.bin> <chan1.bin> <rec_off> len <at>   # value lens
//
// The 0x16 payload is a chain of [ref][class][props][parent] records. When a
// prop's tag has an unknown wire shape the walk stops mid-value; these modes
// show where the chain can resynchronise (scan) and what each candidate value
// length does to the rest of the record (len).
#include "solve/repl_internal.h"
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace rbxclient;

namespace {

int count_from(const std::vector<uint8_t>& d, size_t from,
               const JoinSchema& schema) {
    size_t off = from;
    int n = 0;
    while (off < d.size() && n < 4000) {
        JoinRecord r;
        size_t used = 0;
        std::string err;
        if (!parseJoinRecord(d.data() + off, d.size() - off, schema, r, &used,
                             &err) ||
            used == 0)
            break;
        off += used;
        ++n;
    }
    return n;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: repl_skipsearch <batch.bin> <chan1.bin> <off> "
                     "[scan | len <at>]\n");
        return 2;
    }
    const std::vector<uint8_t> batch = repltool::slurpFile(argv[1]);
    const std::vector<repltool::CapMsg> msgs =
        repltool::splitFrames(repltool::slurpFile(argv[2]));
    std::vector<uint8_t> inner;
    std::string err;
    if (!repltool::extractInner97(msgs, inner, &err)) {
        std::printf("no 0x97: %s\n", err.c_str());
        return 2;
    }
    JoinSchema schema;
    if (!parseJoinSchema(inner.data(), inner.size(), schema, &err)) {
        std::printf("schema: %s\n", err.c_str());
        return 2;
    }
    const size_t off = size_t(std::strtoll(argv[3], nullptr, 0));
    if (off >= batch.size()) {
        std::printf("offset past end\n");
        return 2;
    }
    const char* mode = argc > 4 ? argv[4] : "walk";

    if (std::strcmp(mode, "scan") == 0) {
        size_t best_at = 0, best_n = 0;
        for (size_t s = off + 8; s < batch.size() && s < off + 1200; ++s) {
            const int n = count_from(batch, s, schema);
            if (size_t(n) > best_n) {
                best_n = size_t(n);
                best_at = s;
            }
            if (n >= 5)
                std::printf("start +%zu: %d records\n", s - off, n);
        }
        std::printf("best start +%zu: %zu records\n", best_at - off, best_n);
        return 0;
    }

    // Header: record ref + class.
    detail::Cursor c(batch.data() + off, batch.size() - off);
    uint64_t lo = 0;
    int64_t id = -1;
    const uint8_t* b = nullptr;
    if (!detail::read_ref(c, lo, id, &err, "record") ||
        !c.take(2, &b, &err, "class")) {
        std::printf("header: %s\n", err.c_str());
        return 2;
    }
    const uint16_t cls = detail::u16le(b);
    if (cls >= schema.classes.size()) {
        std::printf("class %u out of range\n", unsigned(cls));
        return 2;
    }
    const std::vector<SchemaMember>& members = schema.classes[cls].members;
    const std::string& cname = schema.classes[cls].name;
    std::printf("record id=%lld class=%u (%s)\n", (long long)id, unsigned(cls),
                cname.c_str());

    if (std::strcmp(mode, "len") == 0) {
        const size_t value_at =
            argc > 5 ? size_t(std::strtoll(argv[5], nullptr, 0)) : off + c.off();
        for (size_t L = 0; L <= 300; ++L) {
            if (value_at + L >= batch.size())
                break;
            detail::Cursor t(batch.data() + value_at + L,
                             batch.size() - value_at - L);
            std::vector<JoinProp> rest;
            std::string e;
            if (!detail::read_prop_loop(t, members, cname, false, rest, &e))
                continue;
            uint64_t plo = 0;
            int64_t pid = -1;
            if (!detail::read_ref(t, plo, pid, &e, "parent"))
                continue;
            std::printf("L=%zu loop_end=+%zu parent_lo=%llu parent=%lld "
                        "props=%zu next=",
                        L, batch.size() - t.left - off,
                        (unsigned long long)plo, (long long)pid, rest.size());
            for (size_t i = 0; i < 8 && t.left > 0; ++i)
                std::printf("%02x", t.p[i]);
            std::printf("\n");
            for (const auto& pp : rest)
                std::printf("    then %s tag=%u len=%zu\n", pp.name.c_str(),
                            unsigned(pp.tag), pp.value.size());
        }
        return 0;
    }

    while (true) {
        uint8_t delta = 0;
        if (!c.u8(&delta, &err, "delta")) {
            std::printf("walk: %s\n", err.c_str());
            return 2;
        }
        if (delta == 0xFF) {
            std::printf("loop closed at +%zu\n", c.off());
            uint64_t plo = 0;
            int64_t pid = -1;
            if (!detail::read_ref(c, plo, pid, &err, "parent")) {
                std::printf("parent: %s\n", err.c_str());
                return 2;
            }
            std::printf("parent lo=%llu id=%lld, record end +%zu\n",
                        (unsigned long long)plo, (long long)pid, c.off());
            return 0;
        }
        if (delta >= members.size()) {
            std::printf("delta %u out of range\n", unsigned(delta));
            return 2;
        }
        const SchemaMember& m = members[delta];
        const size_t before = c.off();
        std::vector<uint8_t> raw;
        if (!detail::read_tagged_value(c, m.tag, false, cname + "." + m.name,
                                       raw, &err)) {
            std::printf("prop[%u] %s tag=%u rec+%zu failed: %s\n", unsigned(delta),
                        m.name.c_str(), unsigned(m.tag), before, err.c_str());
            std::printf("bytes at value: ");
            for (size_t i = off + before;
                 i < batch.size() && i < off + before + 64; ++i)
                std::printf("%02x", batch[i]);
            std::printf("\n");
            return 0;
        }
        std::printf("prop[%u] %s tag=%u rec+%zu..+%zu\n", unsigned(delta),
                    m.name.c_str(), unsigned(m.tag), before, c.off());
    }
}
