// schema_tail.cpp — dump the unparsed tail of a captured 0x97 schema.
//
//   schema_tail <frame-or-inner.bin> [bytes]
//
// parseJoinSchema stops after the URL and shared-string tables and records
// JoinSchema::tail_off. Everything after that offset is the part the tree does
// not model yet; the A1 writer's per-class member table (which is wider than
// the property schema: Part records claim 82 slots against 64 members) is the
// leading candidate for what lives there. This prints the tail's size, a hex
// window, and u16/u32 array views so the first structure can be read off the
// bytes instead of guessed.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <zstd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint32_t u32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint16_t u16le(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: schema_tail <frame-or-inner.bin> [bytes] "
                     "[--tail-out path]\n");
        return 2;
    }
    const char* tail_out = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--tail-out") == 0) tail_out = argv[i + 1];
    const size_t window = argc > 2 ? size_t(std::strtoul(argv[2], nullptr, 0)) : 384;
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    if (d.empty()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> inner;
    std::string err;
    if (d[0] == 0x97) {
        // A single app message: 97 <outer magic> <u32 size> <zstd>. The u32 is
        // the decompressed size on live frames but the compressed size in the
        // gdb harness dumps, so a parser that trusts it fails on captures;
        // decompress the payload and let zstd report the size instead.
        if (!rbxclient::parseJoinOuter(d.data(), d.size(), inner, &err)) {
            const uint8_t* src = d.data() + 9;
            const size_t src_n = d.size() - 9;
            std::vector<uint8_t> out;
            out.resize(ZSTD_getFrameContentSize(src, src_n) < 4u << 20 &&
                               ZSTD_getFrameContentSize(src, src_n) != ZSTD_CONTENTSIZE_UNKNOWN &&
                               ZSTD_getFrameContentSize(src, src_n) != ZSTD_CONTENTSIZE_ERROR
                           ? size_t(ZSTD_getFrameContentSize(src, src_n))
                           : size_t(1) << 24);
            const size_t got = ZSTD_decompress(out.data(), out.size(), src, src_n);
            if (ZSTD_isError(got)) {
                std::fprintf(stderr, "outer: %s / zstd: %s\n", err.c_str(),
                             ZSTD_getErrorName(got));
                return 2;
            }
            out.resize(got);
            inner.swap(out);
        }
    } else {
        const std::vector<repltool::CapMsg> msgs = repltool::splitFrames(d);
        if (!repltool::extractInner97(msgs, inner, &err) || inner.empty()) {
            // The capture may already be the inner payload (the gdb harness
            // writes it after decompression in some runs).
            inner = d;
        }
    }
    rbxclient::JoinSchema schema;
    if (!rbxclient::parseJoinSchema(inner.data(), inner.size(), schema, &err)) {
        std::fprintf(stderr, "schema: %s\n", err.c_str());
        return 2;
    }
    const size_t off = schema.tail_off;
    if (off > inner.size()) {
        std::fprintf(stderr, "tail offset %zu past inner %zu\n", off, inner.size());
        return 2;
    }
    const size_t tail = inner.size() - off;
    const uint8_t* t = inner.data() + off;
    std::printf("inner=%zu tail_off=%zu tail=%zu classes=%zu props=%llu\n", inner.size(),
                off, tail, schema.classes.size(),
                static_cast<unsigned long long>(schema.prop_bound));

    if (tail_out) {
        if (FILE* f = std::fopen(tail_out, "wb")) {
            std::fwrite(t, 1, tail, f);
            std::fclose(f);
            std::printf("wrote tail to %s\n", tail_out);
        }
    }

    const size_t rows = (window < tail ? window : tail);
    for (size_t i = 0; i < rows; i += 16) {
        std::printf("%06zx ", i);
        for (size_t k = 0; k < 16; ++k) {
            if (i + k < rows)
                std::printf("%02x ", t[i + k]);
            else
                std::printf("   ");
        }
        std::printf(" ");
        for (size_t k = 0; k < 16 && i + k < rows; ++k) {
            const uint8_t c = t[i + k];
            std::printf("%c", c >= 32 && c < 127 ? char(c) : '.');
        }
        std::printf("\n");
    }

    // Structural read: the tail opens with the class count, then one list per
    // class as [u32 class_id][u32 count][count * u32 global member id]. The
    // global ids are the schema's member concatenation, which is why they climb
    // to prop_bound-1 and repeat (an inherited member is listed by every class
    // that carries it). Print the per-class delta against the parsed schema so
    // the classes whose A1 slot table is wider than their member list stand out.
    if (tail >= 4 && u32le(t) == schema.classes.size()) {
        size_t q = 4;
        size_t lists = 0, ids_total = 0, wider = 0;
        std::printf("\n-- per-class slot lists --\n");
        while (q + 8 <= tail && lists < schema.classes.size()) {
            const uint32_t cid = u32le(t + q);
            const uint32_t cnt = u32le(t + q + 4);
            if (cid >= schema.classes.size() || q + 8 + size_t(cnt) * 4 > tail) break;
            const size_t members = schema.classes[cid].members.size();
            if (cnt != members) {
                ++wider;
                if (wider <= 20)
                    std::printf("  class %u %-28s members=%zu slots=%u\n", cid,
                                schema.classes[cid].name.c_str(), members, cnt);
            }
            ids_total += cnt;
            q += 8 + size_t(cnt) * 4;
            ++lists;
        }
        std::printf("  lists=%zu ids=%zu consumed=%zu/%zu wider=%zu\n", lists, ids_total, q,
                    tail, wider);
    }

    std::printf("\n-- u32 array view (first 24) --\n");
    for (size_t i = 0; i + 4 <= tail && i < 96; i += 4)
        std::printf("  [%03zu] %u\n", i, u32le(t + i));
    std::printf("-- u16 array view (first 24) --\n");
    for (size_t i = 0; i + 2 <= tail && i < 48; i += 2)
        std::printf("  [%03zu] %u\n", i, unsigned(u16le(t + i)));
    return 0;
}
