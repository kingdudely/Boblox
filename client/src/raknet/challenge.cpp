// challenge.cpp — see challenge.h.
#include "raknet/challenge.h"

#include "common/util.h"   // rbx::le32
#include "raknet/raknet.h" // RaknetClient::sendJoinMessage
#include "solve/solve.h"   // rbxclient::solveMessage / buildResponse

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace rbxclient {
namespace {

// A *value* return, not a static buffer: hexU32(u1) and hexU32(u2) appear in
// one expression, and with a shared static buffer both format calls can leave
// the same text in it (observed: u2 logged as u1).
std::string hexU32(uint32_t v) {
  char b[9];
  snprintf(b, sizeof b, "%08x", v);
  return std::string(b);
}

} // namespace

bool isChallenge(const uint8_t* p, size_t n) {
  if (!p || n < 13 || p[0] != 0x9B) return false;
  const uint32_t blob = rbx::le32(p + 9);
  return size_t(blob) <= n - 13;
}

bool answerChallenge(RaknetClient& client, const uint8_t* p, size_t n,
                     const std::string& job_id,
                     const std::function<void(const std::string&)>& log) {
  if (!isChallenge(p, n)) return false;
  const auto say = log ? log : [](const std::string& m) { printf("%s\n", m.c_str()); };

  const uint32_t u1 = rbx::le32(p + 1);
  const uint32_t u2 = rbx::le32(p + 5);
  const uint32_t blob = rbx::le32(p + 9);
  say(std::string("raknet: 0x9B CHALLENGE u1=0x") + hexU32(u1) + " u2=0x" +
      hexU32(u2) + " blob=" + std::to_string(blob) + "B");

  if (const char* dump = getenv("RBX_RAKNET_DUMP")) {
    const std::string path = std::string(dump) + "/challenge_9b.bin";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(p, 1, n, f);
      fclose(f);
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  uint32_t answer = 0;
  std::string err;
  if (!solveMessage(p, n, job_id, answer, &err)) {
    say("raknet: 0x9B solve FAILED: " + err);
    return false;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();

  const std::vector<uint8_t> resp = buildResponse(u2, answer);
  const int n_dgram = client.sendJoinMessage(resp.data(), resp.size());
  say(std::string("raknet: 0x9B answer 0x") + hexU32(answer) + " (solved in " +
      std::to_string(ms) + "ms) sent in " + std::to_string(n_dgram) + " datagram");
  return n_dgram > 0;
}

} // namespace rbxclient
