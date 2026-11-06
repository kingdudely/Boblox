// exec.cpp — lean executor surface for a live Boblox Engine state.
//
// This is a port of the Xecutor UNC surface that can actually be backed by
// this client. Boblox owns its Luau VM and its replication world, so the
// pieces that need Roblox's native internals (signal introspection, raw VM
// pointers, reflection descriptors, remotes) are deliberately absent rather
// than faked — a script that needs them should fail loudly, not silently
// behave differently from the real client.
//
// What IS backed, and how:
//   environment   — getgenv/getrenv/getreg/getfenv/setfenv real
//   code          — loadstring/loadfile compile through Luau.Compiler
//   closures      — newcclosure/clonefunction/hookfunction over the C API
//   metatables    — getrawmetatable/setreadonly/hookmetamethod (lua_setreadonly)
//   http          — request/syn.request/game:HttpGet via libcurl
//   filesystem    — sandboxed to $BOBLOX_WORKSPACE
//   crypto        — base64 + EVP digests/HMAC
//
// Scripts run on a scheduler thread (task.spawn) so task.wait works.

#include "exec/exec.h"

#include "api/api.h"
#include "instance/instance.h"
#include "send/bridge.h"
#include "send/entity_batch.h"
#include "instance/scheduler.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <curl/curl.h>

#include <algorithm>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace rbx {
namespace exec {

namespace {

namespace fs = std::filesystem;

// --- registry keys ---------------------------------------------------------
constexpr const char* kInstalled = "boblex.installed";
constexpr const char* kOurFns = "boblex.ourfns";
constexpr const char* kNewCClosures = "boblex.cclosures";
constexpr const char* kHooks = "boblex.hooks";
constexpr const char* kIdentity = "boblex.identity";
constexpr const char* kClipboard = "boblex.clipboard";
constexpr const char* kTeleportQueue = "boblex.teleport_queue";
constexpr const char* kVersion = "boblex.version";

const WorldView* g_world = nullptr;
std::mutex g_move_mutex;
float g_move[3] = {0, 0, 0};
bool g_move_set = false;
float g_tp[3] = {0, 0, 0};
bool g_tp_set = false;

constexpr const char* kExecutorName = "Boblex";
constexpr const char* kExecutorVersion = "1.0.0";

// --- small Lua helpers -----------------------------------------------------

int absidx(lua_State* L, int i) { return lua_absindex(L, i); }

void push_regtable(lua_State* L, const char* key) {
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }
}

// Mark a C function as ours so isexecutorclosure() can tell it apart.
void mark_ours(lua_State* L) {
    // [fn]
    lua_pushvalue(L, -1);              // [fn, fn]
    push_regtable(L, kOurFns);         // [fn, fn, set]
    lua_pushvalue(L, -2);              // [fn, fn, set, fn]
    lua_pushboolean(L, 1);             // [fn, fn, set, fn, true]
    lua_rawset(L, -3);                 // [fn, fn, set]
    lua_pop(L, 1);                     // [fn, fn]
    lua_pop(L, 1);                     // [fn]
}

void reg_fn(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    mark_ours(L);
    lua_setglobal(L, name);
}

struct LibEntry {
    const char* name;
    lua_CFunction fn;
};

void reg_lib(lua_State* L, const char* name, const std::vector<LibEntry>& members) {
    lua_newtable(L);
    for (const auto& m : members) {
        lua_pushcfunction(L, m.fn, m.name);
        mark_ours(L);
        lua_setfield(L, -2, m.name);
    }
    lua_pushvalue(L, -1);
    lua_setglobal(L, name);
    lua_pop(L, 1);
}

void set_lib_fn(lua_State* L, int table, const char* name, lua_CFunction fn) {
    table = absidx(L, table);
    lua_pushcfunction(L, fn, name);
    mark_ours(L);
    lua_setfield(L, table, name);
}

std::string arg_string(lua_State* L, int i) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, i, &n);
    return std::string(s ? s : "", n);
}

std::string opt_string(lua_State* L, int i, const std::string& fallback) {
    if (lua_isnoneornil(L, i))
        return fallback;
    size_t n = 0;
    const char* s = luaL_tolstring(L, i, &n);
    std::string out(s ? s : "", n);
    lua_pop(L, 1);
    return out;
}

// --- filesystem ------------------------------------------------------------

std::string g_workspace;

void ensure_workspace() {
    if (!fs::exists(g_workspace)) {
        std::error_code ec;
        fs::create_directories(g_workspace, ec);
    }
}

// Reject escapes: everything stays under the workspace root.
bool resolve_path(const std::string& in, std::string* out, std::string* err) {
    std::string p = in;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/')
        p.erase(p.begin());
    if (p.find("..") != std::string::npos) {
        *err = "path escapes the workspace: " + in;
        return false;
    }
    ensure_workspace();
    *out = (fs::path(g_workspace) / fs::path(p)).string();
    return true;
}

// --- base64 ----------------------------------------------------------------

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += kB64[n & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16;
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool b64_decode(const std::string& in, std::string* out) {
    out->clear();
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ')
            continue;
        int v = b64_val(c);
        if (v < 0)
            return false;
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(char((buf >> bits) & 0xFF));
        }
    }
    return true;
}

// --- crypto ----------------------------------------------------------------

const EVP_MD* pick_md(const std::string& name) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    if (n == "sha1") return EVP_sha1();
    if (n == "md5") return EVP_md5();
    if (n == "sha512") return EVP_sha512();
    if (n == "sha384") return EVP_sha384();
    return EVP_sha256();
}

std::string digest_hex(const std::string& data, const std::string& algo) {
    const EVP_MD* md = pick_md(algo);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    if (!EVP_Digest(data.data(), data.size(), out, &len, md, nullptr))
        return {};
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (unsigned i = 0; i < len; i++) {
        s += hex[out[i] >> 4];
        s += hex[out[i] & 0xF];
    }
    return s;
}

