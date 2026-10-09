// unit_engine.cpp — Phase J: the instance world (Engine profile only).
//
// Drives the persistent engine environment (engine.h) end-to-end:
//   * Instance.new / class defaults / IsA / the instance tree
//   * typed properties (Vector3, Bool), read-only ClassName, error paths
//   * GetPropertyChangedSignal (Connect / Disconnect / Once) + Wait()
//   * the task scheduler on the virtual clock (spawn/wait/defer/delay)
//   * workspace identity (global alias == game:GetService)
//   * the Vector3 class
//   * profile isolation: the Challenge (0x9B solve) surface sees NONE of it.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "engine.h"
#include "runner.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h" // luau_compile (Luau.Compiler)

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<char> compile(const std::string& src) {
    lua_CompileOptions o;
    std::memset(&o, 0, sizeof(o)); // same as Luau's REPL defaults
    size_t out_size = 0;
    char* out = luau_compile(src.c_str(), src.size(), &o, &out_size);
    REQUIRE(out != nullptr);
    std::vector<char> bc(out, out + out_size);
    std::free(out);
    return bc;
}

// RAII wrapper over the persistent Engine-profile environment.
struct Env {
    rbxch::engine::Environment* e = nullptr;

    Env() {
        rbxch::Options o;
        o.profile = rbxch::Profile::Engine;
        e = rbxch::engine::create(o);
        REQUIRE(e != nullptr);
    }
    ~Env() { rbxch::engine::destroy(e); }

    // Run a chunk in the persistent state; REQUIRE it completes cleanly.
    void exec(const std::string& src) {
        std::vector<char> bc = compile(src);
        rbxch::Result r = rbxch::engine::execute(*e, bc.data(), bc.size());
        INFO("script: " << src << "\nerror: " << r.error);
        REQUIRE(r.status == rbxch::Status::Ok);
    }

    int step(double dt) { return rbxch::engine::step(*e, dt); }
    double now() { return rbxch::engine::now(*e); }
    lua_State* L() { return rbxch::engine::state(*e); }
};

double gnum(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

bool gbool(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

std::string gstr(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const char* s = lua_tostring(L, -1);
    std::string v = s ? s : "";
    lua_pop(L, 1);
    return v;
}

// Run `src` under the DEFAULT (Challenge) profile and return its u32 answer.
uint32_t run_challenge(const std::string& src) {
    std::vector<char> bc = compile(src);
    rbxch::Result r = rbxch::run(bc.data(), bc.size(), {}); // default = Challenge
    REQUIRE(r.status == rbxch::Status::Ok);
    return r.answer;
}

} // namespace

TEST_CASE("Instance.new seeds class defaults") {
    Env env;
    env.exec(R"(
        p = Instance.new("Part")
        cls = p.ClassName
        pname = p.Name
        anchored = p.Anchored
        posZero = p.Position == Vector3.zero
        sizeDefault = p.Size == Vector3.new(4, 1, 2)
        isInstance = p:IsA("Instance")
        isPart = p:IsA("Part")
        notWS = p:IsA("Workspace")
        badIsA = p:IsA("NoSuchClass")
        base = Instance.new("Instance")
        baseCls = base.ClassName
        badClass = pcall(function() Instance.new("NoSuchClass") end)
        strOk = #tostring(p) > 0
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "cls") == "Part");
    CHECK(gstr(L, "pname") == "Part");
    CHECK(gbool(L, "anchored") == false);
    CHECK(gbool(L, "posZero"));
    CHECK(gbool(L, "sizeDefault"));
    CHECK(gbool(L, "isInstance"));
    CHECK(gbool(L, "isPart"));
    CHECK(gbool(L, "notWS") == false);
    CHECK(gbool(L, "badIsA") == false);
    CHECK(gstr(L, "baseCls") == "Instance");
    CHECK(gbool(L, "badClass") == false); // pcall caught the error
    CHECK(gbool(L, "strOk"));
}

