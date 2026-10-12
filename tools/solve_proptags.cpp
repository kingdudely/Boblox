// solve_proptags.cpp — exact-close solver for 0x03 prop wire shapes.
//
// Usage: solve_proptags <chan1.bin> [topN=15]
//
// Method (same as the Crossroads v3 solve, now reusable per game):
// every live 0x83 chain must walk exact-close. Messages that stop or
// hard-fail name a culprit prop (most frequent triple prop in open
// messages, discounted by pure-message appearances). Each culprit is
// tried against all value tags (native# first; ties keep the current
// table); the tag maximizing pure-closes over messages containing the
// prop wins. Winners apply to the table and the loop repeats to a
// fixpoint. Output is a candidate overrides map for human review —
// NEVER applied blindly (extent ties like the 5469 u16-vs-string class
// need a judgment call; see FINDINGS "Recv v3").
//
// Build:
//   g++ -std=c++17 -O2 tools/solve_proptags.cpp -Itools -Iclient/src -o \
//       /tmp/opencode/solve_proptags build/client/librbx_solve.a -lzstd -pthread
#include "replicate.h"
#include "repl_toolkit.h"

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

// Walk quality of one message under table t: 2 = pure, 1 = partial,
// 0 = hard fail. Collects triple props from the parsed prefix.
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: solve_proptags <chan1.bin> [topN]\n");
        return 2;
    }
    int topN = argc > 2 ? std::atoi(argv[2]) : 15;
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<Msg> all = repltool::splitFrames(d);
    std::vector<uint8_t> inner97;
    for (auto& m : all)
        if (!m.body.empty() && m.body[0] == 0x97) {
            std::string e;
            if (!rbxclient::parseJoinOuter(m.body.data(), m.body.size(),
                                           inner97, &e)) {
                std::printf("97-OUTER-FAIL %s\n", e.c_str());
                return 0;
            }
            break;
        }
    std::vector<Msg> msgs;
    for (auto& m : all)
        if (!m.body.empty() && m.body[0] == 0x83 &&
            (m.body.size() < 2 || m.body[1] != 0x16))
            msgs.push_back(std::move(m));
    rbxclient::JoinSchema s;
    {
        std::string e;
        if (!rbxclient::parseJoinSchema(inner97.data(), inner97.size(), s,
                                        &e)) {
            std::printf("SCHEMA-FAIL %s\n", e.c_str());
            return 0;
        }
    }
    rbxclient::PropTagTable t = rbxclient::defaultPropTags();
    // prop -> (tag, prefix, win_pure). Human merges winners into the
    // overrides TSV (see merge policy in FINDINGS "Recv v4").
    std::map<uint16_t, std::tuple<uint8_t, bool, int>> solved;
    std::set<uint16_t> banned; // no-improvement props (derail victims)

    for (int round = 0; round < topN; ++round) {
        // Partition under the current table.
        std::map<uint16_t, int> open_cnt, pure_cnt;
        std::vector<size_t> open_idx;
        std::vector<uint16_t> props;
        int pure = 0, part = 0, hard = 0;
        for (size_t i = 0; i < msgs.size(); ++i) {
            int q = quality(msgs[i], s, t, props);
            if (q == 2) {
                pure++;
                for (auto p : props)
                    pure_cnt[p]++;
            } else {
                (q == 1 ? part : hard)++;
                open_idx.push_back(i);
                for (auto p : props)
                    open_cnt[p]++;
            }
        }
        std::printf("round %d: pure=%d part=%d hard=%d\n", round, pure, part,
                    hard);
        if (open_idx.empty())
            break;
        // Culprit = max (open - pure) among open props, skipping banned
        // victims (props that appear in derailed prefixes without causing
        // them — e.g. Crossroads-10145 in Brookhaven).
        uint16_t culprit = 0;
        int best_score = 0;
        for (auto& kv : open_cnt) {
            if (banned.count(kv.first))
                continue;
            int score = kv.second - pure_cnt[kv.first];
            if (score > best_score) {
                best_score = score;
                culprit = kv.first;
            }
        }
        if (!best_score) {
            std::printf("no scored culprit (derails lack triple prefixes?)\n");
            break;
        }
        // Messages containing the culprit (by current-table prefix parse,
        // plus any open message — cheap over-approximation is fine).
        std::vector<size_t> cand;
        for (size_t i : open_idx) {
            int q = quality(msgs[i], s, t, props);
            bool has = false;
            for (auto p : props)
                if (p == culprit) {
                    has = true;
                    break;
                }
            if (has)
                cand.push_back(i);
        }
        // Fallback: if the culprit never parses (derail precedes it), use
        // all open messages for trials.
        if (cand.empty()) {
            cand = open_idx;
            std::printf("culprit %u unparsed; trialing over all %zu open\n",
                        culprit, cand.size());
        }
        uint8_t cur_tag = t.tag.count(culprit) ? t.tag[culprit] : 255;
        int cur_pure = 0;
        for (size_t i : cand) {
            int q = quality(msgs[i], s, t, props);
            if (q == 2)
                cur_pure++;
        }
        // Trial order: current tag first (ties keep it), then all others.
        std::vector<uint8_t> order;
        if (cur_tag != 255)
            order.push_back(cur_tag);
        for (int tg = 0; tg <= 70; ++tg) {
            if (tg == 4 || tg == 5 || tg == 45 || tg == 57)
                continue; // no native case / never server->client
            if ((uint8_t)tg != cur_tag)
                order.push_back((uint8_t)tg);
        }
        uint8_t win_tag = cur_tag;
        bool win_pfx = t.prefix.count(culprit) ? true : false;
        int win_pure = cur_pure;
        for (uint8_t tg : order) {
            for (int pfx_try = 0; pfx_try <= 1; ++pfx_try) {
                if (pfx_try == 1 && tg != 33 && tg != 61)
                    continue; // prefix only shapes 33/61
                rbxclient::PropTagTable t2 = t;
                t2.tag[culprit] = tg;
                if (pfx_try)
                    t2.prefix[culprit] = true;
                else
                    t2.prefix.erase(culprit);
                int p = 0;
                for (size_t i : cand) {
                    int q = quality(msgs[i], s, t2, props);
                    if (q == 2)
                        p++;
                }
                if (p > win_pure) {
                    win_pure = p;
                    win_tag = tg;
                    win_pfx = pfx_try != 0;
                }
            }
        }
        std::printf("culprit %u (open=%d pure=%d): cur tag=%d pure=%d -> win tag=%d pfx=%d pure=%d\n",
                    culprit, open_cnt[culprit], pure_cnt[culprit], cur_tag,
                    cur_pure, win_tag, (int)win_pfx, win_pure);
        if (win_pure == cur_pure) {
            std::printf("no improvement for %u; banning, continuing\n",
                        culprit);
            banned.insert(culprit);
            continue;
        }
        if (solved.count(culprit)) {
            // Oscillation: this prop won before, yet recurs as culprit —
            // its messages need another prop's shape (or a new grammar),
            // not a flip. Keep the higher-pure endpoint, ban it.
            uint8_t prev_tag = std::get<0>(solved[culprit]);
            bool prev_pfx = std::get<1>(solved[culprit]);
            int prev_pure = std::get<2>(solved[culprit]);
            if (win_pure > prev_pure) {
                t.tag[culprit] = win_tag;
                if (win_pfx)
                    t.prefix[culprit] = true;
                else
                    t.prefix.erase(culprit);
                solved[culprit] = {win_tag, win_pfx, win_pure};
                std::printf("culprit %u recurs; upgrading %u@%d -> %u@%d, banning\n",
                            culprit, prev_tag, prev_pure, win_tag, win_pure);
            } else {
                t.tag[culprit] = prev_tag;
                if (prev_pfx)
                    t.prefix[culprit] = true;
                else
                    t.prefix.erase(culprit);
                std::printf("culprit %u recurs; keeping %u@%d over %u@%d, banning\n",
                            culprit, prev_tag, prev_pure, win_tag, win_pure);
            }
            (void)prev_pfx;
            banned.insert(culprit);
            continue;
        }
        t.tag[culprit] = win_tag;
        if (win_pfx)
            t.prefix[culprit] = true;
        else
            t.prefix.erase(culprit);
        solved[culprit] = {win_tag, win_pfx, win_pure};
    }
    std::printf("solved map (review before merging into overrides):\n");
    for (auto& kv : solved)
        std::printf("%u %u %d @%d\n", kv.first, std::get<0>(kv.second),
                    (int)std::get<1>(kv.second), std::get<2>(kv.second));
    return 0;
}