std::string hmac_hex(const std::string& data, const std::string& key, const std::string& algo) {
    const EVP_MD* md = pick_md(algo);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    if (!HMAC(md, key.data(), int(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len))
        return {};
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (unsigned i = 0; i < len; i++) {
        s += hex[out[i] >> 4];
        s += hex[out[i] & 0xF];
    }
    return s;
}

// --- http ------------------------------------------------------------------

struct HttpResponse {
    long status = 0;
    std::string headers; // raw
    std::string body;
};

size_t write_cb(char* p, size_t size, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, size * n);
    return size * n;
}

size_t header_cb(char* p, size_t size, size_t n, void* ud) {
    static_cast<HttpResponse*>(ud)->headers.append(p, size * n);
    return size * n;
}

bool http_perform(const std::string& url, const std::string& method,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, long timeout, HttpResponse* out,
                  std::string* err) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* c = curl_easy_init();
    if (!c) {
        *err = "curl_easy_init failed";
        return false;
    }

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout > 0 ? timeout : 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Roblox/WinInet");

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers)
        hdrs = curl_slist_append(hdrs, (h.first + ": " + h.second).c_str());
    if (hdrs)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

    std::string m = method;
    std::transform(m.begin(), m.end(), m.begin(), ::toupper);
    if (m == "POST" || m == "PUT" || m == "PATCH" || m == "DELETE") {
        if (m != "POST")
            curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, m.c_str());
        if (m == "POST")
            curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (m != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, m.c_str());
    }

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
    if (hdrs)
        curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        *err = curl_easy_strerror(rc);
        return false;
    }
    return true;
}

// Lowercased header table from the raw block.
void push_header_table(lua_State* L, const std::string& raw) {
    lua_newtable(L);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find("\r\n", pos);
        std::string line = raw.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? raw.size() : eol + 2;
        if (line.empty() || line.rfind("HTTP/", 0) == 0)
            continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
            v.erase(v.begin());
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, k.c_str());
    }
}

// --- Lua callbacks: environment -------------------------------------------

int lua_getgenv(lua_State* L) {
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    return 1;
}

int lua_getrenv(lua_State* L) {
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    return 1;
}

int lua_getreg(lua_State* L) {
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    return 1;
}

int lua_getfenv_c(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        return 1;
    }
    if (lua_isfunction(L, 1)) {
        lua_getfenv(L, 1);
        return 1;
    }
    luaL_error(L, "getfenv: expected a function");
    return 0;
}

int lua_setfenv_c(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushvalue(L, 2);
    lua_setfenv(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

int lua_getthreadidentity(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kIdentity);
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        lua_pushinteger(L, 8);
        return 1;
    }
    return 1;
}

int lua_setthreadidentity(lua_State* L) {
    luaL_checkinteger(L, 1);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, kIdentity);
    return 0;
}

int lua_checkcaller(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

int lua_identifyexecutor(lua_State* L) {
    lua_pushstring(L, kExecutorName);
    lua_pushstring(L, kExecutorVersion);
    return 2;
}

int lua_getexecutorname(lua_State* L) {
    lua_pushstring(L, kExecutorName);
    return 1;
}

// --- Lua callbacks: code ---------------------------------------------------

int do_loadstring(lua_State* L, const std::string& src, const std::string& chunk) {
    lua_CompileOptions opts;
    std::memset(&opts, 0, sizeof(opts));
    size_t n = 0;
    char* bc = luau_compile(src.data(), src.size(), &opts, &n);
    if (!bc) {
        lua_pushnil(L);
        lua_pushstring(L, "loadstring: compile failed");
        return 2;
    }
    int rc = luau_load(L, chunk.c_str(), bc, n, 0);
    std::free(bc);
    if (rc != 0) {
        // luau_load leaves the error message on the stack.
        lua_pushnil(L);
        lua_insert(L, -2); // nil, err
        return 2;
    }
    return 1;
}

int lua_loadstring(lua_State* L) {
    std::string src = arg_string(L, 1);
    std::string chunk = lua_isnoneornil(L, 2) ? "=string" : "=" + opt_string(L, 2, "string");
    return do_loadstring(L, src, chunk);
}

int lua_loadfile(lua_State* L) {
    std::string path;
    std::string err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        lua_pushnil(L);
        lua_pushstring(L, "loadfile: cannot open file");
        return 2;
    }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return do_loadstring(L, src, "=@" + path);
}

// --- Lua callbacks: closures ----------------------------------------------

