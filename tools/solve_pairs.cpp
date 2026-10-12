// solve_pairs.cpp — joint exact-close search for COMPOUND unknowns.
//
// Usage: solve_pairs <chan1.bin> [minmsgs=3]
//
// The single-prop solver (solve_proptags) is exhausted when every
// remaining open message needs >= 2 simultaneous shape fixes: no single
// trial improves any message. This tool trials PAIRS of co-occurring
// uncommon props jointly (reduced 8x8 tag shortlists from single-trial
// rankings). Winners (>= minmsgs pure closes over pair-messages, above
// baseline) print for human review + merge. Deterministic; same
// exact-close standard, same tie-keep-native policy.
//
// Build:
//   g++ -std=c++17 -O2 tools/solve_pairs.cpp -Itools -Iclient/src -o \
//       /tmp/opencode/solve_pairs build/client/librbx_solve.a -lzstd -pthread
#include "replicate.h"
#include "repl_toolkit.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

using repltool::CapMsg;
using Msg = repltool::CapMsg;

// Walk quality: 2 = pure, 1 = partial, 0 = hard. Collects triple props.
int quality(const Msg& m, const rbxclient::JoinSchema& s,
            const rbxclient::PropTagTable& t, std::vector<uint16_t>& props) {
    props.clear();
    rbxclient::Walk83 w;
    std::string e;
    if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), s, &t, w,
                                  &e))
        return 0;
    for (auto& it : w.items)
        if (it.type == 0x03)
            props.push_back(it.prop03.prop);
    return (w.complete && w.end_off == m.body.size()) ? 2 : 1;
}

