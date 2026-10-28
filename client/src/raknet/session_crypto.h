// session_crypto.h — the native client's RakNet session crypto ("SessionCrypto"),
// reverse-engineered from libroblox.so (see FINDINGS.md "RakNet session crypto").
//
// Native shape (IDA sub_44B32BA/sub_44B34A2/sub_44AA9D6 + a live gdb capture of
// the Req2 that the official client sent to Udmux 128.116.46.33:62016):
//
//   shared  = X25519(our ephemeral secret, peer EphemeralEarlyPubKey)
//   digest  = BLAKE2b-512( shared || our public || peer public )
//   rx key  = digest[0:32]     (server -> client; native inner+224)
//   tx key  = digest[32:64]    (client -> server; native inner+192)
//
// Every handshake packet is then IETF ChaCha20-Poly1305 with the 12-byte ASCII
// nonce "UniqueNumber", tag 16 bytes, layout [aad][ct][nonce][tag].
// Both directions were verified byte-for-byte against the live session (the
// server's own Reply2 tag validates under rx with these exact keys).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rbxclient {

// 12-byte handshake nonce (ASCII "UniqueNumber"), transmitted verbatim.
extern const uint8_t kHandshakeNonce[12];

// Datagram (post-handshake) counter base: LE64("UniqueNu") = 0x754E657571696E55.
// Both directions start here, increment by one per datagram and only the low
// 16 bits travel on the wire (nonce = LE64(counter) || "mbeR").
extern const uint64_t kDataCounterBase;

struct SessionKeys {
  uint8_t priv[32]{}; // our X25519 secret (never leaves the process)
  uint8_t pub[32]{};  // our X25519 public key — goes in Req2
  uint8_t tx[32]{};   // client -> server AEAD key (handshake, ChaCha20-Poly1305)
  uint8_t rx[32]{};   // server -> client AEAD key (handshake)

  // Post-handshake datagram keys (AES-256-GCM). Derived from the *second* DH:
  // the server's data key arrives in Reply2 (23 bytes in the AAD tail plus the
  // first 9 bytes of the plaintext), the KDF is the same BLAKE2b-512 over
  // shared || our_pub || peer_pub.
  uint8_t data_tx[32]{};
  uint8_t data_rx[32]{};

  // Fresh random X25519 keypair (native: a per-session ephemeral pair).
  bool generate();

  // Install the peer's public key and derive tx/rx.
  bool install(const uint8_t peer_pub[32]);

  // Derive the post-handshake datagram keys from the Reply2 peer key.
  bool installData(const uint8_t peer_pub[32]);
};

// Rebuild a full 64-bit datagram counter from the 16-bit wire hint, nearest to
// the last counter seen in that direction (native sub_449BDFE).
uint64_t counterFromHint(uint16_t hint, uint64_t last);

// Datagram AEAD: frame = [ciphertext][counter low16 LE][tag 16], no AAD,
// nonce = LE64(counter) || "mbeR", AES-256-GCM. datagramSeal returns the frame
// length (plaintext length + 18) or 0; datagramOpen authenticates and writes
// the plaintext, returning the counter actually used.
size_t datagramSeal(const uint8_t key[32], uint64_t counter, const uint8_t* pt,
                    size_t len, uint8_t* out, size_t cap);
bool datagramOpen(const uint8_t key[32], const uint8_t* frame, size_t len,
                  uint64_t last_counter, uint8_t* out, size_t outcap,
                  size_t* outlen, uint64_t* counter_used);

// Seal buf[0:total) in place: buf[0:aadlen) is the AAD, buf[aadlen:nonce_off)
// the plaintext (sealed in place), buf[nonce_off:nonce_off+12) the nonce and
// buf[nonce_off+12:nonce_off+28) the tag. Returns false on a bad buffer.
bool aeadSeal(const uint8_t key[32], uint8_t* buf, size_t total, size_t nonce_off,
              size_t aadlen);

// Verify + open the same layout. Writes at most outcap bytes to out and sets
// *outlen. False when the tag does not verify (wrong key/tampered packet).
bool aeadOpen(const uint8_t key[32], const uint8_t* buf, size_t total,
              size_t nonce_off, size_t aadlen, uint8_t* out, size_t outcap,
              size_t* outlen);

} // namespace rbxclient