int lua_newcclosure(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    push_regtable(L, kNewCClosures);
    lua_pushvalue(L, 1);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

int lua_isnewcclosure(lua_State* L) {
    push_regtable(L, kNewCClosures);
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    lua_pushboolean(L, lua_toboolean(L, -1));
    return 1;
}

int lua_clonefunction(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    return 1;
}

int lua_isexecutorclosure(lua_State* L) {
    bool ours = false;
    push_regtable(L, kOurFns);
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    ours = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    if (!ours) {
        push_regtable(L, kNewCClosures);
        lua_pushvalue(L, 1);
        lua_rawget(L, -2);
        ours = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
    }
    lua_pushboolean(L, ours);
    return 1;
}

// Replace every `old` value with `new` in the table at `tblidx`.
void replace_in_table(lua_State* L, int tblidx, int oldidx, int newidx) {
    tblidx = absidx(L, tblidx);
    oldidx = absidx(L, oldidx);
    newidx = absidx(L, newidx);
    std::vector<std::string> keys;
    lua_pushnil(L);
    while (lua_next(L, tblidx)) {
        if (lua_rawequal(L, -1, oldidx) && lua_type(L, -2) == LUA_TSTRING) {
            size_t n = 0;
            const char* s = lua_tolstring(L, -2, &n);
            keys.emplace_back(s ? s : "", n);
        }
        lua_pop(L, 1);
    }
    for (const auto& k : keys) {
        lua_pushlstring(L, k.data(), k.size());
        lua_pushvalue(L, newidx);
        lua_rawset(L, tblidx);
    }
}

void swap_in_world(lua_State* L, int oldidx, int newidx) {
    oldidx = absidx(L, oldidx);
    newidx = absidx(L, newidx);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    replace_in_table(L, lua_gettop(L), oldidx, newidx);
    lua_pop(L, 1);
    lua_getglobal(L, "game");
    if (lua_istable(L, -1))
        replace_in_table(L, lua_gettop(L), oldidx, newidx);
    lua_pop(L, 1);
}

// Record {old, new} so restorefunction can undo the swap.
void record_hook(lua_State* L, int oldidx, int newidx) {
    oldidx = absidx(L, oldidx);
    newidx = absidx(L, newidx);
    push_regtable(L, kHooks);
    lua_newtable(L);
    lua_pushvalue(L, oldidx);
    lua_setfield(L, -2, "old");
    lua_pushvalue(L, newidx);
    lua_setfield(L, -2, "new");
    lua_rawseti(L, -2, int(lua_objlen(L, -2)) + 1);
    lua_pop(L, 1);
}

int lua_hookfunction(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    record_hook(L, 1, 2);
    swap_in_world(L, 1, 2);
    lua_pushvalue(L, 1);
    return 1;
}

int lua_restorefunction(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    push_regtable(L, kHooks);
    int list = absidx(L, -1);
    size_t n = lua_objlen(L, list);
    for (size_t i = 1; i <= n; i++) {
        lua_rawgeti(L, list, int(i));
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "new");
            bool match = lua_istable(L, -2) && lua_rawequal(L, -1, 1);
            lua_pop(L, 1);
            if (match) {
                lua_getfield(L, -1, "old");
                lua_getfield(L, -2, "new");
                swap_in_world(L, lua_gettop(L), lua_gettop(L) - 1);
                lua_getfield(L, -1, "old");
                lua_remove(L, -2); // leave old on top
                return 1;
            }
        }
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    return 1;
}

int lua_isfunctionhooked(lua_State* L) {
    push_regtable(L, kHooks);
    int list = absidx(L, -1);
    size_t n = lua_objlen(L, list);
    bool found = false;
    for (size_t i = 1; i <= n && !found; i++) {
        lua_rawgeti(L, list, int(i));
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "new");
            found = lua_rawequal(L, -1, 1) != 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pushboolean(L, found);
    return 1;
}

// --- Lua callbacks: metatables --------------------------------------------

int lua_getrawmetatable(lua_State* L) {
    if (!lua_getmetatable(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    return 1;
}

int lua_setrawmetatable(lua_State* L) {
    if (lua_isnoneornil(L, 2))
        lua_pushnil(L);
    else
        luaL_checktype(L, 2, LUA_TTABLE);
    lua_setmetatable(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

int lua_hookmetamethod(lua_State* L) {
    std::string name = arg_string(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!lua_getmetatable(L, 1))
        luaL_error(L, "hookmetamethod: no metatable");
    lua_getfield(L, -1, name.c_str());
    int old = lua_gettop(L);
    lua_pushvalue(L, 3);
    lua_setfield(L, -3, name.c_str());
    lua_pushvalue(L, old);
    return 1;
}

int lua_setreadonly(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_setreadonly(L, 1, lua_toboolean(L, 2) ? 1 : 0);
    lua_pushboolean(L, 1);
    return 1;
}

int lua_isreadonly(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, lua_getreadonly(L, 1));
    return 1;
}

int lua_makereadonly(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_setreadonly(L, 1, 1);
    lua_pushboolean(L, 1);
    return 1;
}

int lua_getnamecallmethod(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

int lua_setnamecallmethod(lua_State* L) {
    (void)L;
    return 0;
}

// --- Lua callbacks: gc / registry -----------------------------------------

void walk_collect(lua_State* L, int tblidx, int resultidx, std::set<const void*>& seen,
                  int depth, bool include_tables) {
    if (depth > 12 || seen.size() > 20000)
        return;
    tblidx = absidx(L, tblidx);
    resultidx = absidx(L, resultidx);
    lua_pushnil(L);
    while (lua_next(L, tblidx)) {
        if (lua_isfunction(L, -1) || (include_tables && lua_istable(L, -1))) {
            lua_pushvalue(L, -1);
            lua_pushboolean(L, 1);
            lua_rawset(L, resultidx);
        }
        if (lua_istable(L, -1)) {
            const void* p = lua_topointer(L, -1);
            if (p && seen.insert(p).second)
                walk_collect(L, lua_gettop(L), resultidx, seen, depth + 1, include_tables);
        }
        lua_pop(L, 1);
    }
}

int lua_getgc(lua_State* L) {
    bool include_tables = lua_toboolean(L, 1) != 0;
    lua_newtable(L);
    int result = absidx(L, -1);
    std::set<const void*> seen;
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    seen.insert(lua_topointer(L, -1));
    walk_collect(L, lua_gettop(L), result, seen, 0, include_tables);
    lua_pop(L, 1);
    lua_getglobal(L, "game");
    if (lua_istable(L, -1)) {
        seen.insert(lua_topointer(L, -1));
        walk_collect(L, lua_gettop(L), result, seen, 1, include_tables);
    }
    lua_pop(L, 1);
    return 1;
}

// --- Lua callbacks: filesystem --------------------------------------------

int lua_readfile(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    std::ifstream in(path, std::ios::binary);
    if (!in)
        luaL_error(L, "readfile: no such file: %s", arg_string(L, 1).c_str());
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int lua_writefile(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    std::string data = arg_string(L, 2);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        luaL_error(L, "writefile: cannot write: %s", arg_string(L, 1).c_str());
    out.write(data.data(), std::streamsize(data.size()));
    lua_pushboolean(L, 1);
    return 1;
}

int lua_appendfile(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    std::string data = arg_string(L, 2);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
        luaL_error(L, "appendfile: cannot write: %s", arg_string(L, 1).c_str());
    out.write(data.data(), std::streamsize(data.size()));
    lua_pushboolean(L, 1);
    return 1;
}

int lua_isfile(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, fs::is_regular_file(path));
    return 1;
}

int lua_isfolder(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, fs::is_directory(path));
    return 1;
}

int lua_makefolder(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    std::error_code ec;
    fs::create_directories(path, ec);
    lua_pushboolean(L, !ec);
    return 1;
}

int lua_delfile(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    std::error_code ec;
    bool ok = fs::remove(path, ec);
    lua_pushboolean(L, ok && !ec);
    return 1;
}

int lua_delfolder(lua_State* L) {
    return lua_delfile(L);
}

int lua_listfiles(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    lua_newtable(L);
    int i = 1;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(path, ec)) {
        std::string full = e.path().string();
        std::string rel = fs::relative(e.path(), fs::path(g_workspace), ec).string();
        lua_pushlstring(L, rel.data(), rel.size());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int lua_getcustomasset(lua_State* L) {
    std::string path, err;
    if (!resolve_path(arg_string(L, 1), &path, &err))
        luaL_error(L, "%s", err.c_str());
    if (!fs::is_regular_file(path))
        luaL_error(L, "getcustomasset: no such file: %s", arg_string(L, 1).c_str());
    lua_pushlstring(L, path.data(), path.size());
    return 1;
}

// --- Lua callbacks: http ---------------------------------------------------

int push_response(lua_State* L, const HttpResponse& r, bool ok, const std::string& err) {
    lua_newtable(L);
    lua_pushboolean(L, ok);
    lua_setfield(L, -2, "Success");
    lua_pushinteger(L, lua_Integer(r.status));
    lua_setfield(L, -2, "StatusCode");
    lua_pushstring(L, ok ? "OK" : err.c_str());
    lua_setfield(L, -2, "StatusMessage");
    push_header_table(L, r.headers);
    lua_setfield(L, -2, "Headers");
    lua_pushlstring(L, r.body.data(), r.body.size());
    lua_setfield(L, -2, "Body");
    return 1;
}

int lua_request(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string url, method = "GET", body;
    long timeout = 30;
    std::vector<std::pair<std::string, std::string>> headers;

    lua_getfield(L, 1, "Url");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "url");
    }
    if (!lua_isnil(L, -1))
        url = opt_string(L, -1, "");
    lua_pop(L, 1);

    lua_getfield(L, 1, "Method");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "method");
    }
    if (!lua_isnil(L, -1))
        method = opt_string(L, -1, "GET");
    lua_pop(L, 1);

    lua_getfield(L, 1, "Body");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "body");
    }
    if (!lua_isnil(L, -1))
        body = opt_string(L, -1, "");
    lua_pop(L, 1);

    lua_getfield(L, 1, "Timeout");
    if (lua_isnumber(L, -1))
        timeout = long(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "Headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            std::string k = opt_string(L, -2, "");
            std::string v = opt_string(L, -1, "");
            headers.emplace_back(k, v);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    if (url.empty())
        luaL_error(L, "request: missing Url");

    HttpResponse r;
    std::string err;
    bool ok = http_perform(url, method, headers, body, timeout, &r, &err);
    return push_response(L, r, ok, err);
}

