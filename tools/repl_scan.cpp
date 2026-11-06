// repl_scan.cpp — scan a chan1 capture for a byte pattern and print context.
//
// Usage: repl_scan <chan1.bin> <hex-pattern> [before] [after]
//
// Prints every message id and offset where the pattern occurs with the bytes
// before/after it, so inline content headers can be framed from real data.
#include "repl_toolkit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static std::vector<uint8_t> parse_hex(const char* s) {
    std::vector<uint8_t> out;
    int hi = -1;
    for (const char* p = s; *p; ++p) {
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else {
            out.push_back(uint8_t((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: repl_scan <capture> <hex> [before] [after]\n");
        return 2;
    }
    const std::vector<uint8_t> pat = parse_hex(argv[2]);
    const size_t before = argc > 3 ? size_t(std::strtoul(argv[3], nullptr, 0)) : 24;
    const size_t after = argc > 4 ? size_t(std::strtoul(argv[4], nullptr, 0)) : 12;
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<repltool::CapMsg> msgs = repltool::splitFrames(d);
    int hits = 0;
    for (size_t mi = 0; mi < msgs.size(); ++mi) {
        const auto& m = msgs[mi];
        if (m.body.size() < pat.size()) continue;
        for (size_t i = 0; i + pat.size() <= m.body.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < pat.size(); ++k)
                if (m.body[i + k] != pat[k]) { ok = false; break; }
            if (!ok) continue;
            ++hits;
            const char* dump_env = std::getenv("RBX_SCAN_DUMP");
            if (dump_env && hits == int(std::strtol(dump_env, nullptr, 0))) {
                std::FILE* f = std::fopen("/tmp/scan_body.hex", "w");
                if (f) {
                    static const char* k = "0123456789abcdef";
                    for (uint8_t c : m.body) {
                        std::fputc(k[c >> 4], f);
                        std::fputc(k[c & 15], f);
                    }
                    std::fclose(f);
                }
                std::printf("  (body dumped to /tmp/scan_body.hex, %zuB)\n",
                            m.body.size());
            }
            const size_t s = i > before ? i - before : 0;
            const size_t e = std::min(m.body.size(), i + pat.size() + after);
            std::printf("#%d msg=%zu id=0x%02x off=%zu (%zuB): ",
                        hits, mi, m.body.empty() ? 0u : unsigned(m.body[0]), i,
                        m.body.size());
            for (size_t k = s; k < e; ++k) std::printf("%02x", m.body[k]);
            std::printf("  |");
            for (size_t k = s; k < e; ++k)
                std::printf("%c", (m.body[k] >= 32 && m.body[k] < 127)
                                      ? m.body[k] : '.');
            std::printf("|\n");
        }
    }
    std::printf("hits=%d\n", hits);
    return 0;
}
