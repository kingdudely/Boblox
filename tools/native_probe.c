// native_probe.c — LD_PRELOAD probe for the native client (Rum).
//
// Three jobs, one .so, because they share the same process lifetime:
//
//   1. NET  — interpose sendto/sendmsg/recvfrom/recvmsg and dump every UDP
//             datagram (direction + timestamp + bytes) so the native's
//             upstream window can be diffed idle-vs-driven.
//   2. RNG  — interpose getrandom and dump the bytes. The session's X25519
//             private scalar is drawn from an entropy source; every candidate
//             is kept so it can be tested offline against the GCM tag.
//   3. INPUT — interpose SDL_PeepEvents (Rum drains events with it, not
//             SDL_PollEvent) and inject a synthetic key press or thumbstick
//             drag after a delay, so the client emits ControlState frames.
//
// Build:
//   cc -shared -fPIC -O2 -o native_probe.so native_probe.c -ldl
// Run:
//   BOBLOX_NET_DUMP=/tmp/nat.bin LD_PRELOAD=$PWD/native_probe.so
//   ROBLOX_AUTO_EXIT_AFTER_PRESENT_MS=45000 ./Rum/build/roblox ...
//
// Env:
//   BOBLOX_NET_DUMP     capture path (append, one record per line)
//   BOBLOX_DRIVE_AFTER_MS   delay before the synthetic input (default 8000)
//   BOBLOX_DRIVE_HOLD_MS    hold duration (default 4000)
//   BOBLOX_DRIVE_MODE       "key" (default) | "touch" | "off"
//   BOBLOX_DRIVE_LOG    log path (default: stderr)

#define _GNU_SOURCE

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- plumbing

static int g_dump_fd = -2; // -2 = not opened, -1 = disabled
static int g_log_fd = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void open_once(void) {
    if (g_dump_fd != -2)
        return;
    const char* path = getenv("BOBLOX_NET_DUMP");
    g_dump_fd = (path && *path) ? open(path, O_WRONLY | O_CREAT | O_APPEND, 0600)
                                : -1;
    const char* logp = getenv("BOBLOX_DRIVE_LOG");
    if (logp && *logp)
        g_log_fd = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0600);
}

static long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void logline(const char* fmt, ...) {
    char body[448];
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    int n = snprintf(buf, sizeof buf, "[%lld] %s", now_us(), body);
    if (n <= 0)
        return;
    if (g_log_fd >= 0) {
        ssize_t r = write(g_log_fd, buf, (size_t)n);
        (void)r;
    } else {
        ssize_t r = write(STDERR_FILENO, buf, (size_t)n);
        (void)r;
    }
}

static void dump_record(const char* tag, const void* data, size_t len) {
    open_once();
    if (g_dump_fd < 0)
        return;
    static const char hex[] = "0123456789abcdef";
    char buf[4096];
    int head = snprintf(buf, sizeof buf, "%s %lld %zu ", tag, now_us(), len);
    pthread_mutex_lock(&g_lock);
    ssize_t r = write(g_dump_fd, buf, (size_t)head);
    (void)r;
    const unsigned char* p = (const unsigned char*)data;
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > (sizeof buf - 2) / 2)
            chunk = (sizeof buf - 2) / 2;
        for (size_t i = 0; i < chunk; i++) {
            buf[i * 2] = hex[p[written + i] >> 4];
            buf[i * 2 + 1] = hex[p[written + i] & 0xF];
        }
        r = write(g_dump_fd, buf, chunk * 2);
        (void)r;
        written += chunk;
    }
    r = write(g_dump_fd, "\n", 1);
    (void)r;
    pthread_mutex_unlock(&g_lock);
}

static void* json_scan_thread(void* arg);
static void sleep_ms(int ms);
static long long now_us(void);

// Forward declarations for the memory scan (used from sendmsg, defined below).
static const unsigned char kReq2Magic[9] = {0x78, 0x00, 0xff, 0xff, 0x00,
                                            0xfe, 0xfe, 0xfe, 0xfe};
static int g_pub_scanned = 0;
static int g_json_found = 0;
static pthread_once_t g_json_once = PTHREAD_ONCE_INIT;
static void scan_pub(const unsigned char* pub);

// ---------------------------------------------------------------- network

typedef ssize_t (*sendto_fn)(int, const void*, size_t, int,
                             const struct sockaddr*, socklen_t);
typedef ssize_t (*sendmsg_fn)(int, const struct msghdr*, int);
typedef ssize_t (*recvfrom_fn)(int, void*, size_t, int, struct sockaddr*,
                               socklen_t*);