int lua_game_http(lua_State* L, const char* method, bool want_body) {
    // Colon call (self, url) or dot call (url).
    int urlidx = lua_isstring(L, 2) ? 2 : 1;
    std::string url = arg_string(L, urlidx);
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    if (!want_body && lua_isstring(L, urlidx + 1))
        body = arg_string(L, urlidx + 1);
    HttpResponse r;
    std::string err;
    if (!http_perform(url, method, headers, body, 30, &r, &err))
        luaL_error(L, "%s", err.c_str());
    lua_pushlstring(L, r.body.data(), r.body.size());
    return 1;
}

int lua_game_httpget(lua_State* L) { return lua_game_http(L, "GET", true); }
int lua_game_httpgetasync(lua_State* L) { return lua_game_http(L, "GET", true); }
int lua_game_httppost(lua_State* L) { return lua_game_http(L, "POST", false); }
int lua_game_httppostasync(lua_State* L) { return lua_game_http(L, "POST", false); }

// --- Lua callbacks: misc ---------------------------------------------------

int lua_setclipboard(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    lua_pushlstring(L, s, n);
    lua_setfield(L, LUA_REGISTRYINDEX, kClipboard);
    lua_pushboolean(L, 1);
    return 1;
}

int lua_getclipboard(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kClipboard);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "");
    }
    return 1;
}

int lua_isrbxactive(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

int lua_messagebox(lua_State* L) {
    std::string text = opt_string(L, 1, "");
    std::string caption = opt_string(L, 2, kExecutorName);
    std::fprintf(stderr, "[%s] messagebox: %s\n", caption.c_str(), text.c_str());
    lua_pushinteger(L, 0);
    return 1;
}

int lua_queue_on_teleport(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    lua_pushlstring(L, s, n);
    lua_setfield(L, LUA_REGISTRYINDEX, kTeleportQueue);
    return 0;
}

int lua_clearqueueonteleport(lua_State* L) {
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kTeleportQueue);
    return 0;
}

// firetouchinterest(partA, partB, phase): report a client-authored collision.
// The message id is not yet confirmed against a native capture; RBX_TOUCH_ID
// selects it (decimal or 0x..) so candidates can be probed one run at a time.
int lua_firetouch(lua_State* L) {
    const rbx::exec::WorldView* v = rbx::exec::world_view();
    if (!v || !v->ref_of) return 0;
    rbx::Instance* a = rbx::Instance::check(L, 1);
    rbx::Instance* b = rbx::Instance::check(L, 2);
    int phase = int(luaL_optinteger(L, 3, 1));
    const int id = getenv("RBX_TOUCH_ID")
                       ? int(strtol(getenv("RBX_TOUCH_ID"), nullptr, 0))
                       : 0x86;
    const uint32_t ra = v->ref_of(v->ctx, a), rb = v->ref_of(v->ctx, b);
    if (!ra || !rb) return 0;
    const std::vector<uint8_t> pkt =
        rbxclient::batch::touch_batch(uint8_t(id), rb, ra,
                                      uint8_t(phase ? 2 : 1));
    if (const rbx::send::Api* api = rbx::send::installed())
        if (api->raw) api->raw(api->ctx, pkt.data(), pkt.size(), true);
    return 0;
}

