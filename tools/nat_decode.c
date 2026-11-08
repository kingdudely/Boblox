// nat_decode.c — offline decoder for a native (Rum) session capture.
//
// The probe (native_probe.c) dumps UDP datagrams, entropy draws and memory
// neighbourhoods. This tool turns that into plaintext application messages:
//
//   1. Req2 in the capture carries the client's X25519 public key in the clear.
//   2. The private scalar is not drawn through libc getrandom (BoringSSL uses
//      the syscall), so it is recovered by testing every 32-byte window in the
//      probed memory regions against that public key.
//   3. The handshake peer key is recovered by using the Reply2 AEAD tag as an
//      oracle (ChaCha20-Poly1305, nonce "UniqueNumber").
//   4. Reply2 decrypts, and its AAD tail + first 9 plaintext bytes complete the
//      data peer key; the data keys are the same BLAKE2b(x25519||pub||peer)
//      split (rx = [0:32], tx = [32:64]).
//   5. Every client datagram (RUPP header 0x1f, 31 bytes) opens with AES-256-
//      GCM, nonce = LE64(counter) || "mbeR", and is printed as hex.
//
// Build:
//   cc -O2 -o nat_decode nat_decode.c -lcrypto
// Run:
//   ./nat_decode /tmp/nat.bin > /tmp/txplain.txt
//
// Output lines: "<capture timestamp> <plaintext hex>" — one per datagram.

#include <openssl/evp.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* kHex = "0123456789abcdef";

static int unhex(const char* s, unsigned char* out, int max) {
    int n = 0;
    for (; s[0] && s[1] && n < max; s += 2) {
        const char* a = strchr(kHex, s[0]);
        const char* b = strchr(kHex, s[1]);
        if (!a || !b) break;
        out[n++] = (unsigned char)(((a - kHex) << 4) | (b - kHex));
    }
    return n;
}

static int x25519(const unsigned char* priv, const unsigned char* peer,
                  unsigned char* out) {
    EVP_PKEY* a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
    EVP_PKEY* b = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, 32);
    if (!a || !b) { EVP_PKEY_free(a); EVP_PKEY_free(b); return 0; }
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(a, NULL);
    size_t n = 32;
    int ok = c && EVP_PKEY_derive_init(c) == 1 &&
             EVP_PKEY_derive_set_peer(c, b) == 1 &&
             EVP_PKEY_derive(c, out, &n) == 1;
    EVP_PKEY_CTX_free(c); EVP_PKEY_free(a); EVP_PKEY_free(b);
    return ok;
}

static int blake2b512(const unsigned char* in, size_t n, unsigned char out[64]) {
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    unsigned int olen = 0;
    int ok = c && EVP_DigestInit_ex(c, EVP_blake2b512(), NULL) == 1 &&
             EVP_DigestUpdate(c, in, n) == 1 &&
             EVP_DigestFinal_ex(c, out, &olen) == 1 && olen == 64;
    EVP_MD_CTX_free(c);
    return ok;
}

static int derive(const unsigned char* priv, const unsigned char* pub,
                  const unsigned char* peer, unsigned char digest[64]) {
    unsigned char shared[32], msg[96];
    if (!x25519(priv, peer, shared)) return 0;
    memcpy(msg, shared, 32);
    memcpy(msg + 32, pub, 32);
    memcpy(msg + 64, peer, 32);
    return blake2b512(msg, 96, digest);
}

static int chacha_open(const unsigned char* key, const unsigned char* nonce,
                       const unsigned char* aad, int aadlen,
                       const unsigned char* ct, int ctlen,
                       const unsigned char* tag, unsigned char* pt) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int outl = 0, fin = 0, total = 0;
    int ok = c && EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), NULL, NULL, NULL) == 1 &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) == 1 &&
             EVP_DecryptInit_ex(c, NULL, NULL, key, nonce) == 1;
    if (ok && aadlen > 0) ok = EVP_DecryptUpdate(c, NULL, &outl, aad, aadlen) == 1;
    if (ok) ok = EVP_DecryptUpdate(c, pt, &outl, ct, ctlen) == 1;
    total = outl;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, (void*)tag) == 1;
    if (ok) ok = EVP_DecryptFinal_ex(c, pt + total, &fin) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok;
}

static void nonce_handshake(unsigned char n[12]) {
    unhex("556e697175654e756d626572", n, 12); // "UniqueNumber"
}

static void nonce_data(unsigned long long ctr, unsigned char n[12]) {
    for (int i = 0; i < 8; i++) n[i] = (unsigned char)(ctr >> (8 * i));
    n[8] = 'm'; n[9] = 'b'; n[10] = 'e'; n[11] = 'R';
}