TEST_CASE("instance tree: parent, children, find, destroy") {
    Env env;
    env.exec(R"(
        local model = Instance.new("Part")
        local child = Instance.new("Part", model)
        child.Name = "Kid"
        count1 = #model:GetChildren()
        gotKid = model:FindFirstChild("Kid") == child
        noOther = model:FindFirstChild("Part") == nil
        byClass = model:FindFirstChildOfClass("Part") == child
        kidParent = child.Parent == model
        modelParentNil = model.Parent == nil
        -- reparenting into one's own descendant must fail
        cycleOk = pcall(function() model.Parent = child end)
        -- detach
        child.Parent = nil
        count2 = #model:GetChildren()
        kidParentNil = child.Parent == nil
        -- Destroy tears down the subtree
        local doomed = Instance.new("Part", model)
        doomed:Destroy()
        count3 = #model:GetChildren()
        doomedParentNil = doomed.Parent == nil
        -- a destroyed instance refuses Parent writes
        reParent = pcall(function() doomed.Parent = nil end)
        -- Destroy is idempotent
        doomed:Destroy()
        count4 = #model:GetChildren()
    )");
    lua_State* L = env.L();
    CHECK(gnum(L, "count1") == 1);
    CHECK(gbool(L, "gotKid"));
    CHECK(gbool(L, "noOther"));
    CHECK(gbool(L, "byClass"));
    CHECK(gbool(L, "kidParent"));
    CHECK(gbool(L, "modelParentNil"));
    CHECK(gbool(L, "cycleOk") == false); // cycle rejected (pcall caught it)
    CHECK(gnum(L, "count2") == 0);
    CHECK(gbool(L, "kidParentNil"));
    CHECK(gnum(L, "count3") == 0);
    CHECK(gbool(L, "doomedParentNil"));
    CHECK(gbool(L, "reParent") == false);
    CHECK(gnum(L, "count4") == 0);
}

TEST_CASE("typed properties: set/get, type errors, read-only ClassName") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        p.Position = Vector3.new(1, 2, 3)
        posOk = p.Position == Vector3.new(1, 2, 3)
        posX = p.Position.X
        p.Size = Vector3.new(9, 9, 9)
        sizeX = p.Size.X
        p.Anchored = true
        anchored = p.Anchored
        p.Name = "Renamed"
        nm = p.Name
        -- 2016-sourced surface (PartInstance.cpp prop_*): defaults + set/get
        defTrans = p.Transparency
        defRefl = p.Reflectance
        defElast = p.Elasticity
        defFric = p.Friction
        defCollide = p.CanCollide
        defLocked = p.Locked
        p.Transparency = 0.5
        p.Reflectance = 0.25
        p.Elasticity = 0.9
        p.Friction = 0.1
        p.CanCollide = false
        p.Locked = true
        gotTrans = p.Transparency
        gotRefl = p.Reflectance
        gotElast = p.Elasticity
        gotFric = p.Friction
        gotCollide = p.CanCollide
        gotLocked = p.Locked
        badTrans = pcall(function() p.Transparency = "x" end)
        readonlyCls = pcall(function() p.ClassName = "X" end)
        unknownGet = pcall(function() local _ = p.Bogus end)
        unknownSet = pcall(function() p.Bogus = 1 end)
        badType = pcall(function() p.Position = 5 end)
        badType2 = pcall(function() p.Anchored = Vector3.zero end)
        badType3 = pcall(function() p.Name = 42 end)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "posOk"));
    CHECK(gnum(L, "posX") == doctest::Approx(1.0));
    CHECK(gnum(L, "sizeX") == doctest::Approx(9.0));
    CHECK(gbool(L, "anchored"));
    CHECK(gstr(L, "nm") == "Renamed");
    CHECK(gnum(L, "defTrans") == doctest::Approx(0.0));
    CHECK(gnum(L, "defRefl") == doctest::Approx(0.0));
    CHECK(gnum(L, "defElast") == doctest::Approx(0.5));
    CHECK(gnum(L, "defFric") == doctest::Approx(0.3));
    CHECK(gbool(L, "defCollide"));
    CHECK(gbool(L, "defLocked") == false);
    CHECK(gnum(L, "gotTrans") == doctest::Approx(0.5));
    CHECK(gnum(L, "gotRefl") == doctest::Approx(0.25));
    CHECK(gnum(L, "gotElast") == doctest::Approx(0.9));
    CHECK(gnum(L, "gotFric") == doctest::Approx(0.1));
    CHECK(gbool(L, "gotCollide") == false);
    CHECK(gbool(L, "gotLocked"));
    CHECK(gbool(L, "badTrans") == false);
    CHECK(gbool(L, "readonlyCls") == false);
    CHECK(gbool(L, "unknownGet") == false);
    CHECK(gbool(L, "unknownSet") == false);
    CHECK(gbool(L, "badType") == false);
    CHECK(gbool(L, "badType2") == false);
    CHECK(gbool(L, "badType3") == false);
}

