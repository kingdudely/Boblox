// unit_exec.cpp — the lean executor surface installed into a live Engine state.
//
// These are integration checks against the real scheduler-backed environment,
// not a UNC conformance suite: each case pins one facility a script actually
// uses (environment, code, metatables, filesystem, crypto, http plumbing).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "engine.h"
#include "exec/exec.h"
#include "send/bridge.h"

#include "lua.h"
#include "lualib.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

struct ExecEnv {
    rbxch::engine::Environment* e = nullptr;

    ExecEnv() {
        rbxch::Options o;
        o.profile = rbxch::Profile::Engine;
        o.job = "unit-exec-job";
        o.place_id = 1818;
        o.universe_id = 1686;
        e = rbxch::engine::create(o);
        REQUIRE(e != nullptr);
        rbx::exec::install(rbxch::engine::state(*e));
    }
    ~ExecEnv() { rbxch::engine::destroy(e); }

    lua_State* L() { return rbxch::engine::state(*e); }

    void run(const std::string& src) {
        std::string err;
        bool ok = rbx::exec::run_source(L(), src.data(), src.size(), &err);
        INFO("script: " << src << "\nerror: " << err);
        REQUIRE(ok);
    }

    std::string str(const char* name) {
        lua_getglobal(L(), name);
        const char* s = lua_tostring(L(), -1);
        std::string v = s ? s : "<nil>";
        lua_pop(L(), 1);
        return v;
    }

    bool boolean(const char* name) {
        lua_getglobal(L(), name);
        bool v = lua_toboolean(L(), -1) != 0;
        lua_pop(L(), 1);
        return v;
    }

    double number(const char* name) {
        lua_getglobal(L(), name);
        double v = lua_tonumber(L(), -1);
        lua_pop(L(), 1);
        return v;
    }
};

} // namespace

TEST_CASE("exec: world identity reaches game") {
    ExecEnv env;
    env.run("r_place = game.PlaceId\n"
            "r_game = game.GameId\n"
            "r_job = game.JobId\n");
    CHECK(env.number("r_place") == 1818.0);
    CHECK(env.number("r_game") == 1686.0);
    CHECK(env.str("r_job") == "unit-exec-job");
}

TEST_CASE("exec: environment surface is installed") {
    ExecEnv env;
    env.run("r_genv = type(getgenv()) == 'table'\n"
            "r_renv = type(getrenv()) == 'table'\n"
            "r_reg  = type(getreg()) == 'table'\n"
            "r_ident = identifyexecutor()\n"
            "r_name = getexecutorname()\n"
            "r_caller = checkcaller()\n"
            "r_ident8 = getthreadidentity() == 8\n");
    CHECK(env.boolean("r_genv"));
    CHECK(env.boolean("r_renv"));
    CHECK(env.boolean("r_reg"));
    CHECK(env.boolean("r_caller"));
    CHECK(env.boolean("r_ident8"));
    CHECK(env.str("r_ident") == "Boblex");
    CHECK(env.str("r_name") == "Boblex");
}

TEST_CASE("exec: loadstring compiles and returns a callable") {
    ExecEnv env;
    env.run("local f, e = loadstring('return 40 + 2')\n"
            "r_fn = type(f)\n"
            "r_val = f and f() or -1\n"
            "local bad, berr = loadstring('this is not lua')\n"
            "r_bad = bad == nil and type(berr) == 'string'\n");
    CHECK(env.str("r_fn") == "function");
    CHECK(env.number("r_val") == 42.0);
    CHECK(env.boolean("r_bad"));
}

TEST_CASE("exec: metatable helpers reach the real metatable") {
    ExecEnv env;
    env.run("local t = setmetatable({}, {__index = function() return 7 end})\n"
            "r_meta = type(getrawmetatable(t)) == 'table'\n"
            "r_raw = t.anything\n"
            "r_ro0 = isreadonly(t)\n");
    CHECK(env.boolean("r_meta"));
    CHECK(env.number("r_raw") == 7.0);
    CHECK(env.boolean("r_ro0") == false);
}