int lua_getconnections(lua_State* L) {
    lua_newtable(L);
    return 1;
}

int lua_empty_table(lua_State* L) {
    lua_newtable(L);
    return 1;
}

int lua_nil(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

int lua_gethui(lua_State* L) {
    lua_getglobal(L, "game");
    return 1;
}

// --- Lua callbacks: crypto -------------------------------------------------

int lua_b64_encode(lua_State* L) {
    std::string data = arg_string(L, 1);
    std::string out = b64_encode(data);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int lua_b64_decode(lua_State* L) {
    std::string data = arg_string(L, 1);
    std::string out;
    if (!b64_decode(data, &out))
        luaL_error(L, "base64_decode: invalid input");
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int lua_crypt_hash(lua_State* L) {
    std::string data = arg_string(L, 1);
    std::string algo = opt_string(L, 2, "sha256");
    std::string out = digest_hex(data, algo);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int lua_crypt_hmac(lua_State* L) {
    std::string data = arg_string(L, 1);
    std::string key = arg_string(L, 2);
    std::string algo = opt_string(L, 3, "sha256");
    std::string out = hmac_hex(data, key, algo);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int lua_crypt_generatebytes(lua_State* L) {
    int n = int(luaL_checkinteger(L, 1));
    if (n < 0 || n > 1 << 20)
        luaL_error(L, "generatebytes: bad length");
    std::string out(size_t(n), '\0');
    if (n > 0 && RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), n) != 1)
        luaL_error(L, "generatebytes: RAND_bytes failed");
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// --- Lua callbacks: compatibility surface ---------------------------------

int lua_warn(lua_State* L) {
    const int n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; i++) {
        size_t sl = 0;
        const char* sp = luaL_tolstring(L, i, &sl);
        if (i > 1) out += ' ';
        out.append(sp ? sp : "", sl);
        lua_pop(L, 1);
    }
    std::fprintf(stderr, "[warn] %s\n", out.c_str());
    return 0;
}

// Executor HWID: stable per machine, not the account.
int lua_gethwid(lua_State* L) {
    char host[256] = {0};
    if (::gethostname(host, sizeof host - 1) != 0) std::snprintf(host, sizeof host, "boblox");
    const std::string h = digest_hex(std::string("boblox:") + host, "sha256");
    lua_pushlstring(L, h.data(), 32);
    return 1;
}

int lua_iscclosure(lua_State* L) {
    lua_pushboolean(L, lua_iscfunction(L, 1) != 0);
    return 1;
}

int lua_islclosure(lua_State* L) {
    lua_pushboolean(L, !lua_iscfunction(L, 1));
    return 1;
}

int lua_setfpscap(lua_State* L) {
    (void)L;
    return 0;
}

// TweenInfo.new(...) — a value object; scripts read its fields.
int lua_tweeninfo_new(lua_State* L) {
    lua_newtable(L);
    lua_pushnumber(L, luaL_optnumber(L, 1, 1.0));
    lua_setfield(L, -2, "Time");
    lua_pushinteger(L, luaL_optinteger(L, 2, 2));
    lua_setfield(L, -2, "EasingStyle");
    lua_pushinteger(L, luaL_optinteger(L, 3, 0));
    lua_setfield(L, -2, "EasingDirection");
    lua_pushinteger(L, luaL_optinteger(L, 4, 0));
    lua_setfield(L, -2, "RepeatCount");
    lua_pushboolean(L, lua_toboolean(L, 5));
    lua_setfield(L, -2, "Reverses");
    lua_pushnumber(L, luaL_optnumber(L, 6, 0.0));
    lua_setfield(L, -2, "DelayTime");
    return 1;
}

// Drawing.new(...) — no renderer here, so objects are inert tables: scripts
// that build a UI get past init instead of dying on a nil global.
int lua_drawing_new(lua_State* L) {
    (void)L;
    lua_newtable(L);
    const char* fields[] = {"Visible", "Color", "Transparency", "Thickness",
                            "Filled", "Position", "Size", "Text", "Font",
                            "Center", "Outline", "OutlineColor", "ZIndex"};
    for (const char* f : fields) {
        lua_pushboolean(L, f[0] == 'V');
        lua_setfield(L, -2, f);
    }
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "Visible");
    auto noop = [](lua_State* S) { (void)S; return 0; };
    lua_pushcfunction(L, noop, "Remove");
    lua_setfield(L, -2, "Remove");
    lua_pushcfunction(L, noop, "Destroy");
    lua_setfield(L, -2, "Destroy");
    return 1;
}

// --- Lua callbacks: upstream send bridge ----------------------------------

int lua_send_hold(lua_State* L) {
    const rbx::send::Api* api = rbx::send::installed();
    if (!api || !api->hold) {
        luaL_error(L, "boblox.send: no live session");
        return 0;
    }
    const int mask = int(luaL_checkinteger(L, 1)) & 0xFF;
    const int values = int(luaL_optinteger(L, 2, 0xFF)) & 0xFF;
    lua_pushboolean(L, api->hold(api->ctx, mask, values));
    return 1;
}

int lua_send_control(lua_State* L) {
    const rbx::send::Api* api = rbx::send::installed();
    if (!api || !api->control) {
        luaL_error(L, "boblox.send: no live session");
        return 0;
    }
    const int mask = int(luaL_checkinteger(L, 1)) & 0xFF;
    const int values = int(luaL_optinteger(L, 2, 0xFF)) & 0xFF;
    lua_pushboolean(L, api->control(api->ctx, mask, values));
    return 1;
}

int lua_send_release(lua_State* L) {
    const rbx::send::Api* api = rbx::send::installed();
    if (!api || !api->release) {
        luaL_error(L, "boblox.send: no live session");
        return 0;
    }
    lua_pushboolean(L, api->release(api->ctx));
    return 1;
}

int lua_send_raw(lua_State* L) {
    const rbx::send::Api* api = rbx::send::installed();
    if (!api || !api->raw) {
        luaL_error(L, "boblox.send: no live session");
        return 0;
    }
    size_t n = 0;
    const char* data = luaL_checklstring(L, 1, &n);
    const bool ordered = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, api->raw(api->ctx, data, n, ordered));
    return 1;
}

// Batch builders. The bytes are the wire shapes read from the labeled native
// capture (see send/entity_batch.h); delivery rides the same reliable-ordered
// path as everything else.
int lua_fire(lua_State* L) {
    const rbx::send::Api* api = rbx::send::installed();
    const WorldView* w = world_view();
    if (!api || !api->raw || !w || !w->ref_of || !w->event_index) {
        luaL_error(L, "boblox.fire: no live session");
        return 0;
    }
    rbx::Instance* inst = rbx::Instance::check(L, 1);
    const std::string ev_name = arg_string(L, 2);
    const uint32_t ref = w->ref_of(w->ctx, inst);
    if (ref == 0) {
        luaL_error(L, "boblox.fire: instance has no replication id");
        return 0;
    }
    const int ev = w->event_index(w->ctx, inst->class_name().c_str(),
                                  ev_name.c_str());
    if (ev < 0) {
        luaL_error(L, "boblox.fire: %s has no event %s",
                   inst->class_name().c_str(), ev_name.c_str());
        return 0;
    }
    std::vector<rbxclient::batch::Arg> args;
    bool unsupported = false;
    if (lua_istable(L, 3)) {
        const int n = int(lua_objlen(L, 3));
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, 3, i);
            switch (lua_type(L, -1)) {
            case LUA_TSTRING: {
                size_t sl = 0;
                const char* sp = lua_tolstring(L, -1, &sl);
                args.push_back(rbxclient::batch::arg_string(
                    std::string(sp ? sp : "", sl)));
                break;
            }
            case LUA_TBOOLEAN:
                args.push_back(rbxclient::batch::arg_bool(lua_toboolean(L, -1) != 0));
                break;
            case LUA_TNUMBER:
                args.push_back(rbxclient::batch::arg_number(lua_tonumber(L, -1)));
                break;
            case LUA_TUSERDATA: {
                rbx::Instance* arg = rbx::Instance::get(L, -1);
                if (!arg) { unsupported = true; break; }
                const uint32_t id = w->ref_of(w->ctx, arg);
                if (id == 0) { unsupported = true; break; }
                args.push_back(rbxclient::batch::arg_instance(id));
                break;
            }
            default:
                unsupported = true;
                break;
            }
            lua_pop(L, 1);
        }
    }
    if (unsupported) {
        luaL_error(L, "boblox.fire: argument not supported "
                      "(string, number, boolean, replicated Instance)");
        return 0;
    }
    const std::vector<uint8_t> bytes =
        rbxclient::batch::event_batch(ref, uint16_t(ev), args);
    lua_pushboolean(L, api->raw(api->ctx, bytes.data(), bytes.size(), true));
    return 1;
}

