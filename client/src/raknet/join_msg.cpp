// join_msg.cpp — see join_msg.h. Reference implementation + vectors live in
// tools/raknet_seal.py; every byte-level rule here was checked against the
// 2026-10-27 live native captures (place 1818).
#include "raknet/join_msg.h"

#include "common/xxh32.h"  // rbxclient::xxh32 (shared with the QUIC side)

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rbxmsg {
namespace {

constexpr uint64_t kMul = 0x685F98A2018FADE9ull; // PCG update constants from
constexpr uint64_t kAdd = 0x399D2694695129DEull; // sub_664D9B0
constexpr uint32_t kMix1 = 0xFFFFCE6D;           // -12691 in the checksum fold
constexpr uint32_t kChecksumSeed = 0xD971;       // "mov dx, 0D971h"
constexpr uint32_t kHashConst = 0x71375635;      // S (dword_71941A0), build-wide
constexpr uint32_t kTicketConst = 17498;         // 0x445A; 17506 in the QUIC build
constexpr uint32_t kIterConst = 1434170839;      // 0x5578B7D7
constexpr uint32_t kBadF00D = 0x0BADF00D;
// v9 (the 0x92's other input) is derived from the template tail in build92().

// The 0x8a build constant (74 bytes): 40 hex chars + '!' + 32 hex + '.'. The
// RakNet-era second half differs from the QUIC-era capture.
const char kConst74[] =
    "2e427f51c4dab762fe9e3471c6cfa1650841723b!"
    "64d6a124222d374da86ce360982320f0.";

uint64_t load64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

void store32le(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

uint32_t rol32(uint32_t x, unsigned r) {
  r &= 31;
  return (x << r) | (x >> ((32 - r) & 31));
}

// PCG-style output byte: sub_664D9B0 reads
//   ror32((s >> 27) ^ (s >> 45), (s >> 59) & 31) & 0xFF
uint8_t randByte(uint64_t s) {
  const uint32_t v = uint32_t((s >> 27) ^ (s >> 45));
  const unsigned rot = unsigned((s >> 59) & 31);
  return uint8_t((v >> rot) | (v << ((32 - rot) & 31)));
}

// AES-128 single-block ECB through libcrypto (same primitive the native uses
// via its own T-tables; the key schedule is not otherwise involved).
class Aes128 {
  public:
    explicit Aes128(const uint8_t key[16]) {
      ctx_ = EVP_CIPHER_CTX_new();
      EVP_EncryptInit_ex(ctx_, EVP_aes_128_ecb(), nullptr, key, nullptr);
      EVP_CIPHER_CTX_set_padding(ctx_, 0);
    }
    ~Aes128() { EVP_CIPHER_CTX_free(ctx_); }
    void block(const uint8_t in[16], uint8_t out[16]) {
      int n = 0;
      EVP_EncryptUpdate(ctx_, out, &n, in, 16);
    }
  private:
    EVP_CIPHER_CTX* ctx_ = nullptr;
};

class Aes128Dec {
  public:
    explicit Aes128Dec(const uint8_t key[16]) {
      ctx_ = EVP_CIPHER_CTX_new();
      EVP_DecryptInit_ex(ctx_, EVP_aes_128_ecb(), nullptr, key, nullptr);
      EVP_CIPHER_CTX_set_padding(ctx_, 0);
    }
    ~Aes128Dec() { EVP_CIPHER_CTX_free(ctx_); }
    void block(const uint8_t in[16], uint8_t out[16]) {
      int n = 0;
      EVP_DecryptUpdate(ctx_, out, &n, in, 16);
    }
  private:
    EVP_CIPHER_CTX* ctx_ = nullptr;
};

std::string hexToStr(const nlohmann::json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) {
    char b[64];
    snprintf(b, sizeof b, "%.17g", v.get<double>());
    return b;
  }
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  return v.dump();
}

// Copy every field the join script also has (recursively, template order and
// template value *kinds* preserved: a string field stays a string).
void mergeInto(nlohmann::ordered_json& dst, const nlohmann::json& src) {
  if (!dst.is_object() || !src.is_object()) return;
  for (auto it = dst.begin(); it != dst.end(); ++it) {
    if (!src.contains(it.key())) continue;
    const nlohmann::json& sv = src[it.key()];
    if (it.value().is_object() && sv.is_object()) {
      mergeInto(it.value(), sv);
    } else if (it.value().is_string()) {
      it.value() = hexToStr(sv);
    } else if (it.value().is_number() && sv.is_number()) {
      it.value() = sv;
    } else if (it.value().is_boolean() && sv.is_boolean()) {
      it.value() = sv;
    } else if (it.value().is_null()) {
      it.value() = sv;
    }
  }
}

// Locate `{"UserId"` and the LEB length in front of it (the 0x90 layout).
bool split90(const std::vector<uint8_t>& tmpl, size_t* json_off, size_t* leb_off,
             std::string* err) {
  static const char kProbe[] = "{\"UserId\"";
  const auto it = std::search(tmpl.begin(), tmpl.end(), kProbe, kProbe + sizeof kProbe - 1);
  if (it == tmpl.end()) {
    if (err) *err = "0x90 template has no {\"UserId\" JSON";
    return false;
  }
  size_t i = size_t(it - tmpl.begin());
  size_t start = i ? i - 1 : 0;
  while (start > 0 && (tmpl[start - 1] & 0x80)) --start;
  uint64_t len = 0;
  unsigned shift = 0;
  for (size_t k = start; k < i; ++k) {
    len |= uint64_t(tmpl[k] & 0x7F) << shift;
    shift += 7;
  }
  if (tmpl.size() < i + len + 24) {
    if (err) *err = "0x90 template is too short for its own length";
    return false;
  }
  *json_off = i;
  *leb_off = start;
  return true;
}

} // namespace

// Walk the 0x8a payload fields in native order (sub_63D7EB2):
//   leb zigzag(UserId) | str ClientTicket | u32 36 | str const74 | str Android
//   | str ? | leb v31 | leb v31-0x0BADF00D | str "" | str SessionId | u32 0xC001CAFE
bool parse8aPayload(const uint8_t* p, size_t n, uint32_t* user_id, std::string* ticket,
                    std::string* session_json, std::string* err) {
  size_t i = 0;
  auto leb = [&](uint64_t* v) {
    *v = 0;
    unsigned shift = 0;
    while (i < n) {
      const uint8_t b = p[i++];
      *v |= uint64_t(b & 0x7F) << shift;
      shift += 7;
      if (!(b & 0x80)) return true;
    }
    return false;
  };
  auto str = [&](std::string* s) {
    uint64_t len = 0;
    if (!leb(&len) || i + len > n) return false;
    s->assign(reinterpret_cast<const char*>(p + i), size_t(len));
    i += size_t(len);
    return true;
  };
  uint64_t zz = 0, app = 0, v31 = 0, v31b = 0;
  std::string tkt, c74, android, q, empty, session;
  if (!leb(&zz) || !str(&tkt)) goto bad;
  if (i + 4 > n) goto bad;
  app = uint32_t(p[i]) | (uint32_t(p[i + 1]) << 8) | (uint32_t(p[i + 2]) << 16) |
        (uint32_t(p[i + 3]) << 24);
  i += 4;
  if (!str(&c74) || !str(&android) || !str(&q)) goto bad;
  if (!leb(&v31) || !leb(&v31b)) goto bad; // the two ticket hashes
  if (!str(&empty) || !str(&session)) goto bad;
  if (user_id) *user_id = uint32_t((zz >> 1) ^ uint64_t(-int64_t(zz & 1)));
  if (ticket) *ticket = tkt;
  if (session_json) *session_json = session;
  return true;
bad:
  if (err) *err = "0x8a template payload does not parse";
  return false;
}

const uint8_t kKey90[16] = {0xfe, 0xf9, 0xf0, 0xeb, 0xe2, 0xdd, 0xd4, 0xcf,
                            0xc6, 0xc1, 0xb8, 0xb3, 0xaa, 0xa5, 0x9c, 0x97};
const uint64_t kState90 = 0xE7DD5C9C5148F144ull;
const uint64_t kState8a = 0xC9AAA1F29BCF8572ull;

std::string key8a(uint32_t place_id, uint32_t tail2) {
  // "%d%d%d%d" % (placeId, -1733811612, tail2, 594292182), truncated to 16.
  std::string s = std::to_string(place_id) + "-1733811612" + std::to_string(tail2) +
                  "594292182";
  return s.substr(0, std::min<size_t>(16, s.size()));
}

std::string leb128(uint64_t v) {
  std::string out;
  for (;;) {
    const uint8_t b = uint8_t(v & 0x7F);
    v >>= 7;
    if (v) {
      out.push_back(char(b | 0x80));
    } else {
      out.push_back(char(b));
      break;
    }
  }
  return out;
}

void putString(std::vector<uint8_t>* out, const std::string& s) {
  putLeb(out, s.size());
  out->insert(out->end(), s.begin(), s.end());
}

void putLeb(std::vector<uint8_t>* out, uint64_t v) {
  const std::string l = leb128(v);
  out->insert(out->end(), l.begin(), l.end());
}

std::vector<uint8_t> seal(const uint8_t* plain, size_t len, const uint8_t key[16],
                          uint64_t state0) {
  const size_t pad = (~(len + 5)) & 0x0F;
  const size_t outlen = len + pad + 6;
  std::vector<uint8_t> out(outlen, 0);
  const uint8_t r0 = randByte(state0);
  const uint64_t s1 = kMul * state0 + kAdd;
  uint64_t state = kMul * s1 + kAdd;
  out[4] = r0;
  out[5] = uint8_t(pad + 16 * (randByte(s1) & 0x0F));
  std::memcpy(out.data() + pad + 6, plain, len);
  for (size_t i = 0; i < pad; ++i) {
    out[6 + i] = randByte(state);
    state = kMul * state + kAdd;
  }
  uint32_t h = kChecksumSeed;
  uint32_t acc = 0;
  for (size_t i = 4; i < outlen; ++i) {
    const uint32_t v = uint32_t(out[i] ^ uint8_t(h >> 8));
    h = uint32_t(kMix1 * (h + (v & 0xFF)) + 22719); // imul edx,-12691; add 58BFh
    acc += v & 0xFF;
  }
  store32le(out.data(), acc);

  Aes128 aes(key);
  const size_t nblk = outlen / 16;
  uint8_t chain[16];
  aes.block(out.data(), chain);
  std::memcpy(out.data(), chain, 16);
  for (size_t i = nblk - 1; i > 0; --i) {
    uint8_t blk[16];
    for (int j = 0; j < 16; ++j) blk[j] = uint8_t(out[16 * i + j] ^ chain[j]);
    aes.block(blk, chain);
    std::memcpy(out.data() + 16 * i, chain, 16);
  }
  return out;
}

std::vector<uint8_t> unseal(const uint8_t* body, size_t len, const uint8_t key[16],
                            size_t* pad_out) {
  if (len == 0 || len % 16) return {};
  const size_t nblk = len / 16;
  std::vector<uint8_t> out(body, body + len);
  Aes128Dec dec(key);
  uint8_t chain[16];
  std::memcpy(chain, body, 16);
  for (size_t i = nblk - 1; i > 0; --i) {
    uint8_t blk[16];
    dec.block(body + 16 * i, blk);
    for (int j = 0; j < 16; ++j) out[16 * i + j] = uint8_t(blk[j] ^ chain[j]);
    std::memcpy(chain, body + 16 * i, 16);
  }
  uint8_t blk0[16];
  dec.block(body, blk0);
  std::memcpy(out.data(), blk0, 16);
  const size_t pad = out[5] & 0x0F;
  if (pad_out) *pad_out = pad;
  if (pad + 6 > out.size()) return {};
  return std::vector<uint8_t>(out.begin() + pad + 6, out.end());
}

std::vector<uint8_t> sealedMessage(uint8_t id, const std::vector<uint8_t>& body,
                                   const uint8_t key[16], uint64_t state0) {
  std::vector<uint8_t> sealed = seal(body.data(), body.size(), key, state0);
  std::vector<uint8_t> msg;
  msg.reserve(sealed.size() + 1);
  msg.push_back(id);
  msg.insert(msg.end(), sealed.begin(), sealed.end());
  return msg;
}

std::string jsonString(const nlohmann::json& js, const char* key) {
  if (!js.contains(key)) return {};
  if (js[key].is_string()) return js[key].get<std::string>();
  return hexToStr(js[key]);
}

bool mergeSessionJson(const std::string& tmpl_text, const nlohmann::json& join_script,
                      std::string* out, std::string* err) {
  nlohmann::ordered_json doc = nlohmann::ordered_json::parse(tmpl_text, nullptr, false);
  if (doc.is_discarded()) {
    if (err) *err = "session JSON template does not parse";
    return false;
  }
  mergeInto(doc, join_script);
  *out = doc.dump();
  return true;
}

bool build90(const std::vector<uint8_t>& tmpl, const nlohmann::json& join_script,
             const nlohmann::json& reply, Join90* out, std::string* err) {
  size_t json_off = 0, leb_off = 0;
  if (!split90(tmpl, &json_off, &leb_off, err)) return false;
  const size_t tail_off = tmpl.size() - 24;
  if (json_off > tail_off) {
    if (err) *err = "0x90 template JSON overlaps its tail";
    return false;
  }
  const std::string tmpl_json(reinterpret_cast<const char*>(tmpl.data() + json_off),
                              tail_off - json_off);
  nlohmann::ordered_json doc = nlohmann::ordered_json::parse(tmpl_json, nullptr, false);
  if (doc.is_discarded()) {
    if (err) *err = "0x90 template JSON does not parse";
    return false;
  }
  mergeInto(doc, join_script);
  // The native's 0x90 carries 47 keys and has no AgeBracket / Gender /
  // IsOriginalUser / OriginalAccountCreationTimestampMs / Os / Platform block
  // (unsealed from its own 0x90 in /tmp/nat_cmp). Those six belong to the
  // separate 0x83/0x04 hydration push, and their join-document form came from
  // a stale template capture, so they are pruned by default now:
  // with them the RCC's 0x93 lands in the "production" bucket, without them
  // the document matches the native key for key. RBX_HYDRATION_FIELDS=1 puts
  // them back for A/B runs.
  const char* hyd_keep = getenv("RBX_HYDRATION_FIELDS");
  if (!(hyd_keep && *hyd_keep && *hyd_keep != '0')) {
    for (const char* k : {"AgeBracket", "Gender", "IsOriginalUser",
                          "OriginalAccountCreationTimestampMs", "Os", "Platform"})
      doc.erase(k);
  }
  // Identity fields the join script does not carry stay on the *template's*
  // values, which belong to whoever that capture was taken as. The native's
  // live body (dumped at sub_664D9B0, 2026-10-29) has CharacterAppearanceId ==
  // UserId for this account, while the template shipped someone else's id —
  // an appearance id that is not the joining user is the kind of mismatch a
  // server can reject the whole join for. Pin it to the join script's UserId.
  if (join_script.contains("UserId") && !join_script.contains("CharacterAppearanceId"))
    doc["CharacterAppearanceId"] = join_script["UserId"];
  // NB: do not prune fields here. The native's live join body carries 53 keys,
  // including AgeBracket / Gender / Os / Platform / IsOriginalUser /
  // OriginalAccountCreationTimestampMs (verified by parsing the plaintext of
  // its own 0x90: native 53 keys, ours 47 when those six were removed).
  // The two join-script seeds (percent-encoded upstream) and the ticket blob
  // from the join reply are what the server checks per session.
  if (join_script.contains("RandomSeed1"))
    doc["RandomSeed1"] = jsonString(join_script, "RandomSeed1");
  if (join_script.contains("APIsecurityToken"))
    doc["APIsecurityToken"] = jsonString(join_script, "APIsecurityToken");
  if (reply.contains("joinTicket") && reply["joinTicket"].is_object()) {
    const nlohmann::json& jt = reply["joinTicket"];
    nlohmann::ordered_json inner;
    inner["SerializedClientFields"] = jsonString(jt, "SerializedClientFields");
    // The join API returns EncryptedServerFields as an *object*
    // ({ephPubKey, nonce, ciphertext, tag}) and the native's wire document
    // carries it as an object too (verified by unsealing a live 0x90,
    // 2026-10-30). Stringifying it — what this builder used to do — makes the
    // RCC read a string where it expects the per-session header; it answers
    // with 0x93 and then never sends the 0x9B challenge. Keep the native's key
    // order (ephPubKey, nonce, ciphertext, tag).
    // Native default is the object form; RBX_ESF_OBJECT=0 restores the
    // stringified form this builder used to send, for A/B runs only.
    const char* esf_obj = getenv("RBX_ESF_OBJECT");
    if (!(esf_obj && *esf_obj == '0') &&
        jt.contains("EncryptedServerFields") &&
        jt["EncryptedServerFields"].is_object()) {
      const nlohmann::json& esf = jt["EncryptedServerFields"];
      nlohmann::ordered_json o = nlohmann::ordered_json::object();
      for (const char* k : {"ephPubKey", "nonce", "ciphertext", "tag"})
        if (esf.contains(k)) o[k] = esf[k];
      // Any future field the API adds still goes through, in object form.
      for (auto it = esf.begin(); it != esf.end(); ++it)
        if (!o.contains(it.key())) o[it.key()] = it.value();
      inner["EncryptedServerFields"] = std::move(o);
    } else {
      inner["EncryptedServerFields"] = jsonString(jt, "EncryptedServerFields");
    }
    doc["__joinTicket"] = inner.dump();
  } else if (reply.is_object()) {
    if (err) *err = "join reply has no joinTicket (0x90 keeps the template's)";
  }
  const std::string json = doc.dump();
  std::vector<uint8_t> body;
  body.insert(body.end(), tmpl.begin(), tmpl.begin() + leb_off);
  putString(&body, json);
  body.insert(body.end(), tmpl.begin() + tail_off, tmpl.end());
  out->json = json;
  out->body = std::move(body);
  return true;
}

bool build8a(uint32_t user_id, const std::string& client_ticket,
             const std::vector<uint8_t>& session_template,
             const nlohmann::json& join_script, std::vector<uint8_t>* body,
             std::string* err) {
  // The session document. The join script's `SessionId` field is *not* a
  // uuid: it is the whole session document, JSON-encoded (the same 36-key
  // object the join reply carries, e.g. {"SessionId":"<uuid>","GameId":…}).
  // The native submits that document verbatim, so the RCC sees this
  // session's SessionId/GameId/SessionStarted/age band/DC choice.
  //
  // Merging the join script into a *captured* document instead (what this
  // used to do) put the 1 KB JSON string into the document's own SessionId
  // and left every other field on the previous session's values; the RCC
  // silently dropped that ticket, which is why the server answered the join
  // with flags/pads but never started the anti-cheat challenge.
  std::string session_json;
  bool have_session_doc = false;
  if (join_script.is_object() && join_script.contains("SessionId") &&
      join_script["SessionId"].is_string()) {
    const std::string raw = join_script["SessionId"].get<std::string>();
    nlohmann::ordered_json parsed = nlohmann::ordered_json::parse(raw, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object()) {
      session_json = parsed.dump();
      have_session_doc = true;
    }
  }
  if (!have_session_doc) {
    // Fallback for a join script without the embedded document: take the
    // shape from the captured 0x8a payload and merge this session's fields.
    if (!session_template.empty() &&
        !parse8aPayload(session_template.data(), session_template.size(), nullptr,
                        nullptr, &session_json, err)) {
      return false;
    }
    if (!session_json.empty()) {
      std::string merged;
      if (!mergeSessionJson(session_json, join_script, &merged, err)) return false;
      session_json = merged;
    }
  }
  const uint64_t zz = (uint64_t(user_id) * 2) ^ (uint64_t(int64_t(user_id) >> 63));
  const uint32_t v31 = ticketHash(client_ticket);
  const uint16_t v8 = uint16_t(0u - kTicketConst * kHashConst);
  const int64_t v36 = (v8 & 0x8000) ? int64_t(v31) : -int64_t(v31);

  // The body excludes the message id: sub_648C61A seals everything after it.
  body->clear();
  putLeb(body, zz);
  putString(body, client_ticket);
  {
    uint8_t v[4];
    store32le(v, 36);
    body->insert(body->end(), v, v + 4);
  }
  putString(body, kConst74);
  putString(body, "Android");
  putString(body, "?");
  putLeb(body, uint64_t(v36));
  putLeb(body, uint64_t(v36 - int64_t(kBadF00D)));
  putString(body, std::string());
  putString(body, session_json);
  {
    uint8_t v[4];
    store32le(v, 0xC001CAFE);
    body->insert(body->end(), v, v + 4);
  }
  return true;
}

std::vector<uint8_t> build92(uint32_t rand32, uint32_t v9) {
  const uint64_t x = (uint64_t(rand32) << 32) | uint64_t(rand32 ^ v9);
  const uint64_t zz = (x << 1) ^ ((rand32 & 0x80000000u) ? ~0ull : 0ull);
  std::vector<uint8_t> out;
  out.push_back(0x92);
  const std::string l = leb128(zz);
  out.insert(out.end(), l.begin(), l.end());
  return out;
}

std::vector<uint8_t> build8f(const std::string& server_id) {
  std::vector<uint8_t> out;
  out.push_back(0x8F);
  putString(&out, server_id);
  return out;
}

std::vector<uint8_t> buildHydration(const std::string& player_info_json,
                                    const std::string& signature) {
  // The two per-account header fields are taken from the king capture
  // (flags 100, nonce e0cbc1e81b). They vary between the two live captures
  // (mizg sent 136 / 84a3d9ba12) but are constant across sessions for one
  // account, which is what this builder is for; the RCC reads them as the
  // attribute-set description, not as a payload checksum (the payload is
  // covered by the hydration service's signature).
  static const uint8_t kHead[] = {0x64, 0x00, 0x00, 0x00, 0x07, 0x01,
                                  0xEF, 0x41, 0x00, 0x00, 0x3D, 0x10,
                                  0xE0, 0xCB, 0xC1, 0xE8, 0x1B};
  std::vector<uint8_t> out;
  out.reserve(2 + sizeof kHead + 5 + player_info_json.size() + signature.size());
  out.push_back(0x83);
  out.push_back(0x04);
  out.insert(out.end(), kHead, kHead + sizeof kHead);
  putLeb(&out, player_info_json.size());
  out.insert(out.end(), player_info_json.begin(), player_info_json.end());
  out.insert(out.end(), signature.begin(), signature.end());
  return out;
}

uint32_t ticketHash(const std::string& ticket) {
  const uint32_t S = kHashConst;
  const uint32_t t = rbxclient::xxh32(
      reinterpret_cast<const uint8_t*>(ticket.data()), ticket.size(), 1);
  // The native computes -17498 * seed (sub_63D7EB2); multiplication is linear
  // mod 2^32, so negating afterwards is the same and keeps it unsigned-clean.
  const uint16_t v8 = uint16_t(0u - kTicketConst * S);
  const unsigned v9 = (v8 & 2) ? 7 : 25;
  const uint32_t v12 = (v8 & 4) ? (0u - S) : (0u - kIterConst);
  const uint32_t v13 = rol32(t + kIterConst, v9) + v12;
  const uint32_t v14 = (v8 & 8) ? S : kIterConst;
  const uint32_t v15 = (v8 & 0x20) ? S : kIterConst;
  const uint32_t v16 = (v8 & 0x40) ? S : kIterConst;
  const uint32_t v17 = (v8 & 0x4000) ? S : kIterConst;
  const uint32_t v18 = v13 * v14;
  const unsigned v19 = (v8 & 0x10) ? 13 : 19;
  const uint32_t v20 = rol32(v16 ^ (v15 - rol32(v18, v19)),
                             (2 * ((v8 & 0xFF) >> 7)) + 15);
  const uint32_t v11 = (v8 & 0x100) ? S : (0u - S);
  const uint32_t v21 = v11 + kIterConst;
  const uint32_t v22 = (v8 & 0x200) ? v21 : (v11 - kIterConst);
  const uint32_t v23 = v20 + v22;
  const uint32_t v25 = rol32(v23, (v8 & 0x400) ? 23 : 9);
  const uint32_t v26 = (v8 & 0x800) ? (0u - v25) : v25;
  const uint32_t v27 = S + v26;
  const uint32_t v28 = (v8 & 0x1000) ? (0u - v27) : v27;
  const uint32_t v29 = v28 + kIterConst;
  return v17 ^ rol32(v29, (v8 & 0x2000) ? 29 : 3);
}

} // namespace rbxmsg
