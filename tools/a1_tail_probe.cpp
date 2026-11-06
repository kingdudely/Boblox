// a1_tail_probe.cpp — offline census of 0xA1 record bodies and their leftover
// bytes, using a live schema dump and a directory of saved frames.
//
//   a1_tail_probe <schema.txt> <frames-dir> [max-frames]
//
// schema.txt is what RBX_DUMP_SCHEMA writes (`classes N props M` then one
// `class <id> <Name> members <n>` line per class and `  m <i> <Name> tag <t>`
// per member). Each frame file holds the app message with its 0xA1 id byte
// first, exactly as RBX_DUMP_APP writes it.
//
// For every record whose body does not close on its selected members it prints
// one line per (class, leftover-length) shape: the class, the selected member
// names with their tags, the body hex and the leftover hex. That answers
// "which member's value decode is leaving bytes" instead of guessing from the
// tail alone.
#include "solve/replicate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

bool loadSchema(const char* path, rbxclient::JoinSchema& out, std::string* err) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        *err = std::string("cannot open ") + path;
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        unsigned cid = 0, n = 0;
        char name[256];
        if (std::sscanf(line, "class %u %255s members %u", &cid, name, &n) == 3) {
            if (out.classes.size() <= cid)
                out.classes.resize(cid + 1);
            out.classes[cid].name = name;
            out.classes[cid].members.clear();
            continue;
        }
        unsigned idx = 0, tag = 0;
        char mname[256];
        if (std::sscanf(line, " m %u %255s tag %u", &idx, mname, &tag) == 3) {
            if (out.classes.empty())
                continue;
            auto& members = out.classes.back().members;
            if (members.size() <= idx)
                members.resize(idx + 1);
            members[idx].name = mname;
            members[idx].tag = uint8_t(tag);
        }
    }
    std::fclose(f);
    if (out.classes.empty()) {
        *err = "schema has no classes";
        return false;
    }
    return true;
}

std::string hex(const uint8_t* d, size_t n, size_t max = 40) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n && i < max; ++i) {
        if (!s.empty())
            s.push_back(' ');
        s.push_back(k[d[i] >> 4]);
        s.push_back(k[d[i] & 15]);
    }
    if (n > max)
        s += " ..";
    return s;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buf[1 << 16];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <schema.txt> <frames-dir> [max-frames]\n",
                     argv[0]);
        return 2;
    }
    rbxclient::JoinSchema schema;
    std::string err;
    if (!loadSchema(argv[1], schema, &err)) {
        std::fprintf(stderr, "schema: %s\n", err.c_str());
        return 1;
    }
    std::printf("schema: %zu classes\n", schema.classes.size());

    std::vector<std::string> files;
    {
        std::string cmd = std::string("ls -1 ") + argv[2] + "/*.bin 2>/dev/null";
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char line[1024];
            while (std::fgets(line, sizeof line, p)) {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                    s.pop_back();
                if (!s.empty())
                    files.push_back(s);
            }
            pclose(p);
        }
    }
    const size_t max_frames =
        argc > 3 ? size_t(std::atoi(argv[3])) : files.size();
    std::printf("frames: %zu (probing %zu)\n", files.size(),
                files.size() < max_frames ? files.size() : max_frames);

    // key -> count / sample
    struct Shape {
        int count = 0;
        std::string members;
        std::string body;
        std::string tail;
    };
    std::map<std::string, Shape> shapes;
    int frames = 0, frame_fail = 0, records = 0, ok = 0, bad = 0;

    for (size_t fi = 0; fi < files.size() && fi < max_frames; ++fi) {
        std::vector<uint8_t> raw = readFile(files[fi]);
        if (raw.size() < 2)
            continue;
        // The dump includes the 0xA1 id first, and the frame parser expects it.
        rbxclient::AppStateFrame frame;
        std::string ferr;
        if (!rbxclient::parseAppStateFrame(raw.data(), raw.size(), schema,
                                          &frame, &ferr)) {
            ++frame_fail;
            if (frame_fail <= 5)
                std::printf("frame %s: %s\n", files[fi].c_str(), ferr.c_str());
            continue;
        }
        ++frames;
        for (const auto& r : frame.records) {
            ++records;
            if (r.cls >= schema.classes.size())
                continue;
            std::vector<rbxclient::JoinProp> props;
            std::vector<uint8_t> tail;
            std::string berr;
            if (rbxclient::parseAppStateBody(r.data.data() + 2,
                                            r.data.size() >= 2 ? size_t(r.data[0]) |
                                                                     (size_t(r.data[1]) << 8)
                                                               : 0,
                                            schema, r, &props, &berr, &tail)) {
                ++ok;
                continue;
            }
            ++bad;
            const auto& cls = schema.classes[r.cls];
            std::string members;
            for (size_t w = 0; w < r.mask_words.size(); ++w) {
                uint64_t bits = r.mask_words[w];
                while (bits) {
                    const size_t bit = size_t(__builtin_ctzll(bits));
                    bits &= bits - 1;
                    const size_t idx = w * 64 + bit;
                    if (idx >= cls.members.size())
                        continue;
                    char b[160];
                    std::snprintf(b, sizeof b, "%s%s(tag %u)",
                                  members.empty() ? "" : ",",
                                  cls.members[idx].name.c_str(),
                                  unsigned(cls.members[idx].tag));
                    members += b;
                }
            }
            // The reason string is the discriminator: a refused value (empty
            // tail) and an unmodelled trailing section (non-empty tail) are
            // different bugs and must not be counted together.
            // Locate the record inside the frame so the bytes *before* the
            // body length (mask width / marker) can be seen at a known offset.
            std::string before;
            if (r.data.size() >= 4) {
                const uint8_t* p = raw.data();
                const uint8_t* q = std::search(p, p + raw.size(), r.data.data(),
                                               r.data.data() + 4);
                if (q != p + raw.size() && q >= p + 16)
                    before = hex(q - 16, 16);
            }
            std::string key = cls.name + " tail=" + std::to_string(tail.size()) +
                              " members=" + std::to_string(props.size()) +
                              " mc=" + std::to_string(r.member_count) +
                              " sel=[" + members + "] err=" + berr + " pre=" + before;
            Shape& s = shapes[key];
            if (s.count++ == 0) {
                s.members = berr;
                s.body = hex(r.data.data() + 2,
                             r.data.size() >= 2 ? r.data.size() - 2 : 0, 48);
                s.tail = hex(tail.data(), tail.size(), 40);
            }
        }
    }

    std::printf("frames_ok=%d frame_fail=%d records=%d closed=%d tailed=%d\n",
                frames, frame_fail, records, ok, bad);
    std::vector<std::pair<int, std::string>> order;
    for (const auto& kv : shapes)
        order.push_back({kv.second.count, kv.first});
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& kv : order) {
        const Shape& s = shapes[kv.second];
        std::printf("\n%6d  %s\n", s.count, kv.second.c_str());
        std::printf("        body %s\n", s.body.c_str());
        std::printf("        tail %s\n", s.tail.c_str());
    }
    return 0;
}