static int gcm_open(const unsigned char* key, const unsigned char* nonce,
                    const unsigned char* ct, int ctlen,
                    const unsigned char* tag, unsigned char* pt) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int outl = 0, fin = 0, total = 0;
    int ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) == 1 &&
             EVP_DecryptInit_ex(c, NULL, NULL, key, nonce) == 1;
    if (ok) ok = EVP_DecryptUpdate(c, pt, &outl, ct, ctlen) == 1;
    total = outl;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, (void*)tag) == 1;
    if (ok) ok = EVP_DecryptFinal_ex(c, pt + total, &fin) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok;
}

static const unsigned char kReq2Magic[9] = {0x78, 0x00, 0xff, 0xff, 0x00,
                                            0xfe, 0xfe, 0xfe, 0xfe};
static const unsigned char kReply2Magic[9] = {0x7d, 0x00, 0xff, 0xff, 0x00,
                                              0xfe, 0xfe, 0xfe, 0xfe};

typedef struct {
    unsigned char* mem;
    size_t mem_len;
    unsigned char pub[32];
    unsigned char reply2[2048];
    size_t reply2_len;
    bool have_pub;
} Capture;

static void note_memory(Capture* cap, const char* hex) {
    const size_t n = strlen(hex) / 2;
    unsigned char* grown = (unsigned char*)realloc(cap->mem, cap->mem_len + n);
    if (!grown) return;
    cap->mem = grown;
    unhex(hex, cap->mem + cap->mem_len, (int)n);
    cap->mem_len += n;
}

static bool scan_capture(const char* path, Capture* cap) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); return false; }
    static char line[900000];
    while (fgets(line, sizeof line, f)) {
        char* save = NULL;
        char* tag = strtok_r(line, " \n", &save);
        if (!tag) continue;
        if (strcmp(tag, "MEM") == 0) {
            (void)strtok_r(NULL, " \n", &save); // ts
            (void)strtok_r(NULL, " \n", &save); // len
            (void)strtok_r(NULL, " \n", &save); // hit address
            (void)strtok_r(NULL, " \n", &save); // dump address
            char* hex = strtok_r(NULL, " \n", &save);
            if (hex) note_memory(cap, hex);
            continue;
        }
        if (strcmp(tag, "TX") != 0 && strcmp(tag, "TXM") != 0 &&
            strcmp(tag, "RX") != 0 && strcmp(tag, "RXM") != 0)
            continue;
        (void)strtok_r(NULL, " \n", &save); // ts
        (void)strtok_r(NULL, " \n", &save); // len
        char* hex = strtok_r(NULL, " \n", &save);
        if (!hex) continue;
        static unsigned char buf[200000];
        const int n = unhex(hex, buf, (int)sizeof buf);
        if (n < 24) continue;
        const bool outbound = tag[0] == 'T';
        if (outbound && !cap->have_pub) {
            for (int k = 0; k + 9 + 55 <= n && k < 200; k++) {
                if (memcmp(buf + k, kReq2Magic, 9) == 0) {
                    memcpy(cap->pub, buf + k + 23, 32);
                    cap->have_pub = true;
                    break;
                }
            }
        }
        if (!outbound && cap->reply2_len == 0) {
            for (int k = 0; k + 9 + 40 <= n; k++) {
                if (memcmp(buf + k, kReply2Magic, 9) == 0) {
                    cap->reply2_len = (size_t)(n - k);
                    if (cap->reply2_len > sizeof cap->reply2)
                        cap->reply2_len = sizeof cap->reply2;
                    memcpy(cap->reply2, buf + k, cap->reply2_len);
                    break;
                }
            }
        }
    }
    fclose(f);
    return true;
}

static bool recover_private(Capture* cap, unsigned char priv[32]) {
    unsigned char probe[32];
    for (size_t i = 0; i + 32 <= cap->mem_len; i++) {
        EVP_PKEY* pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
                                                    cap->mem + i, 32);
        if (!pk) continue;
        size_t n = 32;
        const bool ok = EVP_PKEY_get_raw_public_key(pk, probe, &n) == 1 &&
                        memcmp(probe, cap->pub, 32) == 0;
        EVP_PKEY_free(pk);
        if (ok) {
            memcpy(priv, cap->mem + i, 32);
            fprintf(stderr, "priv: found at memory offset %zu\n", i);
            return true;
        }
    }
    return false;
}