TEST_CASE("GetPropertyChangedSignal: Connect, Disconnect, Once") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        local log = {}
        local c = p:GetPropertyChangedSignal("Name")
            :Connect(function() table.insert(log, "n") end)
        connected0 = c.Connected
        p.Name = "A"
        fired1 = #log
        c:Disconnect()
        connected1 = c.Connected
        p.Name = "B"
        fired2 = #log
        local n = 0
        p:GetPropertyChangedSignal("Anchored"):Once(function() n = n + 1 end)
        p.Anchored = true
        p.Anchored = false
        onceN = n
        badSignal = pcall(function() p:GetPropertyChangedSignal("Nope") end)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "connected0"));
    CHECK(gnum(L, "fired1") == 1);
    CHECK(gbool(L, "connected1") == false);
    CHECK(gnum(L, "fired2") == 1); // disconnect actually took effect
    CHECK(gnum(L, "onceN") == 1);  // Once fired exactly once
    CHECK(gbool(L, "badSignal") == false);
}

TEST_CASE("signal Wait() resumes the thread at the next pump") {
    Env env;
    env.exec(R"(
        out = ""
        p = Instance.new("Part")   -- global: reused by the re-fire below
        task.spawn(function()
            p:GetPropertyChangedSignal("Name"):Wait()
            out = "resumed"
        end)
        before = out        -- spawn ran, Wait yielded: not yet resumed
        p.Name = "X"        -- Fire schedules the waiter (not yet pumped)
        mid = out
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "before") == "");
    CHECK(gstr(L, "mid") == "");
    int resumes = env.step(0.0); // pump the deferred waiter
    CHECK(resumes >= 1);
    CHECK(gstr(L, "out") == "resumed");

    // A second Fire must NOT re-resume the consumed waiter (that would try
    // to resume a finished thread: 0 resumes, no scheduler error).
    env.exec("p.Name = \"Y\"");
    CHECK(env.step(0.0) == 0);
    CHECK(gstr(L, "out") == "resumed");
}

TEST_CASE("task.wait resumes on the virtual clock") {
    Env env;
    env.exec(R"(
        out = ""
        task.spawn(function()
            out = out .. "a"
            waitRet = task.wait(1)
            out = out .. "b"
            task.wait(1)
            out = out .. "c"
        end)
        n0 = out
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "n0") == "a"); // spawn runs immediately
    CHECK(env.now() == doctest::Approx(0.0));

    CHECK(env.step(0.5) == 0); // nothing due yet
    CHECK(gstr(L, "out") == "a");
    CHECK(env.now() == doctest::Approx(0.5));

    CHECK(env.step(0.5) == 1); // now == 1: first wait resumes
    CHECK(gstr(L, "out") == "ab");
    CHECK(gnum(L, "waitRet") == doctest::Approx(1.0)); // wait returns its time

    CHECK(env.step(1.0) == 1); // second wait resumes at now == 2
    CHECK(gstr(L, "out") == "abc");
    CHECK(env.now() == doctest::Approx(2.0));
}

TEST_CASE("task.defer runs at the next pump; task.delay at now+t") {
    Env env;
    env.exec(R"(
        out = ""
        task.defer(function() out = out .. "d" end)
        beforeDefer = out     -- defer does NOT run inline
        task.delay(2, function() out = out .. "e" end)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "beforeDefer") == "");

    env.step(0.0); // the pump starts deferred tasks
    CHECK(gstr(L, "out") == "d");

    env.step(0.5);
    env.step(0.5);
    env.step(0.5);
    CHECK(gstr(L, "out") == "d"); // now == 1.5 < 2
    env.step(0.5);                // now == 2.0: delay due
    CHECK(gstr(L, "out") == "de");
    CHECK(env.now() == doctest::Approx(2.0));
}

