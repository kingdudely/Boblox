// session_crypto.cpp — OpenSSL implementation of the native SessionCrypto.
#include "raknet/session_crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>

namespace rbxclient {

const uint8_t kHandshakeNonce[12] = {'U', 'n', 'i', 'q', 'u', 'e', 'N',
                                     'u', 'm', 'b', 'e', 'r'};

const uint64_t kDataCounterBase = 0x754E657571696E55ULL; // LE64("UniqueNu")

namespace {

// One-shot BLAKE2b-512 (the KDF's only primitive).
bool blake2b512(const uint8_t* data, size_t len, uint8_t out[64]) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  unsigned int olen = 0;
  bool ok = EVP_DigestInit_ex(ctx, EVP_blake2b512(), nullptr) == 1 &&
            EVP_DigestUpdate(ctx, data, len) == 1 &&
            EVP_DigestFinal_ex(ctx, out, &olen) == 1 && olen == 64;
  EVP_MD_CTX_free(ctx);
  return ok;
}

bool x25519Public(const uint8_t priv[32], uint8_t pub[32]) {
  EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
  if (!k) return false;
  size_t len = 32;
  const bool ok = EVP_PKEY_get_raw_public_key(k, pub, &len) == 1 && len == 32;
  EVP_PKEY_free(k);
  return ok;
}

bool x25519Shared(const uint8_t priv[32], const uint8_t peer_pub[32],
                  uint8_t shared[32]) {
  EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
  if (!k) return false;
  EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_pub, 32);
  if (!peer) {
    EVP_PKEY_free(k);
    return false;
  }
  EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(k, nullptr);
  bool ok = false;
  if (c) {
    size_t len = 32;
    ok = EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, peer) == 1 &&
         EVP_PKEY_derive(c, shared, &len) == 1 && len == 32;
    EVP_PKEY_CTX_free(c);
  }
  EVP_PKEY_free(peer);
  EVP_PKEY_free(k);
  return ok;
}

} // namespace

bool SessionKeys::generate() {
  // The native pair is a per-session ephemeral X25519 keypair; 32 random bytes
  // are the standard construction (X25519 clamps the scalar internally, so
  // pub == X25519(priv, 9) exactly as the native's key exchange does).
  if (RAND_bytes(priv, 32) != 1) return false;
  return x25519Public(priv, pub);
}

namespace {

// native: BLAKE2b-512(shared || our_pub || peer_pub) -> rx=lo32, tx=hi32.
bool derivePair(const uint8_t priv[32], const uint8_t pub[32],
                const uint8_t peer_pub[32], uint8_t rx[32], uint8_t tx[32]) {
  uint8_t shared[32];
  if (!x25519Shared(priv, peer_pub, shared)) return false;
  uint8_t buf[32 * 3];
  std::memcpy(buf, shared, 32);
  std::memcpy(buf + 32, pub, 32);
  std::memcpy(buf + 64, peer_pub, 32);
  uint8_t digest[64];
  if (!blake2b512(buf, sizeof buf, digest)) return false;
  std::memcpy(rx, digest, 32);
  std::memcpy(tx, digest + 32, 32);
  return true;
}

bool aesGcmSeal(const uint8_t key[32], const uint8_t nonce[12],
                const uint8_t* pt, size_t len, uint8_t* ct, uint8_t tag[16]) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;
  int outl = 0;
  if (ok && len) ok = EVP_EncryptUpdate(ctx, ct, &outl, pt, int(len)) == 1;
  int finl = 0;
  if (ok) ok = EVP_EncryptFinal_ex(ctx, ct + outl, &finl) == 1;
  if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

bool aesGcmOpen(const uint8_t key[32], const uint8_t nonce[12],
                const uint8_t* ct, size_t len, const uint8_t tag[16],
                uint8_t* pt) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;
  int outl = 0;
  if (ok && len) ok = EVP_DecryptUpdate(ctx, pt, &outl, ct, int(len)) == 1;
  if (ok)
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                             const_cast<uint8_t*>(tag)) == 1;
  int finl = 0;
  if (ok) ok = EVP_DecryptFinal_ex(ctx, pt + outl, &finl) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

