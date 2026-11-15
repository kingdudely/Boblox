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
#include "solve/repl_pv.h"
#include "solve/replicate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

bool loadSchema(const char* path, rbxclient::JoinSchema& out, std::string* err) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        *err = std::string("cannot open ") + path;
        return false;
    }
    char line[512];
    size_t cur = 0; // class the following "m"/"s" lines belong to
    std::vector<std::vector<uint32_t>> slots;
    using GidOwner = std::pair<uint16_t, uint16_t>;
    std::vector<GidOwner> gid_owner;
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
            if (cur >= out.classes.size())
                continue;
            auto& members = out.classes[cur].members;
            if (members.size() <= idx)
                members.resize(idx + 1);
            members[idx].name = mname;
            members[idx].tag = uint8_t(tag);
            continue;
        }
        unsigned slot = 0, gid = 0;
        if (std::sscanf(line, " s %u %u", &slot, &gid) == 2) {
            if (cur >= slots.size())
                continue;
            auto& list = slots[cur];
            if (list.size() <= slot)
                list.resize(slot + 1);
            list[slot] = gid;
            continue;
        }
        unsigned prop = 0, ptag = 0;
        char pcls[256], pmem[256];
        if (std::sscanf(line, " prop %u tag %u class %255s member %255s",
                        &prop, &ptag, pcls, pmem) == 4) {
            for (size_t ci = 0; ci < out.classes.size(); ++ci) {
                if (out.classes[ci].name != pcls)
                    continue;
                for (size_t mi = 0; mi < out.classes[ci].members.size();
                     ++mi) {
                    if (out.classes[ci].members[mi].name != pmem)
                        continue;
                    if (gid_owner.size() <= prop)
                        gid_owner.resize(prop + 1);
                    gid_owner[prop] = {uint16_t(ci), uint16_t(mi)};
                    break;
                }
                break;
            }
        }
    }
    std::fclose(f);
    if (out.classes.empty()) {
        *err = "schema has no classes";
        return false;
    }
    // Rebuild the flat global-member table exactly like parseJoinSchema does:
    // slots carry global ids, and resolution needs the (class, member) they
    // name. Order is class-major, member-minor, so it matches the live table
    // only when every class keeps its wire member count.
    out.member_by_gid.clear();
    if (!gid_owner.empty()) {
        // The dump carries the authoritative prop-id -> (class, member) map.
        // Rebuilding it as a flat class-major array is wrong when a class has
        // gaps/sentinels (the 189707 schema has 10,411 prop ids against 10,408
        // listed members), and then every slot resolves to the wrong tag.
        out.member_by_gid = std::move(gid_owner);
    } else {
        for (size_t ci = 0; ci < out.classes.size(); ++ci)
            for (size_t mi = 0; mi < out.classes[ci].members.size(); ++mi)
                out.member_by_gid.push_back(
                    {uint16_t(ci), uint16_t(mi)});
    }
    if (slots.size() == out.classes.size())
        out.slots = std::move(slots);
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
    // Partial records: a mask bit that the tail's slot list does not name.
    // Two distinct causes, and they are different bugs:
    //   slot_past_end  idx >= slots[class].size()  (mask wider than the list)
    //   gid_past_end   slot varint >= member_by_gid.size()  (list names a
    //                  global id the schema table does not have)
    struct Part {
        int count = 0;
        int64_t cls = -1;
        size_t bit = 0;
        size_t named = 0;
        std::string body;
    };
    std::map<std::string, Part> parts;
    int frames = 0, frame_fail = 0, records = 0, ok = 0, bad = 0;
    int pv_values = 0, pv_fail = 0;
    int pad_records = 0;
    size_t pad_bytes = 0, pad_max = 0;
    size_t total_body = 0, total_decoded = 0, total_skip = 0;
    size_t total_guard = 0, total_unaccounted = 0;
    size_t partial_records = 0, partial_body = 0;
    size_t skip_other_mask = 0;
    FILE* tail_csv = nullptr;
    if (const char* cp = std::getenv("A1_TAIL_CSV")) {
        tail_csv = std::fopen(cp, "w");
        if (tail_csv)
            std::fprintf(tail_csv,
                         "file,record,id,cls,body_len,body_off,body_end,used,pad,flags,kind,"
                         "mask,body_hex,value_hex,tail_hex,next_hex\n");
    }
    struct PadShape {
        int count = 0;
        int nonzero = 0;
        std::string sample;
        std::string body_sample;
    };
    // (body length, first body byte, orientation kind, skipped byte count).
    std::map<std::tuple<int, int, int, int>, PadShape> pad_shapes;
    // Attribute schemas persist per entity across records and frames. The
    // native sender only includes a schema bit in the record that establishes
    // it, so a values-only record can only be sized against this live map.
    rbxclient::A1AttributeSchemas attr_schemas;

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
        size_t rec_index = 0;
        for (const auto& r : frame.records) {
            ++records;
            ++rec_index;
            if (r.cls >= schema.classes.size())
                continue;
            std::vector<rbxclient::JoinProp> props;
            std::vector<uint8_t> tail;
            std::string berr;
            bool partial = false;
            size_t end_padding = 0;
            rbxclient::A1BodyAccounting acct;
            const size_t body_len =
                r.data.size() >= 2
                    ? size_t(r.data[0]) | (size_t(r.data[1]) << 8)
                    : 0;
            if (rbxclient::parseAppStateBody(r.data.data() + 2,
                                            body_len,
                                            schema, r, &props, &berr, &tail,
                                            &partial, nullptr,
                                            &attr_schemas,
                                            r.data.size() - 2, &end_padding,
                                            r.guard.data(), r.guard.size(),
                                            &acct)) {
                ++ok;
                if (partial) {
                    // An unresolved pre-capture attribute schema is a known
                    // external gap, not silent end padding; keep it out of the
                    // closed-record accounting and report it separately.
                    ++partial_records;
                    partial_body += acct.body_bytes;
                } else {
                    total_body += acct.body_bytes;
                    total_decoded += acct.decoded_bytes;
                    total_skip += acct.end_seek_skipped;
                    total_guard += acct.guard_bytes;
                    total_unaccounted += acct.unaccounted;
                }
                if (end_padding) {
                    ++pad_records;
                    pad_bytes += end_padding;
                    if (end_padding > pad_max)
                        pad_max = end_padding;
                    if (!(r.mask_words.size() == 1 &&
                          r.mask_words[0] == 4))
                        ++skip_other_mask;
                    const size_t body_len =
                        r.data.size() >= 2
                            ? size_t(r.data[0]) | (size_t(r.data[1]) << 8)
                            : 0;
                    if (body_len >= end_padding) {
                        const uint8_t* pb = r.data.data() + 2;
                        const size_t skip_off = body_len - end_padding;
                        PadShape& ps =
                            pad_shapes[{int(body_len), int(pb[0]),
                                        int((pb[0] >> 3) & 3),
                                        int(end_padding)}];
                        ++ps.count;
                        for (size_t i = 0; i < end_padding; ++i) {
                            if (pb[skip_off + i] != 0) {
                                ++ps.nonzero;
                                break;
                            }
                        }
                        if (ps.sample.empty())
                            ps.sample = hex(pb + skip_off, end_padding, 16);
                        if (ps.body_sample.empty())
                            ps.body_sample = hex(pb, body_len, 40);
                        if (tail_csv) {
                            const rbxclient::JoinProp* pv = nullptr;
                            for (const auto& p : props)
                                if (p.tag == 57) {
                                    pv = &p;
                                    break;
                                }
                            std::fprintf(
                                tail_csv, "%s,%zu,%u,%u,%d,%zu,%zu,%zu,%zu,%d,%d,"
                                          "%016llx,%s,%s,%s,%s\n",
                                files[fi].c_str(), rec_index, r.id, r.cls,
                                int(body_len), r.body_offset,
                                r.body_offset + body_len,
                                pv ? int(pv->value.size())
                                   : int(body_len - end_padding),
                                int(end_padding), int(pb[0]),
                                int((pb[0] >> 3) & 3),
                                (unsigned long long)(r.mask_words.empty()
                                                         ? 0
                                                         : r.mask_words[0]),
                                hex(pb, body_len, 40).c_str(),
                                pv ? hex(pv->value.data(), pv->value.size(), 48)
                                         .c_str()
                                   : "",
                                hex(pb + body_len - end_padding,
                                    end_padding, 16)
                                    .c_str(),
                                r.body_offset + body_len <= raw.size()
                                    ? hex(raw.data() + r.body_offset + body_len,
                                          std::min<size_t>(8, raw.size() -
                                                                  (r.body_offset +
                                                                   body_len)),
                                          8)
                                          .c_str()
                                    : "");
                        }
                    }
                }
                for (const auto& p : props) {
                    if (p.tag != 57)
                        continue;
                    rbxclient::ReplicationPV pv;
                    size_t used = 0;
                    std::string perr;
                    if (rbxclient::decodeReplicationPV(
                            p.value.data(), p.value.size(), pv, &used,
                            &perr) &&
                        used == p.value.size())
                        ++pv_values;
                    else
                        ++pv_fail;
                }
                if (partial) {
                    // A1_TAIL_DIR: keep every partial record's unread bytes
                    // (the attribute section) so its grammar can be fitted
                    // offline over thousands of samples instead of a handful.
                    if (const char* td = std::getenv("A1_TAIL_DIR")) {
                        static int tn = 0;
                        char path[1024];
                        std::snprintf(path, sizeof path, "%s/t%05d.bin", td, tn++);
                        if (FILE* fp = std::fopen(path, "wb")) {
                            std::fwrite(tail.data(), 1, tail.size(), fp);
                            std::fclose(fp);
                        }
                    }
                    // Walk the mask the way the body parser does and name the
                    // first slot it could not resolve.
                    const auto& pc = schema.classes[r.cls];
                    const bool have_sl =
                        r.cls < schema.slots.size() && !schema.slots[r.cls].empty();
                    const size_t named = have_sl ? schema.slots[r.cls].size() : 0;
                    size_t bit = 0, cnt_slots = named;
                    bool found = false;
                    for (size_t w = 0; w < r.mask_words.size() && !found; ++w) {
                        uint64_t bits = r.mask_words[w];
                        while (bits) {
                            const size_t b2 = size_t(__builtin_ctzll(bits));
                            bits &= bits - 1;
                            const size_t idx = w * 64 + b2;
                            const rbxclient::SchemaMember* mp = nullptr;
                            if (have_sl) {
                                if (idx < schema.slots[r.cls].size()) {
                                    const uint32_t gid = schema.slots[r.cls][idx];
                                    if (gid < schema.member_by_gid.size()) {
                                        const auto own = schema.member_by_gid[gid];
                                        if (own.first < schema.classes.size() &&
                                            own.second < schema.classes[own.first]
                                                               .members.size())
                                            mp = &schema.classes[own.first]
                                                       .members[own.second];
                                    }
                                }
                            } else if (idx < pc.members.size()) {
                                mp = &pc.members[idx];
                            }
                            if (!mp) {
                                bit = idx;
                                found = true;
                                break;
                            }
                        }
                    }
                    std::string key =
                        pc.name + (!found ? " attributes"
                                 : bit >= cnt_slots ? " slot_past_end"
                                                    : " gid_past_end");
                    Part& pp = parts[key];
                    if (pp.count++ == 0) {
                        pp.cls = r.cls;
                        pp.bit = bit;
                        pp.named = cnt_slots;
                        pp.body = hex(r.data.data() + 2,
                                      r.data.size() >= 2 ? r.data.size() - 2 : 0,
                                      40);
                    }
                }
                continue;
            }
            ++bad;
            const auto& cls = schema.classes[r.cls];
            std::string members;
            // Resolve each selected slot the way parseAppStateBody does: the
            // mask indexes the writer's slot list, whose entries are global
            // member ids. Falling back to direct indexing here would name a
            // different member than production and misreport the gap.
            const bool have_slots =
                r.cls < schema.slots.size() && !schema.slots[r.cls].empty();
            for (size_t w = 0; w < r.mask_words.size(); ++w) {
                uint64_t bits = r.mask_words[w];
                while (bits) {
                    const size_t bit = size_t(__builtin_ctzll(bits));
                    bits &= bits - 1;
                    const size_t idx = w * 64 + bit;
                    const rbxclient::SchemaMember* mp = nullptr;
                    if (have_slots) {
                        const std::vector<uint32_t>& list = schema.slots[r.cls];
                        if (idx < list.size()) {
                            const uint32_t gid = list[idx];
                            if (gid < schema.member_by_gid.size()) {
                                const auto own = schema.member_by_gid[gid];
                                if (own.first < schema.classes.size() &&
                                    own.second <
                                        schema.classes[own.first].members.size())
                                    mp = &schema.classes[own.first]
                                               .members[own.second];
                            }
                        }
                    } else if (idx < cls.members.size()) {
                        mp = &cls.members[idx];
                    }
                    if (!mp)
                        continue;
                    char b[160];
                    std::snprintf(b, sizeof b, "%s%s(tag %u)",
                                  members.empty() ? "" : ",",
                                  mp->name.c_str(), unsigned(mp->tag));
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

    std::printf("frames_ok=%d frame_fail=%d records=%d closed=%d tailed=%d "
                "pv=%d pv_fail=%d pad=%d/%zu max_pad=%zu\n",
                frames, frame_fail, records, ok, bad, pv_values, pv_fail,
                pad_records, pad_bytes, pad_max);
    std::printf("body bytes: total=%zu decoded=%zu end_skipped=%zu "
                "guard=%zu unaccounted=%zu\n",
                total_body, total_decoded, total_skip, total_guard,
                total_unaccounted);
    std::printf("partial bodies: records=%zu bytes=%zu\n", partial_records,
                partial_body);
    std::printf("end-seek records whose mask is not exactly bit 2: %zu\n",
                skip_other_mask);
    if (total_body != total_decoded + total_skip + total_unaccounted) {
        std::printf("FAIL: body byte accounting does not sum\n");
        return 1;
    }
    if (!pad_shapes.empty()) {
        std::vector<std::pair<int, std::tuple<int, int, int, int>>> po;
        for (const auto& kv : pad_shapes)
            po.push_back({kv.second.count, kv.first});
        std::sort(po.begin(), po.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::printf("\nend-seek skip shapes (body flags kind pad):\n");
        for (const auto& kv : po) {
            const auto& k = kv.second;
            const PadShape& ps = pad_shapes[k];
            std::printf("%6d  bl%d f%02x k%d pad%d  nonzero=%d  skip=%s bod=%s\n",
                        ps.count, std::get<0>(k), std::get<1>(k),
                        std::get<2>(k), std::get<3>(k), ps.nonzero,
                        ps.sample.c_str(), ps.body_sample.c_str());
        }
    }
    std::vector<std::pair<int, std::string>> order;
    for (const auto& kv : shapes)
        order.push_back({kv.second.count, kv.first});
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (!parts.empty()) {
        std::vector<std::pair<int, std::string>> po;
        for (const auto& kv : parts)
            po.push_back({kv.second.count, kv.first});
        std::sort(po.begin(), po.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        int ptotal = 0;
        for (const auto& kv : po) ptotal += kv.first;
        std::printf("\npartial records: %d\n", ptotal);
        for (const auto& kv : po) {
            const Part& pp = parts[kv.second];
            const char* nm =
                pp.cls >= 0 && size_t(pp.cls) < schema.classes.size()
                    ? schema.classes[size_t(pp.cls)].name.c_str()
                    : "?";
            std::printf("%6d  %s (%s) bit=%zu named=%zu\n        body %s\n",
                        kv.first, kv.second.c_str(), nm, pp.bit, pp.named,
                        pp.body.c_str());
        }
    }
    for (const auto& kv : order) {
        const Shape& s = shapes[kv.second];
        std::printf("\n%6d  %s\n", s.count, kv.second.c_str());
        std::printf("        body %s\n", s.body.c_str());
        std::printf("        tail %s\n", s.tail.c_str());
    }
    return 0;
}