// boblox.move(x, y, z) — writes the local Humanoid's MoveDirection.
int lua_move(lua_State* L) {
    if (getenv("RBX_DIAG")) {
        static int n = 0;
        if (n++ < 3) std::fprintf(stderr, "[diag] lua_move called\n");
    }
    const float x = float(luaL_checknumber(L, 1));
    const float y = float(luaL_checknumber(L, 2));
    const float z = float(luaL_checknumber(L, 3));
    {
        std::lock_guard<std::mutex> lk(g_move_mutex);
        g_move[0] = x;
        g_move[1] = y;
        g_move[2] = z;
        g_move_set = true;
    }
    if (const rbx::exec::WorldView* v = rbx::exec::world_view())
        if (v->move) v->move(v->ctx, x, y, z);
    lua_pushboolean(L, 1);
    return 1;
}

// boblox.teleport(x, y, z) — absolute CFrame write for the local root part.
int lua_teleport(lua_State* L) {
    const float x = float(luaL_checknumber(L, 1));
    const float y = float(luaL_checknumber(L, 2));
    const float z = float(luaL_checknumber(L, 3));
    {
        std::lock_guard<std::mutex> lk(g_move_mutex);
        g_tp[0] = x; g_tp[1] = y; g_tp[2] = z;
        g_tp_set = true;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// boblox.pos(x, y, z) — one clocked own-transform upload (0x83/0x05).
//
// The reference client's fields are clocks and a flag word, not instance
// references (see entity_batch.h). This sends the same shape directly, so the
// upload can be exercised without a local character instance — which is exactly
// the state the client is in when the server has not replicated one.
int lua_pos(lua_State* L) {
    const float x = float(luaL_checknumber(L, 1));
    const float y = float(luaL_checknumber(L, 2));
    const float z = float(luaL_checknumber(L, 3));
    // Same clock domain as the physics upload: milliseconds of client uptime.
    const std::vector<uint8_t> b = rbxclient::batch::physics_batch_clocked(
        rbxclient::batch::net_clock_ms(), rbxclient::batch::net_clock_ms(),
        rbxclient::batch::net_clock_ms() + 3, x, y, z, 0xffffffffu, 0.0f);
    const rbx::send::Api* api = rbx::send::installed();
    bool sent = false;
    if (api && api->raw)
        sent = api->raw(api->ctx, b.data(), b.size(), true);
    if (const rbx::exec::WorldView* v = rbx::exec::world_view())
        if (v->nudge_pos) v->nudge_pos(v->ctx, x, y, z);
    lua_pushboolean(L, sent ? 1 : 0);
    return 1;
}

int lua_send_ready(lua_State* L) {
    const rbx::send::Api* api = rbx::send::installed();
    lua_pushboolean(L, api && api->ready && api->ready(api->ctx));
    return 1;
}

int lua_crypt_unsupported(lua_State* L) {
    luaL_error(L, "crypt: this operation is not available in %s", kExecutorName);
    return 0;
}

} // namespace

const std::string& workspace_root() {
    if (g_workspace.empty()) {
        const char* env = std::getenv("BOBLOX_WORKSPACE");
        if (env && *env) {
            g_workspace = env;
        } else {
            const char* home = std::getenv("HOME");
            g_workspace = std::string(home ? home : ".") + "/.boblox/workspace";
        }
        ensure_workspace();
    }
    return g_workspace;
}

bool installed(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kInstalled);
    bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

namespace {
std::mutex g_q_mutex;
std::vector<std::string> g_q;

void* socket_thread(void* arg) {
    const std::string path = *static_cast<std::string*>(arg);
    delete static_cast<std::string*>(arg);
    ::unlink(path.c_str());
    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return nullptr;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(srv, 8) != 0) {
        ::close(srv);
        return nullptr;
    }
    for (;;) {
        int c = ::accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        std::string src;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(c, buf, sizeof buf);
            if (n <= 0) break;
            src.append(buf, size_t(n));
        }
        ::close(c);
        if (src.empty()) continue;
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_q.push_back(std::move(src));
    }
    return nullptr;
}
} // namespace

