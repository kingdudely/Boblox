// kdf_probe.c — capture X25519 session key material from a closed binary that
// links OpenSSL dynamically.
//
// The native derives AES-256-GCM keys as BLAKE2b-512(shared || our_pub || peer_pub)
// with rx = digest[0..32] and tx = digest[32..64]. Interposing EVP_DigestUpdate /
// EVP_DigestFinal_ex recovers the 96-byte preimage and the 64-byte digest, which is
// everything needed to decrypt a captured session offline.
//
// build: cc -shared -fPIC -O2 -o kdf_probe.so kdf_probe.c -ldl
// use:   LD_PRELOAD=/path/native_probe.so:/path/kdf_probe.so BOBLOX_KDF_LOG=/tmp/kdf.log
//
// Env:
//   BOBLOX_KDF_LOG  output path (append, one line per 96-byte BLAKE2b-512 digest)
//
// Scope: dump only 96-byte BLAKE2b-512 inputs. Anything else passes through
// untouched, so TLS and every other digest in the process are unaffected.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>

#define MAX_CTX 16
#define MAX_BUF 128

typedef struct {
    EVP_MD_CTX* ctx;
    unsigned char buf[MAX_BUF];
    size_t len;
    int is_blake2b512;
} slot_t;

static slot_t g_slots[MAX_CTX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int (*real_update)(EVP_MD_CTX*, const void*, size_t);
static int (*real_final)(EVP_MD_CTX*, unsigned char*, unsigned int*);

static void hex_append(FILE* f, const unsigned char* p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        fputc(hx[p[i] >> 4], f);
        fputc(hx[p[i] & 0xF], f);
    }
}

static slot_t* slot_for(EVP_MD_CTX* ctx, int create) {
    for (int i = 0; i < MAX_CTX; i++)
        if (g_slots[i].ctx == ctx) return &g_slots[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_CTX; i++) {
        if (g_slots[i].ctx == NULL) {
            g_slots[i].ctx = ctx;
            g_slots[i].len = 0;
            g_slots[i].is_blake2b512 = 0;
            return &g_slots[i];
        }
    }
    return NULL;
}

int EVP_DigestUpdate(EVP_MD_CTX* ctx, const void* data, size_t count) {
    if (!real_update) real_update = dlsym(RTLD_NEXT, "EVP_DigestUpdate");
    if (ctx && data && count) {
        pthread_mutex_lock(&g_lock);
        slot_t* s = slot_for(ctx, 1);
        if (s) {
            if (s->len == 0) {
                const EVP_MD* md = EVP_MD_CTX_get0_md(ctx);
                s->is_blake2b512 =
                    md && EVP_MD_get_type(md) == NID_blake2b512 ? 1 : 0;
            }
            if (s->is_blake2b512 && count <= MAX_BUF - s->len) {
                memcpy(s->buf + s->len, data, count);
                s->len += count;
            } else if (s->is_blake2b512) {
                s->is_blake2b512 = 0; // too large to be the 96-byte preimage
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return real_update(ctx, data, count);
}

int EVP_DigestFinal_ex(EVP_MD_CTX* ctx, unsigned char* md, unsigned int* size) {
    if (!real_final) real_final = dlsym(RTLD_NEXT, "EVP_DigestFinal_ex");
    const int rc = real_final(ctx, md, size);
    if (ctx && rc == 1 && md) {
        pthread_mutex_lock(&g_lock);
        slot_t* s = slot_for(ctx, 0);
        if (s && s->is_blake2b512 && s->len == 96 && size && *size == 64) {
            const char* path = getenv("BOBLOX_KDF_LOG");
            if (path && *path) {
                FILE* f = fopen(path, "a");
                if (f) {
                    fprintf(f, "KDF %zu ", s->len);
                    hex_append(f, s->buf, s->len);
                    fputc(' ', f);
                    hex_append(f, md, 64);
                    fputc('\n', f);
                    fclose(f);
                }
            }
        }
        if (s) { s->ctx = NULL; s->len = 0; s->is_blake2b512 = 0; }
        pthread_mutex_unlock(&g_lock);
    }
    return rc;
}