bool validTag(uint8_t tg) {
    return tg != 4 && tg != 5 && tg != 45 && tg != 57;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: solve_pairs <chan1.bin> [minmsgs]\n");
        return 2;
    }
    int minmsgs = argc > 2 ? std::atoi(argv[2]) : 3;
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<Msg> all = repltool::splitFrames(d);
    std::vector<uint8_t> inner97;
    {
        std::string e;
        if (!repltool::extractInner97(all, inner97, &e)) {
            std::printf("97-FAIL %s\n", e.c_str());
            return 0;
        }
    }
    std::vector<Msg> msgs;
    for (const auto& m : all)
        if (!m.body.empty() && m.body[0] == 0x83 &&
            (m.body.size() < 2 || m.body[1] != 0x16))
            msgs.push_back(m);
    rbxclient::JoinSchema s;
    {
        std::string e;
        if (!rbxclient::parseJoinSchema(inner97.data(), inner97.size(), s,
                                        &e)) {
            std::printf("SCHEMA-FAIL %s\n", e.c_str());
            return 0;
        }
    }
    rbxclient::PropTagTable base = rbxclient::defaultPropTags();
    // Solved set: props with >= 1 pure appearance under the base table
    // (their tag is trusted; pairs only trial two unsolved props).
    // Open messages + prefix props.
    std::map<uint16_t, int> open_cnt, pure_cnt;
    std::vector<std::vector<uint16_t>> open_prefix;
    std::vector<size_t> open_idx;
    std::vector<uint16_t> props;
    int pure0 = 0;
    for (size_t i = 0; i < msgs.size(); ++i) {
        int q = quality(msgs[i], s, base, props);
        if (q == 2) {
            pure0++;
            for (auto p : props)
                pure_cnt[p]++;
        } else {
            open_idx.push_back(i);
            open_prefix.push_back(props);
            for (auto p : props)
                open_cnt[p]++;
        }
    }
    std::printf("baseline pure=%d open=%zu\n", pure0, open_idx.size());
    std::set<uint16_t> unsolved;
    for (auto& kv : open_cnt)
        if (pure_cnt[kv.first] == 0)
            unsolved.insert(kv.first);
    std::printf("unsolved-only props: %zu\n", unsolved.size());
    // Co-occurring unsolved pairs in open messages.
    std::map<std::pair<uint16_t, uint16_t>, std::vector<size_t>> pairs;
    for (size_t k = 0; k < open_idx.size(); ++k) {
        std::vector<uint16_t> u;
        for (auto p : open_prefix[k])
            if (unsolved.count(p))
                u.push_back(p);
        std::sort(u.begin(), u.end());
        u.erase(std::unique(u.begin(), u.end()), u.end());
        for (size_t a = 0; a < u.size(); ++a)
            for (size_t b = a + 1; b < u.size(); ++b)
                pairs[{u[a], u[b]}].push_back(open_idx[k]);
    }
    // Keep pairs with >= minmsgs messages.
    struct PairJob {
        uint16_t p1, p2;
        std::vector<size_t> msgs;
    };
    std::vector<PairJob> jobs;
    for (auto& kv : pairs)
        if ((int)kv.second.size() >= minmsgs)
            jobs.push_back({kv.first.first, kv.first.second, kv.second});
    std::sort(jobs.begin(), jobs.end(),
              [](const PairJob& a, const PairJob& b) {
                  return a.msgs.size() > b.msgs.size();
              });
    std::printf("pair jobs (>=%d msgs): %zu\n", minmsgs, jobs.size());
    for (size_t j = 0; j < jobs.size() && j < 10; ++j)
        std::printf("  %u+%u x%zu\n", jobs[j].p1, jobs[j].p2,
                    jobs[j].msgs.size());

    // Applied winners (prop -> (tag, pfx)); conflicts skipped greedily.
    std::map<uint16_t, std::pair<uint8_t, bool>> applied;
    int round = 0;
    for (const auto& job : jobs) {
        if (applied.count(job.p1) || applied.count(job.p2))
            continue;
        // Baseline closes over job messages under current table.
        rbxclient::PropTagTable cur = base;
        for (auto& kv : applied) {
            cur.tag[kv.first] = kv.second.first;
            if (kv.second.second)
                cur.prefix[kv.first] = true;
        }
        int base_pure = 0;
        for (size_t i : job.msgs) {
            int q = quality(msgs[i], s, cur, props);
            if (q == 2)
                base_pure++;
        }
        // Single-trial ranking per prop -> top-8 shortlist.
        auto shortlist = [&](uint16_t p) {
            std::vector<std::pair<int, uint8_t>> scored;
            for (int tg = 0; tg <= 70; ++tg) {
                if (!validTag((uint8_t)tg))
                    continue;
                rbxclient::PropTagTable t2 = cur;
                t2.tag[p] = (uint8_t)tg;
                int c = 0;
                for (size_t i : job.msgs) {
                    int q = quality(msgs[i], s, t2, props);
                    if (q == 2)
                        c++;
                }
                scored.push_back({c, (uint8_t)tg});
            }
            std::sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) {
                          return a.first > b.first;
                      });
            std::vector<uint8_t> out;
            for (size_t k = 0; k < scored.size() && k < 8; ++k)
                out.push_back(scored[k].second);
            return out;
        };
        std::vector<uint8_t> s1 = shortlist(job.p1);
        std::vector<uint8_t> s2 = shortlist(job.p2);
        uint8_t w1 = cur.tag[job.p1], w2 = cur.tag[job.p2];
        bool f1 = false, f2 = false;
        int win = base_pure;
        for (uint8_t t1 : s1)
            for (uint8_t t2 : s2) {
                for (int f = 0; f < 4; ++f) {
                    bool pf1 = (f & 1) != 0, pf2 = (f & 2) != 0;
                    if (pf1 && t1 != 33 && t1 != 61)
                        continue;
                    if (pf2 && t2 != 33 && t2 != 61)
                        continue;
                    rbxclient::PropTagTable t3 = cur;
                    t3.tag[job.p1] = t1;
                    t3.tag[job.p2] = t2;
                    if (pf1)
                        t3.prefix[job.p1] = true;
                    if (pf2)
                        t3.prefix[job.p2] = true;
                    int c = 0;
                    for (size_t i : job.msgs) {
                        int q = quality(msgs[i], s, t3, props);
                        if (q == 2)
                            c++;
                    }
                    if (c > win) {
                        win = c;
                        w1 = t1;
                        w2 = t2;
                        f1 = pf1;
                        f2 = pf2;
                    }
                }
            }
        if (win >= minmsgs && win > base_pure) {
            std::printf("round %d: %u->%u%s + %u->%u%s closes %d/%zu (base %d)\n",
                        round++, job.p1, w1, f1 ? "p" : "", job.p2, w2,
                        f2 ? "p" : "", win, job.msgs.size(), base_pure);
            applied[job.p1] = {w1, f1};
            applied[job.p2] = {w2, f2};
        }
    }
    std::printf("pair winners:\n");
    for (auto& kv : applied)
        std::printf("%u %u %d\n", kv.first, kv.second.first,
                    (int)kv.second.second);
    return 0;
}