static bool recover_peer(Capture* cap, const unsigned char priv[32],
                         unsigned char peer[32]) {
    unsigned char shared[32], msg[96], digest[64], pt[2048], nonce[12];
    const size_t aadlen = cap->reply2[18];
    nonce_handshake(nonce);
    for (size_t i = 0; i + 32 <= cap->mem_len; i++) {
        if (!x25519(priv, cap->mem + i, shared)) continue;
        memcpy(msg, shared, 32);
        memcpy(msg + 32, cap->pub, 32);
        memcpy(msg + 64, cap->mem + i, 32);
        if (!blake2b512(msg, 96, digest)) continue;
        if (cap->reply2_len >= 40 && aadlen + 28 <= cap->reply2_len &&
            chacha_open(digest, nonce, cap->reply2, (int)aadlen,
                        cap->reply2 + aadlen,
                        (int)(cap->reply2_len - 28 - aadlen),
                        cap->reply2 + cap->reply2_len - 16, pt)) {
            memcpy(peer, cap->mem + i, 32);
            fprintf(stderr, "peer: found at memory offset %zu\n", i);
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <capture>\n", argv[0]);
        return 2;
    }
    Capture cap;
    memset(&cap, 0, sizeof cap);
    if (!scan_capture(argv[1], &cap)) return 1;
    if (!cap.have_pub) { fprintf(stderr, "no Req2 in capture\n"); return 1; }
    if (cap.reply2_len == 0) { fprintf(stderr, "no Reply2 in capture\n"); return 1; }
    if (cap.mem_len == 0) { fprintf(stderr, "no MEM records (run with the probe)\n"); return 1; }

    unsigned char priv[32], peer[32];
    if (!recover_private(&cap, priv)) {
        fprintf(stderr, "private key not in the probed memory\n");
        return 1;
    }
    if (!recover_peer(&cap, priv, peer)) {
        fprintf(stderr, "peer key not in the probed memory\n");
        return 1;
    }

    // Handshake open of Reply2 -> the 9 plaintext bytes of the data peer key.
    unsigned char hd[64], r2pt[2048], nonce[12];
    if (!derive(priv, cap.pub, peer, hd)) return 1;
    nonce_handshake(nonce);
    const size_t aadlen = cap.reply2[18];
    if (!chacha_open(hd, nonce, cap.reply2, (int)aadlen, cap.reply2 + aadlen,
                     (int)(cap.reply2_len - 28 - aadlen),
                     cap.reply2 + cap.reply2_len - 16, r2pt)) {
        fprintf(stderr, "Reply2 did not open\n");
        return 1;
    }
    unsigned char dpeer[32];
    memcpy(dpeer, cap.reply2 + 21, 23);
    memcpy(dpeer + 23, r2pt, 9);

    unsigned char dd[64];
    if (!derive(priv, cap.pub, dpeer, dd)) return 1;
    const unsigned char* tx = dd + 32;

    // Re-read the capture for datagrams (the MEM records were consumed above).
    FILE* f = fopen(argv[1], "r");
    if (!f) return 1;
    static char line[900000];
    const unsigned long long base = 0x754E657571696E55ULL;
    unsigned long long last = base;
    int count = 0;
    while (fgets(line, sizeof line, f)) {
        char* save = NULL;
        char* tag = strtok_r(line, " \n", &save);
        if (!tag || (strcmp(tag, "TX") && strcmp(tag, "TXM"))) continue;
        char* tsp = strtok_r(NULL, " \n", &save);
        (void)strtok_r(NULL, " \n", &save);
        char* hex = strtok_r(NULL, " \n", &save);
        if (!hex) continue;
        static unsigned char d[200000];
        const int n = unhex(hex, d, (int)sizeof d);
        if (n < 40 || d[0] != 0x01 || d[2] != 0 || d[3] != 0x1f) continue;
        const unsigned char* frame = d + 31;
        const int flen = n - 31;
        if (flen < 18) continue;
        const int ctlen = flen - 18;
        const unsigned short hint =
            (unsigned short)(frame[ctlen] | (frame[ctlen + 1] << 8));
        unsigned long long cands[8];
        int nc = 0;
        cands[nc++] = (last & ~0xffffULL) | hint;
        for (int k = 1; k <= 3; k++) {
            cands[nc++] = base + (unsigned long long)k;
            cands[nc++] = ((last & ~0xffffULL) | hint) + (unsigned long long)k;
        }
        unsigned char pt[2048];
        for (int i = 0; i < nc; i++) {
            nonce_data(cands[i], nonce);
            if (!gcm_open(tx, nonce, frame, ctlen, frame + ctlen + 2, pt))
                continue;
            printf("%s ", tsp ? tsp : "0");
            for (int k = 0; k < ctlen; k++) printf("%02x", pt[k]);
            printf("\n");
            last = cands[i];
            count++;
            break;
        }
    }
    fclose(f);
    fprintf(stderr, "decrypted %d datagrams\n", count);
    free(cap.mem);
    return count > 0 ? 0 : 1;
}