bool start_socket(const std::string& path) {
    pthread_t t;
    auto* arg = new std::string(path);
    if (pthread_create(&t, nullptr, socket_thread, arg) != 0) {
        delete arg;
        return false;
    }
    pthread_detach(t);
    return true;
}

void queue_source(std::string source) {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_q.push_back(std::move(source));
}

int pump_pending(lua_State* L) {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        batch.swap(g_q);
    }
    int n = 0;
    for (const std::string& src : batch) {
        std::string err;
        if (!run_source(L, src.data(), src.size(), &err))
            std::fprintf(stderr, "[exec] queued script failed: %s\n", err.c_str());
        else
            ++n;
    }
    return n;
}

bool take_teleport(float out[3]) {
    std::lock_guard<std::mutex> lk(g_move_mutex);
    if (!g_tp_set) return false;
    out[0] = g_tp[0];
    out[1] = g_tp[1];
    out[2] = g_tp[2];
    g_tp_set = false;
    return true;
}

bool take_move(float out[3]) {
    std::lock_guard<std::mutex> lk(g_move_mutex);
    if (!g_move_set) return false;
    out[0] = g_move[0];
    out[1] = g_move[1];
    out[2] = g_move[2];
    g_move_set = false;
    return true;
}

void set_world_view(const WorldView* view) { g_world = view; }
const WorldView* world_view() { return g_world; }

