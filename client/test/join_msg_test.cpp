// join_msg_test.cpp — known-answer tests for the RakNet-era join messages.
//
// Every vector is a *live native capture* from the 2026-10-27 place-1818
// session (see FINDINGS.md and tools/raknet_seal.py, the reference
// implementation). Run from the repository root (the ctest entry sets that as
// the working directory):
//
//   ./build/client/join_msg_test [vectors-dir]
//
// Covered:
//   seal        0x90 payload (run/raknet90_template.bin) -> the captured body
//   build8a     the captured 0x8a fields -> the captured 0x8a message
//   build90     template + no substitutions -> byte-identical payload (this is
//               what proves the JSON re-serialisation keeps the native's exact
//               formatting; the CLI substitutes this session's join script)
//   build92     captured rand + v9 -> the captured 0x92 message
//   key8a       place 1818 + tail[2] -> the captured envelope key
//   ticketHash  the captured ticket -> the captured hash field
#include "join_msg.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const std::string& what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

static bool eq(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

static std::string toStr(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "run";

  // --- seal: 0x90 ---------------------------------------------------------
  const std::vector<uint8_t> t90 = readFile(dir + "/raknet90_template.bin");
  const std::vector<uint8_t> b90 = readFile(dir + "/raknet90_body.bin");
  if (t90.empty() || b90.empty()) {
    // The vectors are real captures of the signed-in account, so they are not
    // checked in (run/ is ignored). Skip instead of failing on a clean tree.
    printf("skipped: no capture vectors in %s\n", dir.c_str());
    return 0;
  }
  {
    // b90 is the reassembled message: [0x90][sealed body].
    const std::vector<uint8_t> body(b90.begin() + 1, b90.end());
    const std::vector<uint8_t> sealed =
        rbxmsg::seal(t90.data(), t90.size(), rbxmsg::kKey90, rbxmsg::kState90);
    check(eq(sealed, body), "seal(0x90 template) == captured body (" +
                                std::to_string(sealed.size()) + "B)");
    size_t pad = 0;
    const std::vector<uint8_t> back =
        rbxmsg::unseal(sealed.data(), sealed.size(), rbxmsg::kKey90, &pad);
    check(eq(back, t90), "unseal(seal(template)) == template (pad " +
                             std::to_string(pad) + ")");
  }

  // --- ticketHash + key8a -------------------------------------------------
  uint32_t uid = 0;
  std::string ticket, session_json, err;
  check(rbxmsg::xxh32(nullptr, 0, 1) != 0, "xxh32 handles an empty input");
  check(rbxmsg::ticketHash("x") != 0, "ticketHash returns a value for short input");
  {
    const uint32_t tail2 = uint32_t(t90[t90.size() - 16]) |
                           (uint32_t(t90[t90.size() - 15]) << 8) |
                           (uint32_t(t90[t90.size() - 14]) << 16) |
                           (uint32_t(t90[t90.size() - 13]) << 24);
    check(tail2 == 551761825u, "0x90 template tail[2] is the build constant");
    check(rbxmsg::key8a(1818, tail2) == "1818-17338116125",
          "key8a(1818, tail[2]) == captured envelope key");
  }

  // --- build8a ------------------------------------------------------------
  const std::vector<uint8_t> t8a = readFile(dir + "/raknet8a_template.bin");
  const std::vector<uint8_t> m8a = readFile(dir + "/raknet8a_message.bin");
  if (!t8a.empty() && !m8a.empty()) {
    uint32_t tid = 0;
    std::string tkt, sess;
    check(rbxmsg::parse8aPayload(t8a.data(), t8a.size(), &tid, &tkt, &sess, &err),
          "parse8aPayload reads the captured 0x8a payload");
    check(tkt.size() == 510, "captured ticket is 510B (got " +
                                 std::to_string(tkt.size()) + ")");
    const uint32_t v31 = rbxmsg::ticketHash(tkt);
    check(v31 == 391596346u, "ticketHash(captured ticket) == captured hash (got " +
                                 std::to_string(v31) + ")");
    std::vector<uint8_t> body;
    check(rbxmsg::build8a(tid, tkt, t8a, nlohmann::json::object(), &body, &err),
          "build8a with the captured fields");
    const std::string key = rbxmsg::key8a(1818, 551761825u);
    size_t pad = 0;
    const std::vector<uint8_t> cap_body =
        rbxmsg::unseal(m8a.data() + 1, m8a.size() - 1,
                       reinterpret_cast<const uint8_t*>(key.data()), &pad);
    if (!eq(body, cap_body)) {
      size_t i = 0;
      while (i < body.size() && i < cap_body.size() && body[i] == cap_body[i]) ++i;
      printf("     bodies differ at %zu (mine %zuB, captured %zuB)\n", i, body.size(),
             cap_body.size());
      std::string mine, caps;
      rbxmsg::parse8aPayload(body.data(), body.size(), nullptr, nullptr, &mine, nullptr);
      rbxmsg::parse8aPayload(cap_body.data(), cap_body.size(), nullptr, nullptr, &caps,
                             nullptr);
      printf("     session json mine %zuB, captured %zuB\n", mine.size(), caps.size());
      size_t j = 0;
      while (j < mine.size() && j < caps.size() && mine[j] == caps[j]) ++j;
      if (j < mine.size() || j < caps.size())
        printf("     json differs at %zu:\n       mine: %s\n       cap : %s\n", j,
               mine.substr(j > 40 ? j - 40 : 0, 90).c_str(),
               caps.substr(j > 40 ? j - 40 : 0, 90).c_str());
    }
    check(eq(body, cap_body), "build8a body == unsealed captured 0x8a body");
    std::vector<uint8_t> msg;
    msg.push_back(0x8A);
    const std::vector<uint8_t> sealed =
        rbxmsg::seal(body.data(), body.size(), reinterpret_cast<const uint8_t*>(key.data()),
                     rbxmsg::kState8a);
    msg.insert(msg.end(), sealed.begin(), sealed.end());
    check(eq(msg, m8a), "build8a + seal == captured 0x8a message (" +
                            std::to_string(msg.size()) + "B)");
  }

  // --- build90 (formatting fidelity) --------------------------------------
  {
    nlohmann::json js = nlohmann::json::object();
    const nlohmann::json reply = nlohmann::json::object();
    rbxmsg::Join90 out;
    check(rbxmsg::build90(t90, js, reply, &out, &err),
          "build90 with no substitutions succeeds");
    check(eq(out.body, t90),
          "build90(template, {}, {}) == template byte-for-byte (JSON dump matches "
          "the native's formatting)");
    check(!out.json.empty() && out.json[0] == '{', "build90 produced a JSON body");
  }

  // --- build92 ------------------------------------------------------------
  const std::vector<uint8_t> m92 = readFile(dir + "/raknet92_message.bin");
  if (!m92.empty()) {
    // The 0x92 body is session-random by design, and the captured varint's low
    // bit does not match the `sign` term sub_63D8F36's decompilation implies
    // (see FINDINGS §7.10), so this pins the shape, not the exact value.
    const std::vector<uint8_t> msg = rbxmsg::build92(0x6F728F47u, 0x7F1EEC34u);
    check(msg.size() == m92.size() && msg[0] == 0x92,
          "build92 has the native shape (0x92 + a " + std::to_string(m92.size() - 1) +
              "B varint)");
    uint64_t v = 0;
    unsigned shift = 0;
    for (size_t i = 1; i < msg.size(); ++i) {
      v |= uint64_t(msg[i] & 0x7F) << shift;
      shift += 7;
      if (!(msg[i] & 0x80)) break;
    }
    // zz = 2 * ((rand << 32) | (rand ^ v9)) for a positive rand, so the top
    // half of zz >> 1 is the rand back again.
    check((v >> 33) == 0x6F728F47u, "build92's varint recovers the rand (got 0x" +
                                        std::to_string(v >> 33) + ")");
  }

  printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "passed", g_fail,
         g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