typedef ssize_t (*recvmsg_fn)(int, struct msghdr*, int);
typedef ssize_t (*getrandom_fn)(void*, size_t, unsigned);

static sendto_fn real_sendto;
static sendmsg_fn real_sendmsg;
static recvfrom_fn real_recvfrom;
static recvmsg_fn real_recvmsg;
static getrandom_fn real_getrandom;

__attribute__((constructor)) static void resolve_early(void) {
    open_once();
    real_sendto = (sendto_fn)dlsym(RTLD_NEXT, "sendto");
    real_sendmsg = (sendmsg_fn)dlsym(RTLD_NEXT, "sendmsg");
    real_recvfrom = (recvfrom_fn)dlsym(RTLD_NEXT, "recvfrom");
    real_recvmsg = (recvmsg_fn)dlsym(RTLD_NEXT, "recvmsg");
    real_getrandom = (getrandom_fn)dlsym(RTLD_NEXT, "getrandom");
    logline("[probe] loaded pid=%d\n", (int)getpid());
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* addr, socklen_t alen) {
    if (!real_sendto)
        real_sendto = (sendto_fn)dlsym(RTLD_NEXT, "sendto");
    if (!g_pub_scanned) {
        const unsigned char* b = (const unsigned char*)buf;
        for (size_t k = 0; k + 9 + 55 <= len && k < 200; k++) {
            if (memcmp(b + k, kReq2Magic, 9) == 0) {
                g_pub_scanned = 1;
                logline("[probe] Req2 seen (sendto), scanning for the public key\n");
                scan_pub(b + k + 23);
                break;
            }
        }
    }
    dump_record("TX", buf, len);
    return real_sendto(fd, buf, len, flags, addr, alen);
}

ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
    if (!real_sendmsg)
        real_sendmsg = (sendmsg_fn)dlsym(RTLD_NEXT, "sendmsg");
    if (msg && msg->msg_iov) {
        size_t total = 0;
        for (size_t i = 0; i < msg->msg_iovlen; i++)
            total += msg->msg_iov[i].iov_len;
        if (total > 0 && total <= 65536) {
            unsigned char* flat = (unsigned char*)malloc(total);
            if (flat) {
                size_t off = 0;
                for (size_t i = 0; i < msg->msg_iovlen; i++) {
                    memcpy(flat + off, msg->msg_iov[i].iov_base,
                           msg->msg_iov[i].iov_len);
                    off += msg->msg_iov[i].iov_len;
                }
                dump_record("TXM", flat, total);
                free(flat);
            }
        }
    }
    if (!g_pub_scanned) {
        for (size_t i = 0; i < msg->msg_iovlen; i++) {
            const unsigned char* b = (const unsigned char*)msg->msg_iov[i].iov_base;
            size_t n = msg->msg_iov[i].iov_len;
            for (size_t k = 0; k + 9 + 55 <= n && k < 200; k++) {
                if (memcmp(b + k, kReq2Magic, 9) == 0) {
                    g_pub_scanned = 1;
                    logline("[probe] Req2 seen, scanning for the public key\n");
                    scan_pub(b + k + 23);
                    break;
                }
            }
        }
    }
    return real_sendmsg(fd, msg, flags);
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* addr, socklen_t* alen) {
    if (!real_recvfrom)
        real_recvfrom = (recvfrom_fn)dlsym(RTLD_NEXT, "recvfrom");
    ssize_t n = real_recvfrom(fd, buf, len, flags, addr, alen);
    if (n > 0)
        dump_record("RX", buf, (size_t)n);
    return n;
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
    if (!real_recvmsg)
        real_recvmsg = (recvmsg_fn)dlsym(RTLD_NEXT, "recvmsg");
    ssize_t n = real_recvmsg(fd, msg, flags);
    if (n > 0)
        dump_record("RXM", msg->msg_iov[0].iov_base, (size_t)n);
    return n;
}

ssize_t getrandom(void* buf, size_t len, unsigned flags) {
    if (!real_getrandom)
        real_getrandom = (getrandom_fn)dlsym(RTLD_NEXT, "getrandom");
    ssize_t n = real_getrandom(buf, len, flags);
    if (n > 0)
        dump_record("RND", buf, (size_t)n);
    return n;
}

// ---------------------------------------------------------------- memory
//
// The session's X25519 private scalar is drawn by BoringSSL through the
// getrandom *syscall*, not libc, so the RNG hook above never sees it. What we
// can see is the public key the client puts in Req2. Scanning the process for
// that 32-byte pattern and dumping the neighbourhood gives an offline search
// space small enough to test every 32-byte window against the public key.