TEST_CASE("workspace identity: global alias == GetService") {
    Env env;
    env.exec(R"(
        wsOk = workspace ~= nil
        same = workspace == game:GetService("workspace")
        wsCls = workspace.ClassName
        wsIsA = workspace:IsA("Instance") and workspace:IsA("Workspace")
        aliasCls = game:GetService("workspace").ClassName
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "wsOk"));
    CHECK(gbool(L, "same")); // __eq over the same underlying instance
    CHECK(gstr(L, "wsCls") == "Workspace");
    CHECK(gbool(L, "wsIsA"));
    CHECK(gstr(L, "aliasCls") == "Workspace");
}

TEST_CASE("Vector3 class") {
    Env env;
    env.exec(R"(
        local v = Vector3.new(1, 2, 3)
        xyz = v.X == 1 and v.y == 2 and v.z == 3
        mag = v.Magnitude
        dot = v:Dot(Vector3.new(1, 1, 1))
        eq = v == Vector3.new(1, 2, 3)
        neq = v == Vector3.zero
        zeroOk = Vector3.zero == Vector3.new(0, 0, 0)
        str = tostring(v)
        badUd = pcall(function() return v:Dot(5) end)
        readonlyComp = pcall(function() v.X = 5 end)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "xyz"));
    CHECK(gnum(L, "mag") == doctest::Approx(std::sqrt(14.0)));
    CHECK(gnum(L, "dot") == doctest::Approx(6.0));
    CHECK(gbool(L, "eq"));
    CHECK(gbool(L, "neq") == false);
    CHECK(gbool(L, "zeroOk"));
    CHECK(gstr(L, "str") == "1, 2, 3");
    CHECK(gbool(L, "badUd") == false);
    CHECK(gbool(L, "readonlyComp") == false); // components are read-only
}

TEST_CASE("typeof parity: every engine userdata reports its Roblox type") {
    // Luau's typeof() reads __type from the userdata metatable
    // (luaT_objtypenamestr) — instances always report "Instance" regardless
    // of class. type() does NOT consult __type: still "userdata".
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        t_part = typeof(p)
        t_base = typeof(Instance.new("Instance"))
        t_ws = typeof(workspace)
        t_v3 = typeof(Vector3.new(1, 2, 3))
        t_zero = typeof(Vector3.zero)
        t_sig = typeof(p:GetPropertyChangedSignal("Name"))
        t_conn = typeof(p:GetPropertyChangedSignal("Name"):Connect(function() end))
        ty_part = type(p)
        ty_v3 = type(Vector3.zero)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "t_part") == "Instance");
    CHECK(gstr(L, "t_base") == "Instance");
    CHECK(gstr(L, "t_ws") == "Instance");
    CHECK(gstr(L, "t_v3") == "Vector3");
    CHECK(gstr(L, "t_zero") == "Vector3");
    CHECK(gstr(L, "t_sig") == "RBXScriptSignal");
    CHECK(gstr(L, "t_conn") == "RBXScriptConnection");
    CHECK(gstr(L, "ty_part") == "userdata");
    CHECK(gstr(L, "ty_v3") == "userdata");
}

TEST_CASE("Challenge profile never sees the engine surface") {
    // The 0x9B solve path must stay byte-exact: none of the engine globals
    // exist under the Challenge profile (they read as nil, not as errors).
    CHECK(run_challenge(
              "return (Instance == nil and task == nil and workspace == nil and "
              "Vector3 == nil) and 1 or 0") == 1);
}

TEST_CASE("engine profile keeps the challenge surface") {
    // Engine ⊇ Challenge: the historical shims still work for gameplay code
    // (RunService is a service, not a global — same as the frozen profile).
    Env env;
    env.exec("engKeepsChallenge = Random ~= nil and game ~= nil and os ~= nil "
             "and game:GetService(\"RunService\") ~= nil");
    CHECK(gbool(env.L(), "engKeepsChallenge"));
}
