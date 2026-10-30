// join_msg.h — RakNet-era join messages (0x90, 0x8a, 0x92, 0x8f) and the
// native message envelope ("seal") they are wrapped in.
//
// Reverse-engineered from the 2026-10-27 libroblox.so build and verified
// byte-for-byte against live native captures (tools/raknet_seal.py is the
// reference implementation; client/test/join_msg_test.cpp pins the vectors):
//
//   envelope  sub_664D9B0 (seal) / sub_664D95A (cipher init), AES-128 with a
//             backwards-chained CBC and a PCG-seeded padding/checksum header
//   0x90      sub_63D7662  prefix(9B) + network flag list + LEB(JSON) + JSON
//                           + 24-byte build-constant tail, sealed with a
//                           hardcoded key; the JSON carries the join ticket
//   0x8a      sub_63D7EB2  ticket submit: zigzag(UserId), ticket, app id, the
//                           74-byte build constant, "Android", "?", the ticket
//                           hash, the session JSON, 0xC001CAFE — sealed with
//                           a key derived from placeId + two build constants
//   0x92      sub_63D8F36  [0x92] + LEB(2 * (rand<<32 | rand^0x63E25F26)^sign)
//                           (its handler also sends 0x8a and 0x8f)
//   0x8f      sub_63D9E42  [0x8f] + string(server id)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rbxmsg {

// --- envelope --------------------------------------------------------------

// Hardcoded 0x90 envelope key (native: xmmword_77050B0, filled from the
// binary constant at 0x23DDE0) and its captured PRNG state.
extern const uint8_t kKey90[16];
extern const uint64_t kState90;

// 0x8a key: ("%d" placeId + "-1733811612" + tail[2] + "594292182")[:16] —
// tail[2] is a build constant read from the 0x90 template (551761825 here).
std::string key8a(uint32_t place_id, uint32_t tail2);

// PRNG state the native's cipher init produced for the captured 0x8a key. The
// state only feeds the envelope's padding bytes, so any state yields a valid
// message; this one makes the captured key byte-identical to the native.
extern const uint64_t kState8a;

// Seal `len` bytes of plaintext with AES-128 (key, state0) -> body whose size
// is len + pad + 6 (always a 16-byte multiple). Deterministic for (key, len).
std::vector<uint8_t> seal(const uint8_t* plain, size_t len, const uint8_t key[16],
                          uint64_t state0);

// Inverse of seal() — used by the self-tests and for decoding captures.
std::vector<uint8_t> unseal(const uint8_t* body, size_t len, const uint8_t key[16],
                            size_t* pad_out);

// One sealed message: [id][seal(body)].
std::vector<uint8_t> sealedMessage(uint8_t id, const std::vector<uint8_t>& body,
                                   const uint8_t key[16], uint64_t state0);

// --- builders --------------------------------------------------------------

// LEB128 (continuation-bit varint) and the two writers the native uses:
// putLeb = sub_3917603, putString = sub_3917851 (LEB length + bytes).
std::string leb128(uint64_t v);
void putLeb(std::vector<uint8_t>* out, uint64_t v);
void putString(std::vector<uint8_t>* out, const std::string& s);

// The 0x90 join body. Templates are real native payloads (the join data JSON
// plus the build-constant flag list and tail); every JSON field that also
// exists in the join script is replaced with this session's value, and
// __joinTicket is rebuilt from the join reply.
struct Join90 {
  std::vector<uint8_t> body;     // sealed payload (put through seal())
  std::string json;              // the JSON that went in (for logs/tests)
};
bool build90(const std::vector<uint8_t>& tmpl, const nlohmann::json& join_script,
             const nlohmann::json& reply, Join90* out, std::string* err);

// The 0x8a ticket body: the ticket blob plus the session JSON (also merged
// from the join script against the template's own session document).
bool build8a(uint32_t user_id, const std::string& client_ticket,
             const std::vector<uint8_t>& session_template,
             const nlohmann::json& join_script, std::vector<uint8_t>* body,
             std::string* err);

// Read a captured 0x8a payload (raknet8a_template.bin) back into its fields;
// any out pointer may be null.
bool parse8aPayload(const uint8_t* p, size_t n, uint32_t* user_id,
                    std::string* ticket, std::string* session_json, std::string* err);

// [0x92] + LEB(2 * ((rand<<32) + (rand ^ v9)) ^ sign) — the native's
// sub_63D8F36; rand is a fresh PCG u32 (sub_1E5D507) and v9 is sub_664CCD4()
// = tail[3] ^ (uint16(tail[2]) | 0x22640000), i.e. it also comes out of the
// 0x90 template's build-constant tail.
std::vector<uint8_t> build92(uint32_t rand32, uint32_t v9);

// [0x8f] + string(server id) — sub_63D9E42.
std::vector<uint8_t> build8f(const std::string& server_id);

// The 0x83/0x04 player-attributes push the native sends right after the join:
//   [83][04][u32 flags][07][01][u32 const][u16 const][u40 nonce]
//   [LEB json][playerInfo JSON][base64 signature]
// Layout from the live 1818 captures (king: flags 100, JSON 209 B; mizg:
// flags 136, JSON 211 B — the middle constants and the nonce slot are shared
// across both). The JSON is the hydration service's `playerInfo` document and
// the signature is the service's, both fetched fresh per session.
std::vector<uint8_t> buildHydration(const std::string& player_info_json,
                                    const std::string& signature);

// The Unreal-style check hash the 0x8a carries (port of sub_301CBE2 +
// the ticket hash chain; constants pinned against the live capture). The
// underlying hash is rbxclient::xxh32 (common/xxh32.h), shared with QUIC.
uint32_t ticketHash(const std::string& ticket);

// Join-script helpers: read a string field percent-decoded, and merge a
// template JSON with this session's join script (template order preserved).
std::string jsonString(const nlohmann::json& js, const char* key);
bool mergeSessionJson(const std::string& tmpl_text, const nlohmann::json& join_script,
                      std::string* out, std::string* err);

} // namespace rbxmsg