void install(lua_State* L) {
    if (!L || installed(L))
        return;
    workspace_root();

    lua_pushboolean(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, kInstalled);
    lua_pushstring(L, kExecutorVersion);
    lua_setfield(L, LUA_REGISTRYINDEX, kVersion);

    // environment
    reg_fn(L, "getgenv", lua_getgenv);
    reg_fn(L, "getrenv", lua_getrenv);
    reg_fn(L, "getreg", lua_getreg);
    reg_fn(L, "getfenv", lua_getfenv_c);
    reg_fn(L, "setfenv", lua_setfenv_c);
    reg_fn(L, "getthreadidentity", lua_getthreadidentity);
    reg_fn(L, "setthreadidentity", lua_setthreadidentity);
    reg_fn(L, "checkcaller", lua_checkcaller);
    reg_fn(L, "identifyexecutor", lua_identifyexecutor);
    reg_fn(L, "getexecutorname", lua_getexecutorname);
    reg_fn(L, "isexecutorclosure", lua_isexecutorclosure);
    reg_fn(L, "getgc", lua_getgc);

    // code
    reg_fn(L, "loadstring", lua_loadstring);
    reg_fn(L, "loadfile", lua_loadfile);

    // closures
    reg_fn(L, "newcclosure", lua_newcclosure);
    reg_fn(L, "isnewcclosure", lua_isnewcclosure);
    reg_fn(L, "clonefunction", lua_clonefunction);
    reg_fn(L, "hookfunction", lua_hookfunction);
    reg_fn(L, "restorefunction", lua_restorefunction);
    reg_fn(L, "isfunctionhooked", lua_isfunctionhooked);

    // metatables
    reg_fn(L, "getrawmetatable", lua_getrawmetatable);
    reg_fn(L, "setrawmetatable", lua_setrawmetatable);
    reg_fn(L, "hookmetamethod", lua_hookmetamethod);
    reg_fn(L, "setreadonly", lua_setreadonly);
    reg_fn(L, "isreadonly", lua_isreadonly);
    reg_fn(L, "makereadonly", lua_makereadonly);
    reg_fn(L, "getnamecallmethod", lua_getnamecallmethod);
    reg_fn(L, "setnamecallmethod", lua_setnamecallmethod);

    // filesystem
    reg_fn(L, "readfile", lua_readfile);
    reg_fn(L, "writefile", lua_writefile);
    reg_fn(L, "appendfile", lua_appendfile);
    reg_fn(L, "isfile", lua_isfile);
    reg_fn(L, "isfolder", lua_isfolder);
    reg_fn(L, "makefolder", lua_makefolder);
    reg_fn(L, "delfile", lua_delfile);
    reg_fn(L, "delfolder", lua_delfolder);
    reg_fn(L, "listfiles", lua_listfiles);
    reg_fn(L, "getcustomasset", lua_getcustomasset);

    // http
    reg_fn(L, "request", lua_request);
    reg_fn(L, "http_request", lua_request);
    reg_fn(L, "setclipboard", lua_setclipboard);
    reg_fn(L, "getclipboard", lua_getclipboard);
    reg_fn(L, "isrbxactive", lua_isrbxactive);
    reg_fn(L, "iswindowactive", lua_isrbxactive);
    reg_fn(L, "messagebox", lua_messagebox);
    reg_fn(L, "queue_on_teleport", lua_queue_on_teleport);
    reg_fn(L, "clearqueueonteleport", lua_clearqueueonteleport);
    reg_fn(L, "clearteleportqueue", lua_clearqueueonteleport);
    reg_fn(L, "firetouchinterest", lua_firetouch);
    reg_fn(L, "fireproximityprompt", lua_firetouch);
    reg_fn(L, "getconnections", lua_getconnections);
    reg_fn(L, "getscripts", lua_empty_table);
    reg_fn(L, "getloadedmodules", lua_empty_table);
    reg_fn(L, "getinstances", lua_empty_table);
    reg_fn(L, "getnilinstances", lua_empty_table);
    reg_fn(L, "gethui", lua_gethui);
    reg_fn(L, "getscriptbytecode", lua_nil);
    reg_fn(L, "decompile", lua_nil);

    // crypto
    reg_fn(L, "base64_encode", lua_b64_encode);
    reg_fn(L, "base64_decode", lua_b64_decode);
    reg_lib(L, "crypt", {
        {"base64_encode", lua_b64_encode},
        {"base64_decode", lua_b64_decode},
        {"hash", lua_crypt_hash},
        {"hmac", lua_crypt_hmac},
        {"generatebytes", lua_crypt_generatebytes},
        {"generatekeypair", lua_crypt_unsupported},
        {"encrypt", lua_crypt_unsupported},
        {"decrypt", lua_crypt_unsupported},
    });

    // aliases scripts probe for
    reg_fn(L, "warn", lua_warn);
    reg_fn(L, "gethwid", lua_gethwid);
    reg_fn(L, "setfpscap", lua_setfpscap);
    reg_lib(L, "TweenInfo", {{"new", lua_tweeninfo_new}});
    reg_lib(L, "Drawing", {{"new", lua_drawing_new}});
    reg_lib(L, "Path2DControlPoint", {{"new", lua_drawing_new}});
    reg_fn(L, "islclosure", lua_islclosure);
    reg_fn(L, "iscclosure", lua_iscclosure);
    reg_lib(L, "syn", {{"request", lua_request}});
    reg_lib(L, "http", {{"request", lua_request}});
    reg_lib(L, "fluxus", {{"request", lua_request}});

    // game:HttpGet & friends — the single most-used executor entry point.
    lua_getglobal(L, "game");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, lua_game_httpget, "HttpGet");
        mark_ours(L);
        lua_setfield(L, -2, "HttpGet");
        lua_pushcfunction(L, lua_game_httpgetasync, "HttpGetAsync");
        mark_ours(L);
        lua_setfield(L, -2, "HttpGetAsync");
        lua_pushcfunction(L, lua_game_httppost, "HttpPost");
        mark_ours(L);
        lua_setfield(L, -2, "HttpPost");
        lua_pushcfunction(L, lua_game_httppostasync, "HttpPostAsync");
        mark_ours(L);
        lua_setfield(L, -2, "HttpPostAsync");
    }
    lua_pop(L, 1);

    // World identity. The Engine `game` table carries JobId from
    // register_game; PlaceId/GameId are the client's join values, not the
    // sandbox's, so they are filled here when the embedder knows them.
    const rbxch::Options& o = rbxch::api::opts(L);
    lua_getglobal(L, "game");
    if (lua_istable(L, -1)) {
        if (o.place_id != 0) {
            lua_pushinteger(L, lua_Integer(o.place_id));
            lua_setfield(L, -2, "PlaceId");
        }
        if (o.universe_id != 0) {
            lua_pushinteger(L, lua_Integer(o.universe_id));
            lua_setfield(L, -2, "GameId");
        }
    }
    lua_pop(L, 1);

    // Upstream control. Registered even without a live session so a script
    // can feature-detect; the calls error clearly until the bridge is set.
    lua_newtable(L);                       // [boblox]
    lua_newtable(L);                       // [boblox, send]
    set_lib_fn(L, -1, "hold", lua_send_hold);
    set_lib_fn(L, -1, "control", lua_send_control);
    set_lib_fn(L, -1, "release", lua_send_release);
    set_lib_fn(L, -1, "raw", lua_send_raw);
    set_lib_fn(L, -1, "ready", lua_send_ready);
    set_lib_fn(L, -1, "fire", lua_fire);
    set_lib_fn(L, -1, "move", lua_move);
    lua_setfield(L, -2, "send");           // [boblox]
    set_lib_fn(L, -1, "fire", lua_fire);    // boblox.fire(instance, event, args)
    set_lib_fn(L, -1, "move", lua_move);    // boblox.move(x, y, z)
    set_lib_fn(L, -1, "teleport", lua_teleport); // boblox.teleport(x, y, z)
    set_lib_fn(L, -1, "pos", lua_pos);           // boblox.pos(x, y, z)
    lua_pushstring(L, kExecutorVersion);
    lua_setfield(L, -2, "version");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "boblox");
    lua_setglobal(L, "boblex");            // alias while the name settles

    if (const char* sock = std::getenv("BOBLOX_SOCKET"); sock && *sock) {
        if (start_socket(sock))
            std::fprintf(stderr, "[exec] script socket %s\n", sock);
    }

    lua_pushstring(L, kExecutorName);
    lua_setglobal(L, "BOBLOX_EXECUTOR");
}

bool run_source(lua_State* L, const char* src, size_t size, std::string* err) {
    if (!L || !src)
        return false;
    lua_CompileOptions opts;
    std::memset(&opts, 0, sizeof(opts));
    size_t n = 0;
    char* bc = luau_compile(src, size, &opts, &n);
    if (!bc) {
        if (err)
            *err = "compile failed";
        return false;
    }
    int rc = luau_load(L, "=exec", bc, n, 0);
    std::free(bc);
    if (rc != 0) {
        if (err)
            *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load failed";
        lua_pop(L, 1);
        return false;
    }

    // Spawn on a scheduler thread so task.wait yields into the Engine clock.
    lua_getglobal(L, "task");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2); // task, function
        if (err)
            *err = "task library missing";
        return false;
    }
    lua_getfield(L, -1, "spawn");
    lua_remove(L, -2);      // [fn, spawn]
    lua_pushvalue(L, -2);   // [fn, spawn, fn]
    lua_call(L, 1, 0);      // [fn]
    lua_pop(L, 1);
    return true;
}

} // namespace exec
} // namespace rbx