static void scan_pub(const unsigned char* pub) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    int mem = open("/proc/self/mem", O_RDONLY);
    if (mem < 0) { fclose(maps); return; }
    static unsigned char chunk[1 << 20];
    char line[512];
    int hits = 0;
    while (hits < 6 && fgets(line, sizeof line, maps)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        for (unsigned long off = lo; off < hi && hits < 6; off += sizeof chunk) {
            size_t want = (hi - off) < sizeof chunk ? (size_t)(hi - off) : sizeof chunk;
            ssize_t n = pread(mem, chunk, want, (off_t)off);
            if (n < 32) break;
            for (ssize_t i = 0; i + 32 <= n && hits < 6; i++) {
                if (memcmp(chunk + i, pub, 32) != 0) continue;
                hits++;
                unsigned long hit_at = off + (unsigned long)i;
                unsigned long dlo = hit_at > 65536 ? hit_at - 65536 : 0;
                size_t dlen = 131072;
                if (dlo + dlen > hit_at + 65536) dlen = (hit_at + 65536) - dlo;
                static unsigned char region[131072];
                ssize_t rn = pread(mem, region, dlen, (off_t)dlo);
                if (rn > 0) {
                    char hdr[96];
                    snprintf(hdr, sizeof hdr, "MEM %lld %zd %lx %lx ", now_us(),
                             rn, hit_at, dlo);
                    char buf[4096];
                    int hn = snprintf(buf, sizeof buf, "%s", hdr);
                    pthread_mutex_lock(&g_lock);
                    ssize_t w = write(g_dump_fd, buf, (size_t)hn);
                    (void)w;
                    static const char hexc[] = "0123456789abcdef";
                    for (ssize_t k = 0; k < rn; k += (ssize_t)((sizeof buf - 2) / 2)) {
                        ssize_t m = rn - k;
                        if (m > (ssize_t)((sizeof buf - 2) / 2)) m = (ssize_t)((sizeof buf - 2) / 2);
                        for (ssize_t j = 0; j < m; j++) {
                            buf[j * 2] = hexc[region[k + j] >> 4];
                            buf[j * 2 + 1] = hexc[region[k + j] & 0xF];
                        }
                        w = write(g_dump_fd, buf, (size_t)m * 2);
                        (void)w;
                    }
                    w = write(g_dump_fd, "\n", 1);
                    (void)w;
                    pthread_mutex_unlock(&g_lock);
                    logline("[probe] pub hit at %lx, dumped %zd bytes\n", hit_at, rn);
                }
                break;
            }
        }
    }
    close(mem);
    fclose(maps);
    logline("[probe] pub scan done, %d hit(s)\n", hits);
}

// The 0x90 body is JSON built in memory before it is sealed. Polling for its
// first key gives the join template without touching the cipher.
static void* json_scan_thread(void* arg) {
    (void)arg;
    static const char kProbe[] = "{\"UserId\"";
    static unsigned char chunk[1 << 20];
    for (int round = 0; round < 600 && !g_json_found; round++) {
        sleep_ms(200);
        FILE* maps = fopen("/proc/self/maps", "r");
        int mem = maps ? open("/proc/self/mem", O_RDONLY) : -1;
        if (!maps || mem < 0) { if (maps) fclose(maps); continue; }
        char line[512];
        const unsigned long self_probe = (unsigned long)(uintptr_t)&kProbe;
        while (fgets(line, sizeof line, maps) && !g_json_found) {
            unsigned long lo = 0, hi = 0;
            char perms[8] = {0};
            if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
            if (perms[0] != 'r' || (perms[1] != 'w' && perms[1] != '-')) continue;
            // Skip this module: its own format strings contain the probes.
            (void)self_probe;
            if (strstr(line, "native_probe.so") != NULL) continue;
            if (hi - lo > (1UL << 31)) continue; // skip huge reservations
            for (unsigned long off = lo; off < hi; off += sizeof chunk) {
                size_t want = (hi - off) < sizeof chunk ? (size_t)(hi - off) : sizeof chunk;
                ssize_t n = pread(mem, chunk, want, (off_t)off);
                if (n < 12) break;
                for (ssize_t i = 0; i + 8 <= n; i++) {
                    if (memcmp(chunk + i, kProbe, sizeof kProbe - 1) != 0) continue;
                    unsigned long hit = off + (unsigned long)i;
                    unsigned long dlo = hit > 262144 ? hit - 262144 : 0;
                    size_t dlen = 524288;
                    static unsigned char region[524288];
                    ssize_t rn = pread(mem, region, dlen, (off_t)dlo);
                    // Reject our own probe literal: the real body also carries
                    // the rest of the join document.
                    int real = 0;
                    if (rn > 0) {
                        static const char kMore[] = "\"PlaceId\"";
                        for (ssize_t q = 0; q + (ssize_t)(sizeof kMore - 1) <= rn; q++)
                            if (memcmp(region + q, kMore, sizeof kMore - 1) == 0) { real = 1; break; }
                    }
                    if (rn > 0 && real) {
                        char hdr[96];
                        snprintf(hdr, sizeof hdr, "JSON %lld %zd %lx %lx ", now_us(),
                                 rn, hit, dlo);
                        pthread_mutex_lock(&g_lock);
                        ssize_t w = write(g_dump_fd, hdr, strlen(hdr));
                        (void)w;
                        static const char hx[] = "0123456789abcdef";
                        char buf[4096];
                        for (ssize_t k = 0; k < rn; k += (ssize_t)((sizeof buf - 2) / 2)) {
                            ssize_t m = rn - k;
                            if (m > (ssize_t)((sizeof buf - 2) / 2)) m = (ssize_t)((sizeof buf - 2) / 2);
                            for (ssize_t j = 0; j < m; j++) {
                                buf[j * 2] = hx[region[k + j] >> 4];
                                buf[j * 2 + 1] = hx[region[k + j] & 0xF];
                            }
                            w = write(g_dump_fd, buf, (size_t)m * 2);
                            (void)w;
                        }
                        w = write(g_dump_fd, "\n", 1);
                        (void)w;
                        pthread_mutex_unlock(&g_lock);
                        g_json_found = 1;
                        logline("[probe] 0x90 JSON hit at %lx\n", hit);
                    }
                    break;
                }
                if (g_json_found) break;
            }
        }
        close(mem);
        fclose(maps);
    }
    return NULL;
}