void datagramNonce(uint64_t counter, uint8_t nonce[12]) {
  for (int i = 0; i < 8; ++i) nonce[i] = uint8_t(counter >> (8 * i));
  nonce[8] = 'm';
  nonce[9] = 'b';
  nonce[10] = 'e';
  nonce[11] = 'R';
}

} // namespace

bool SessionKeys::install(const uint8_t peer_pub[32]) {
  return derivePair(priv, pub, peer_pub, rx, tx);
}

bool SessionKeys::installData(const uint8_t peer_pub[32]) {
  return derivePair(priv, pub, peer_pub, data_rx, data_tx);
}

uint64_t counterFromHint(uint16_t hint, uint64_t last) {
  uint64_t c = (last & ~uint64_t(0xffff)) | hint;
  if (c < last && last - c > 0x8000) c += 0x10000;      // hint wrapped forward
  else if (c > last && c - last > 0x8000) c -= 0x10000; // hint wrapped back
  return c;
}

size_t datagramSeal(const uint8_t key[32], uint64_t counter, const uint8_t* pt,
                    size_t len, uint8_t* out, size_t cap) {
  if (cap < len + 18) return 0;
  uint8_t nonce[12];
  datagramNonce(counter, nonce);
  if (!aesGcmSeal(key, nonce, pt, len, out, out + len + 2)) return 0;
  out[len] = uint8_t(counter & 0xff);
  out[len + 1] = uint8_t((counter >> 8) & 0xff);
  return len + 18;
}

bool datagramOpen(const uint8_t key[32], const uint8_t* frame, size_t len,
                  uint64_t last_counter, uint8_t* out, size_t outcap,
                  size_t* outlen, uint64_t* counter_used) {
  if (len < 18 || len - 18 > outcap) return false;
  const size_t ctlen = len - 18;
  const uint16_t hint = uint16_t(frame[ctlen] | (frame[ctlen + 1] << 8));
  const uint64_t near = counterFromHint(hint, last_counter);
  const uint64_t cands[3] = {near, near - 1, near + 1};
  for (uint64_t c : cands) {
    uint8_t nonce[12];
    datagramNonce(c, nonce);
    if (aesGcmOpen(key, nonce, frame, ctlen, frame + ctlen + 2, out)) {
      *outlen = ctlen;
      *counter_used = c;
      return true;
    }
  }
  return false;
}

bool aeadSeal(const uint8_t key[32], uint8_t* buf, size_t total, size_t nonce_off,
              size_t aadlen) {
  if (nonce_off < aadlen || nonce_off + 28 > total) return false;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  const size_t ptlen = nonce_off - aadlen;
  bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, buf + nonce_off) == 1;
  int outl = 0;
  if (ok && aadlen)
    ok = EVP_EncryptUpdate(ctx, nullptr, &outl, buf, int(aadlen)) == 1;
  if (ok && ptlen)
    ok = EVP_EncryptUpdate(ctx, buf + aadlen, &outl, buf + aadlen, int(ptlen)) == 1;
  int finl = 0;
  if (ok) ok = EVP_EncryptFinal_ex(ctx, buf + aadlen + ptlen, &finl) == 1;
  if (ok)
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                             buf + nonce_off + 12) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

bool aeadOpen(const uint8_t key[32], const uint8_t* buf, size_t total,
              size_t nonce_off, size_t aadlen, uint8_t* out, size_t outcap,
              size_t* outlen) {
  if (nonce_off < aadlen || nonce_off + 28 > total) return false;
  const size_t ctlen = nonce_off - aadlen;
  if (ctlen > outcap) return false;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, buf + nonce_off) == 1;
  int outl = 0;
  if (ok && aadlen)
    ok = EVP_DecryptUpdate(ctx, nullptr, &outl, buf, int(aadlen)) == 1;
  if (ok && ctlen)
    ok = EVP_DecryptUpdate(ctx, out, &outl, buf + aadlen, int(ctlen)) == 1;
  if (ok)
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                             const_cast<uint8_t*>(buf + nonce_off + 12)) == 1;
  int finl = 0;
  // A failed tag is the normal "not the server's reply" outcome, not an error:
  // the final call returns 0 and the caller falls through with ok == false.
  if (ok) ok = EVP_DecryptFinal_ex(ctx, out + outl, &finl) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return false;
  *outlen = size_t(outl + finl);
  return true;
}

} // namespace rbxclient
