// a1_57_width.cpp — how wide is tag 57 (BasePart.ReplicationPV) on the wire?
//
//   a1_57_width <schema.txt> <frames-dir> [max-frames]
//
// The value reader refuses tag 57 because its shape is not modelled, and every
// refusal throws away the rest of the record. When 57 is followed by other
// selected members, the body itself is a constraint: exactly one width lets the
// remaining members decode and land on the body's last byte. This tool tries
// every width, keeps only the ones that close, and prints the histogram plus
// the bytes at the value's start, which is what a variable shape would key on.
#include "solve/repl_internal.h"
#include "solve/replicate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using rbxclient::AppStateFrame;
using rbxclient::AppStateRecord;
using rbxclient::JoinSchema;
using rbxclient::SchemaClass;
using rbxclient::SchemaMember;
namespace detail = rbxclient::detail;

bool loadSchema(const char* path, JoinSchema& out, std::string* err) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        *err = std::string("cannot open ") + path;
        return false;
    }
    char line[512];
    size_t cur = 0;
    std::vector<std::vector<uint32_t>> slots;
    while (std::fgets(line, sizeof line, f)) {
        unsigned cid = 0, n = 0;
        char name[256];
        if (std::sscanf(line, "class %u %255s members %u", &cid, name, &n) == 3) {
            if (out.classes.size() <= cid) {
                out.classes.resize(cid + 1);
                slots.resize(cid + 1);
            }
            out.classes[cid].name = name;
            out.classes[cid].members.clear();
            cur = cid;
            continue;
        }
        unsigned idx = 0, tag = 0;
        char mname[256];
        if (std::sscanf(line, " m %u %255s tag %u", &idx, mname, &tag) == 3) {
            if (cur >= out.classes.size()) continue;
            auto& members = out.classes[cur].members;
            if (members.size() <= idx) members.resize(idx + 1);
            members[idx].name = mname;
            members[idx].tag = uint8_t(tag);
            continue;
        }
        unsigned slot = 0, gid = 0;
        if (std::sscanf(line, " s %u %u", &slot, &gid) == 2) {
            if (cur >= slots.size()) continue;
            auto& list = slots[cur];
            if (list.size() <= slot) list.resize(slot + 1);
            list[slot] = gid;
        }
    }
    std::fclose(f);
    if (out.classes.empty()) {
        *err = "schema has no classes";
        return false;
    }
    out.member_by_gid.clear();
    for (size_t ci = 0; ci < out.classes.size(); ++ci)
        for (size_t mi = 0; mi < out.classes[ci].members.size(); ++mi)
            out.member_by_gid.push_back({uint16_t(ci), uint16_t(mi)});
    if (slots.size() == out.classes.size()) out.slots = std::move(slots);
    return true;
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

// The same resolution parseAppStateBody performs: a mask slot indexes the
// writer's slot list, whose entries are global member ids.
const SchemaMember* resolve(const JoinSchema& schema, const AppStateRecord& r,
                           size_t idx, const SchemaClass** owning) {
    const SchemaClass* cls = &schema.classes[r.cls];
    *owning = cls;
    const bool have_slots =
        r.cls < schema.slots.size() && !schema.slots[r.cls].empty();
    if (have_slots) {
        const std::vector<uint32_t>& list = schema.slots[r.cls];
        if (idx < list.size()) {
            const uint32_t gid = list[idx];
            if (gid < schema.member_by_gid.size()) {
                const auto own = schema.member_by_gid[gid];
                if (own.first < schema.classes.size() &&
                    own.second < schema.classes[own.first].members.size()) {
                    *owning = &schema.classes[own.first];
                    return &schema.classes[own.first].members[own.second];
                }
            }
        }
        return nullptr;
    }
    if (idx < cls->members.size()) return &cls->members[idx];
    return nullptr;
}