static void start_json_scan(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, json_scan_thread, NULL) == 0) pthread_detach(t);
}

// ---------------------------------------------------------------- input

typedef int (*peep_fn)(SDL_Event*, int, SDL_EventAction, Uint32, Uint32);
static peep_fn real_peep;

#define QUEUE_MAX 16
static SDL_Event g_queue[QUEUE_MAX];
static int g_queue_head = 0;
static int g_queue_tail = 0;

static void queue_push(const SDL_Event* e) {
    pthread_mutex_lock(&g_lock);
    int next = (g_queue_tail + 1) % QUEUE_MAX;
    if (next != g_queue_head) {
        g_queue[g_queue_tail] = *e;
        g_queue_tail = next;
    } else {
        logline("[probe] event queue full, dropping type=%u\n", e->type);
    }
    pthread_mutex_unlock(&g_lock);
}

static int queue_pop(SDL_Event* out, int max, Uint32 minType, Uint32 maxType) {
    int n = 0;
    pthread_mutex_lock(&g_lock);
    while (n < max && g_queue_head != g_queue_tail) {
        const SDL_Event* e = &g_queue[g_queue_head];
        if (e->type >= minType && e->type <= maxType) {
            out[n++] = *e;
            g_queue_head = (g_queue_head + 1) % QUEUE_MAX;
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

static void push_key(Uint32 type) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = 0;
    e.key.which = 0;
    e.key.scancode = SDL_SCANCODE_W;
    e.key.key = SDLK_W;
    e.key.mod = 0;
    e.key.repeat = false;
    e.key.down = (type == SDL_EVENT_KEY_DOWN);
    queue_push(&e);
    logline("[probe] injected %s scancode=W\n",
            type == SDL_EVENT_KEY_DOWN ? "KEY_DOWN" : "KEY_UP");
}

static void push_finger(Uint32 type, float x, float y, SDL_FingerID id) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.tfinger.timestamp = SDL_GetTicksNS();
    e.tfinger.touchID = 0;
    e.tfinger.fingerID = id;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.dx = 0;
    e.tfinger.dy = 0;
    e.tfinger.pressure = (type == SDL_EVENT_FINGER_UP) ? 0.0f : 1.0f;
    e.tfinger.windowID = 0;
    queue_push(&e);
    logline("[probe] injected FINGER type=%u x=%.3f y=%.3f\n", type, x, y);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void* drive_thread(void* arg) {
    (void)arg;
    const char* mode = getenv("BOBLOX_DRIVE_MODE");
    if (mode && strcmp(mode, "off") == 0) {
        logline("[probe] drive disabled\n");
        return NULL;
    }
    int after = 8000, hold = 4000;
    if (getenv("BOBLOX_DRIVE_AFTER_MS"))
        after = atoi(getenv("BOBLOX_DRIVE_AFTER_MS"));
    if (getenv("BOBLOX_DRIVE_HOLD_MS"))
        hold = atoi(getenv("BOBLOX_DRIVE_HOLD_MS"));

    sleep_ms(after);
    logline("[probe] drive start mode=%s after=%d hold=%d\n",
            mode ? mode : "key", after, hold);

    if (mode && strcmp(mode, "quad") == 0) {
        // up, left, right, down — the four legs diff into the stick vector.
        const float cx = 0.12f, cy = 0.74f;
        const float dx[4] = {0.0f, -0.10f, 0.10f, 0.0f};
        const float dy[4] = {-0.12f, 0.0f, 0.0f, 0.12f};
        for (int leg = 0; leg < 4; leg++) {
            const float x0 = cx, y0 = cy;
            const float x1 = cx + dx[leg], y1 = cy + dy[leg];
            push_finger(SDL_EVENT_FINGER_DOWN, x0, y0, (SDL_FingerID)(leg + 1));
            sleep_ms(16);
            for (int i = 1; i <= 6; i++) {
                push_finger(SDL_EVENT_FINGER_MOTION,
                            x0 + (x1 - x0) * (float)i / 6.0f,
                            y0 + (y1 - y0) * (float)i / 6.0f,
                            (SDL_FingerID)(leg + 1));
                sleep_ms(16);
            }
            sleep_ms(hold);
            push_finger(SDL_EVENT_FINGER_UP, x1, y1, (SDL_FingerID)(leg + 1));
            logline("[probe] quad leg %d done\n", leg);
            sleep_ms(1200);
        }
        logline("[probe] drive end\n");
        return NULL;
    }
    if (mode && strcmp(mode, "seq") == 0) {
        // Forward, release, backward, release — the two hold windows diff into
        // the movement vector (fields that flip sign are the vector).
        const float x = 0.12f, low = 0.86f, high = 0.62f;
        for (int dir = 0; dir < 2; dir++) {
            const float from = dir == 0 ? low : high;
            const float to = dir == 0 ? high : low;
            push_finger(SDL_EVENT_FINGER_DOWN, x, from, 1);
            sleep_ms(16);
            for (int i = 1; i <= 8; i++) {
                push_finger(SDL_EVENT_FINGER_MOTION, x,
                            from + (to - from) * (float)i / 8.0f, 1);
                sleep_ms(16);
            }
            sleep_ms(hold);
            push_finger(SDL_EVENT_FINGER_UP, x, to, 1);
            logline("[probe] seq leg %d hold done\n", dir);
            sleep_ms(1500);
        }
        logline("[probe] drive end\n");
        return NULL;
    }
    if (mode && strcmp(mode, "touch") == 0) {
        // Right-hand-side-free thumbstick: press low-left, drag up, hold.
        const float x = 0.12f, y0 = 0.86f, y1 = 0.62f;
        push_finger(SDL_EVENT_FINGER_DOWN, x, y0, 1);
        sleep_ms(16);
        for (int i = 1; i <= 8; i++) {
            float y = y0 + (y1 - y0) * (float)i / 8.0f;
            push_finger(SDL_EVENT_FINGER_MOTION, x, y, 1);
            sleep_ms(16);
        }
        sleep_ms(hold);
        push_finger(SDL_EVENT_FINGER_UP, x, y1, 1);
    } else {
        push_key(SDL_EVENT_KEY_DOWN);
        sleep_ms(hold);
        push_key(SDL_EVENT_KEY_UP);
    }
    logline("[probe] drive end\n");
    return NULL;
}

static pthread_once_t g_drive_once = PTHREAD_ONCE_INIT;

static void start_drive(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, drive_thread, NULL) == 0)
        pthread_detach(t);
}

int SDL_PeepEvents(SDL_Event* events, int numevents, SDL_EventAction action,
                   Uint32 minType, Uint32 maxType) {
    if (!real_peep)
        real_peep = (peep_fn)dlsym(RTLD_NEXT, "SDL_PeepEvents");
    pthread_once(&g_drive_once, start_drive);
    pthread_once(&g_json_once, start_json_scan);

    static int calls = 0, hits = 0;
    if (calls++ == 0)
        logline("[probe] SDL_PeepEvents intercepted (action=%d range=%u..%u)\n",
                (int)action, minType, maxType);
    if (action == SDL_GETEVENT && events && numevents > 0) {
        int n = queue_pop(events, numevents, minType, maxType);
        if (n > 0) {
            if (hits++ < 4)
                logline("[probe] delivered %d synthetic event(s), first type=%u\n",
                        n, events[0].type);
            return n;
        }
    }
    return real_peep(events, numevents, action, minType, maxType);
}