TEST_CASE("exec: crypt and base64 are real, not identity") {
    ExecEnv env;
    env.run("r_b64 = base64_encode('hello')\n"
            "r_b64rt = base64_decode(r_b64)\n"
            "r_sha = crypt.hash('abc', 'sha256')\n"
            "r_bytes = #crypt.generatebytes(16)\n");
    CHECK(env.str("r_b64") == "aGVsbG8=");
    CHECK(env.str("r_b64rt") == "hello");
    CHECK(env.str("r_sha") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(env.number("r_bytes") == 16.0);
}

TEST_CASE("exec: filesystem is sandboxed to the workspace root") {
    ExecEnv env;
    env.run("makefolder('unit_tmp')\n"
            "writefile('unit_tmp/a.txt', 'abc')\n"
            "r_file0 = isfile('unit_tmp/a.txt')\n"
            "r_read = readfile('unit_tmp/a.txt')\n"
            "r_dir = isfolder('unit_tmp')\n"
            "local ok = pcall(function() readfile('../escape') end)\n"
            "r_escape_blocked = ok == false\n");
    CHECK(env.boolean("r_file0"));
    CHECK(env.str("r_read") == "abc");
    CHECK(env.boolean("r_dir"));
    CHECK(env.boolean("r_escape_blocked"));

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::path(rbx::exec::workspace_root()) / "unit_tmp", ec);
}

TEST_CASE("exec: game carries the HttpGet entry points scripts expect") {
    ExecEnv env;
    env.run("r_get = type(game.HttpGet)\n"
            "r_getasync = type(game.HttpGetAsync)\n"
            "r_post = type(game.HttpPost)\n"
            "r_req = type(request)\n"
            "r_syn = type(syn.request)\n");
    CHECK(env.str("r_get") == "function");
    CHECK(env.str("r_getasync") == "function");
    CHECK(env.str("r_post") == "function");
    CHECK(env.str("r_req") == "function");
    CHECK(env.str("r_syn") == "function");
}

TEST_CASE("exec: a script that yields is driven by the Engine scheduler") {
    ExecEnv env;
    env.run("r_stage = 0\n"
            "task.spawn(function()\n"
            "  task.wait(0.5)\n"
            "  r_stage = 1\n"
            "end)\n");
    CHECK(env.number("r_stage") == 0.0);
    rbxch::engine::step(*env.e, 0.6);
    CHECK(env.number("r_stage") == 1.0);
}

TEST_CASE("exec: boblox.send dispatches through the installed bridge") {
    struct Fake {
        int holds = 0, controls = 0, releases = 0, raws = 0;
        bool ready = false;
        int last_mask = -1, last_values = -1;
        bool last_ordered = false;
    } fake;
    const rbx::send::Api api{
        &fake,
        [](void* c, int m, int v) {
            auto* f = static_cast<Fake*>(c);
            f->holds++; f->last_mask = m; f->last_values = v; return true;
        },
        [](void* c) { static_cast<Fake*>(c)->releases++; return true; },
        [](void* c, int m, int v) {
            auto* f = static_cast<Fake*>(c);
            f->controls++; f->last_mask = m; f->last_values = v; return true;
        },
        [](void* c, const void*, size_t n, bool ordered) {
            auto* f = static_cast<Fake*>(c);
            f->raws++; f->last_ordered = ordered; return n > 0;
        },
        [](void* c) { return static_cast<Fake*>(c)->ready; },
    };

    ExecEnv env;
    // No session: the calls must error, not silently succeed.
    env.run("r_nosession = pcall(function() boblox.send.hold(1) end) == false\n"
            "r_ready0 = boblox.send.ready()\n");
    CHECK(env.boolean("r_nosession"));
    CHECK_FALSE(env.boolean("r_ready0"));

    rbx::send::install(&api);
    REQUIRE(rbx::send::installed() == &api);
    env.run("r_h = boblox.send.hold(3, 255)\n"
            "r_c = boblox.send.control(4, 0)\n"
            "r_r = boblox.send.release()\n"
            "r_raw = boblox.send.raw('\\4\\7\\0\\0\\0', true)\n");
    rbx::send::install(nullptr);

    CHECK(env.boolean("r_h"));
    CHECK(env.boolean("r_c"));
    CHECK(env.boolean("r_r"));
    CHECK(env.boolean("r_raw"));
    CHECK(fake.holds == 1);
    CHECK(fake.controls == 1);
    CHECK(fake.releases == 1);
    CHECK(fake.raws == 1);
    CHECK(fake.last_ordered == true);
    CHECK(fake.last_mask == 4); // control() ran last with mask 4
    CHECK(fake.last_values == 0);
}

TEST_CASE("exec: install is idempotent") {
    ExecEnv env;
    rbx::exec::install(env.L()); // second install must not clobber getgenv
    env.run("r_ok = type(getgenv) == 'function' and type(loadstring) == 'function'\n");
    CHECK(env.boolean("r_ok"));
}
