// schema_raw.cpp — byte-level dump of one class's 0x97 schema records.
//
// Usage: schema_raw <chan1.bin> <ClassName>
//
// Prints, per member: the name, the tag byte our parser reads, and the raw
// bytes that follow, with offsets. Used when a member's tag disagrees with
// the native runtime prop table (e.g. Humanoid.MoveDirectionInternal reads
// 65 in the schema but the client resolves 22), to see whether the record
// carries extra bytes the parser is not consuming.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct R {
    const std::vector<uint8_t>& d;
    size_t off = 0;
    bool ok = true;

    uint64_t varint() {
        uint64_t v = 0;
        int shift = 0;
        while (off < d.size()) {
            const uint8_t b = d[off++];
            v |= uint64_t(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        return v;
    }
    std::string str() {
        const uint64_t n = varint();
        std::string s(reinterpret_cast<const char*>(d.data() + off), size_t(n));
        off += size_t(n);
        return s;
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: schema_raw <chan1.bin> <ClassName>\n");
        return 2;
    }
    const std::string want = argv[2];
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<repltool::CapMsg> msgs = repltool::splitFrames(d);
    std::vector<uint8_t> inner;
    std::string err;
    if (!repltool::extractInner97(msgs, inner, &err)) {
        std::fprintf(stderr, "no 0x97: %s\n", err.c_str());
        return 2;
    }
    R r{inner};
    r.varint();          // version
    r.off += 8;          // flags
    const uint64_t nenum = r.varint();
    for (uint64_t i = 0; i < nenum; ++i) {
        r.str();
        ++r.off;         // 0x07 trailer
    }
    const uint64_t nclass = r.varint();
    const uint64_t prop_bound = r.varint();
    const uint64_t ev_bound = r.varint();
    std::printf("classes=%llu prop_bound=%llu ev_bound=%llu\n",
                (unsigned long long)nclass, (unsigned long long)prop_bound,
                (unsigned long long)ev_bound);
    int prop_id = 0;
    for (uint64_t ci = 0; ci < nclass; ++ci) {
        const std::string cls = r.str();
        const uint64_t nmem = r.varint();
        const bool show = cls == want;
        if (show)
            std::printf("class %s (%llu members, first prop id %d)\n",
                        cls.c_str(), (unsigned long long)nmem, prop_id);
        for (uint64_t k = 0; k < nmem; ++k) {
            const std::string nm = r.str();
            const size_t tag_off = r.off;
            const uint8_t tag = inner[r.off++];
            std::string extra;
            if (tag == 60) {
                const uint64_t v = r.varint();
                extra = "varint " + std::to_string(v);
            } else if (tag == 7) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "u16 %02x%02x", inner[r.off],
                              inner[r.off + 1]);
                extra = buf;
                r.off += 2;
            }
            if (show) {
                std::printf("  id=%-6d %-38s tag=%-3u @%zu", prop_id,
                            nm.c_str(), unsigned(tag), tag_off);
                if (!extra.empty())
                    std::printf(" [%s]", extra.c_str());
                std::printf("  next8=");
                for (size_t i = r.off; i < r.off + 8 && i < inner.size(); ++i)
                    std::printf("%02x", inner[i]);
                std::printf("\n");
            }
            ++prop_id;
        }
        r.off += 2; // class flags
        const uint64_t nev = r.varint();
        for (uint64_t k = 0; k < nev; ++k) {
            r.str();
            const uint8_t np = inner[r.off++];
            r.off += 3 * size_t(np);
        }
    }
    return 0;
}