std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (!s.empty()) s.push_back(' ');
        s.push_back(k[d[i] >> 4]);
        s.push_back(k[d[i] & 15]);
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <schema.txt> <frames-dir> [max-frames]\n", argv[0]);
        return 2;
    }
    JoinSchema schema;
    std::string err;
    if (!loadSchema(argv[1], schema, &err)) {
        std::fprintf(stderr, "schema: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::string> files;
    {
        std::string cmd = std::string("ls -1 ") + argv[2] + "/*.bin 2>/dev/null";
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char line[1024];
            while (std::fgets(line, sizeof line, p)) {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                    s.pop_back();
                if (!s.empty()) files.push_back(s);
            }
            pclose(p);
        }
    }
    const size_t max_frames = argc > 3 ? size_t(std::atoi(argv[3])) : 400;
    std::printf("schema: %zu classes, frames: %zu\n", schema.classes.size(),
                files.size());

    long recs = 0, with57 = 0, last57 = 0, solved = 0, ambiguous = 0, unsolved = 0;
    std::map<int, long> widths;
    std::map<int, std::string> sample;
    std::map<std::string, long> classes;

    for (size_t fi = 0; fi < files.size() && fi < max_frames; ++fi) {
        const std::vector<uint8_t> raw = readFile(files[fi]);
        if (raw.size() < 2) continue;
        AppStateFrame frame;
        if (!rbxclient::parseAppStateFrame(raw.data(), raw.size(), schema, &frame,
                                          &err))
            continue;
        for (const AppStateRecord& r : frame.records) {
            ++recs;
            if (r.cls >= schema.classes.size()) continue;
            // Selected members in mask order.
            std::vector<std::pair<const SchemaMember*, const SchemaClass*>> sel;
            for (size_t w = 0; w < r.mask_words.size(); ++w) {
                uint64_t bits = r.mask_words[w];
                while (bits) {
                    const size_t bit = size_t(__builtin_ctzll(bits));
                    bits &= bits - 1;
                    const size_t idx = w * 64 + bit;
                    const SchemaClass* own = nullptr;
                    const SchemaMember* m = resolve(schema, r, idx, &own);
                    if (m) sel.push_back({m, own});
                }
            }
            size_t pos57 = sel.size();
            for (size_t i = 0; i < sel.size(); ++i)
                if (sel[i].first->tag == 57) {
                    pos57 = i;
                    break;
                }
            if (pos57 == sel.size()) continue;
            ++with57;
            const size_t body_len =
                r.data.size() >= 2 ? size_t(r.data[0]) | (size_t(r.data[1]) << 8)
                                   : 0;
            if (r.data.size() < 2 || 2 + body_len > r.data.size()) continue;
            const uint8_t* body = r.data.data() + 2;

            // Decode every member before 57; the cursor then sits on its value.
            detail::Cursor c(body, body_len);
            bool ok = true;
            for (size_t i = 0; i < pos57 && ok; ++i) {
                std::vector<uint8_t> raw_value;
                const std::string ctx =
                    sel[i].second->name + "." + sel[i].first->name;
                ok = detail::read_tagged_value(c, sel[i].first->tag, false, ctx,
                                               raw_value, &err);
            }
            if (!ok) continue;
            const size_t p57 = c.off();
            if (pos57 + 1 == sel.size()) {
                ++last57;
                continue;
            }
            // Try every width; a width is right when the remaining members
            // decode and the cursor lands exactly on the body's last byte.
            std::vector<int> closing;
            for (int k = 0; k <= 96; ++k) {
                if (p57 + size_t(k) > body_len) break;
                detail::Cursor t(body + p57 + k, body_len - p57 - size_t(k));
                bool good = true;
                for (size_t i = pos57 + 1; i < sel.size() && good; ++i) {
                    std::vector<uint8_t> raw_value;
                    const std::string ctx =
                        sel[i].second->name + "." + sel[i].first->name;
                    good = detail::read_tagged_value(t, sel[i].first->tag, false,
                                                     ctx, raw_value, &err);
                }
                if (good && t.left == 0) closing.push_back(k);
            }
            if (closing.size() == 1) {
                ++solved;
                ++widths[closing[0]];
                if (!sample.count(closing[0]))
                    sample[closing[0]] =
                        hex(body + p57, std::min<size_t>(24, body_len - p57));
                classes[schema.classes[r.cls].name]++;
            } else if (closing.empty()) {
                ++unsolved;
            } else {
                ++ambiguous;
            }
        }
    }

    std::printf(
        "records=%ld with57=%ld last57=%ld solved=%ld ambiguous=%ld unsolved=%ld\n",
        recs, with57, last57, solved, ambiguous, unsolved);
    std::vector<std::pair<long, int>> order;
    for (const auto& kv : widths) order.push_back({kv.second, kv.first});
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& kv : order) {
        std::printf("\n%6ld  width %d\n", kv.first, kv.second);
        std::printf("        value starts %s\n", sample[kv.second].c_str());
    }
    std::printf("\nclasses: ");
    for (const auto& kv : classes)
        std::printf("%s=%ld ", kv.first.c_str(), kv.second);
    std::printf("\n");
    return 0;
}
