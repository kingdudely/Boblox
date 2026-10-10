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
#include "instance/scheduler.h" // last_error diagnostics (failing task threads)

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

TEST_CASE("generated registry: 936 dump classes with dump-faithful hierarchy") {
    // gen_instances.py output: every non-overridden dump class registers
    // (creatable or not), supers resolve, Name defaults to the class name,
    // Instance-typed props hold nil until assigned.
    Env env;
    env.exec(R"(
        local m = Instance.new("Model")
        mName = m.Name
        mChain = m:IsA("Model") and m:IsA("PVInstance") and m:IsA("Instance")
        mNotPart = m:IsA("Part")
        mPPNil = m.PrimaryPart == nil
        local kid = Instance.new("Part")
        m.PrimaryPart = kid
        mPPSet = m.PrimaryPart == kid
        local s = Instance.new("SpawnLocation")
        sName = s.Name
        sChain = s:IsA("SpawnLocation") and s:IsA("Part") and
                 s:IsA("FormFactorPart") and s:IsA("BasePart")
        sEnabled = s.Enabled
        s.Enabled = true
        sEnabled2 = s.Enabled
        local p0 = Instance.new("Part")
        partChain = p0:IsA("FormFactorPart") and p0:IsA("BasePart") and
                    p0:IsA("PVInstance")
        wsFail = pcall(Instance.new, "Workspace")
        okW, wsErr = pcall(Instance.new, "Workspace")
        dmFail = pcall(Instance.new, "DataModel")
        bpFail = pcall(Instance.new, "BasePart")
        unknownFail = pcall(Instance.new, "Nope")
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "mName") == "Model");
    CHECK(gbool(L, "mChain"));
    CHECK(gbool(L, "mNotPart") == false);
    CHECK(gbool(L, "mPPNil"));
    CHECK(gbool(L, "mPPSet"));
    CHECK(gstr(L, "sName") == "SpawnLocation");
    CHECK(gbool(L, "sChain"));
    CHECK(gbool(L, "sEnabled") == false);
    CHECK(gbool(L, "sEnabled2"));
    CHECK(gbool(L, "partChain")); // hand Part honors its dump superclass
    CHECK(gbool(L, "wsFail") == false);
    CHECK(gstr(L, "wsErr").find("Unable to create an Instance of type 'Workspace'") !=
          std::string::npos); // the native message, verbatim
    CHECK(gbool(L, "dmFail") == false);
    CHECK(gbool(L, "bpFail") == false);
    CHECK(gbool(L, "unknownFail") == false);
}

TEST_CASE("string-compatible datatypes round-trip (ContentId/BinaryString)") {
    // Generator maps ContentId/BinaryString/SharedString onto String:
    // ContentId is an asset-ID string; byte blobs are safe because Lua
    // strings (and our Variant) carry embedded NULs.
    Env env;
    env.exec(R"(
        local a = Instance.new("Animation")
        aid0 = a.AnimationId
        a.AnimationId = "rbxassetid://123"
        aid1 = a.AnimationId
        local e = Instance.new("AudioEmitter")
        att0 = e.AngleAttenuation
        e.AngleAttenuation = "\0\1\2abc"
        attLen = #e.AngleAttenuation
        attRound = e.AngleAttenuation == "\0\1\2abc"
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "aid0") == "");
    CHECK(gstr(L, "aid1") == "rbxassetid://123");
    CHECK(gstr(L, "att0") == "");
    CHECK(gnum(L, "attLen") == 6);
    CHECK(gbool(L, "attRound"));
}

TEST_CASE("datatypes: Color3") {
    Env env;
    env.exec(R"(
        local c = Color3.new(0.5, 0.25, 1)
        cr, cg, cb = c.R, c.G, c.B
        rgb = Color3.fromRGB(255, 128, 0)
        rgbR, rgbG = rgb.R, rgb.G
        hsv = Color3.fromHSV(0, 1, 1)
        hsvOk = hsv == Color3.new(1, 0, 0)
        local h, s, v = Color3.fromHSV(0.5, 0.5, 0.5):toHSV()
        hsvRound = math.abs(h - 0.5) < 1e-9 and math.abs(s - 0.5) < 1e-9
                   and math.abs(v - 0.5) < 1e-9
        tColor = typeof(c)
        strC = tostring(Color3.new(1, 0, 0))
        eqC = Color3.new(1, 2, 3) == Color3.new(1, 2, 3)
        neC = Color3.new(1, 2, 3) == Color3.zero
        badC = pcall(function() return Color3.new("x") end)
    )");
    lua_State* L = env.L();
    CHECK(gnum(L, "cr") == doctest::Approx(0.5));
    CHECK(gnum(L, "cg") == doctest::Approx(0.25));
    CHECK(gnum(L, "cb") == doctest::Approx(1.0));
    CHECK(gnum(L, "rgbR") == doctest::Approx(1.0));
    CHECK(gnum(L, "rgbG") == doctest::Approx(128.0 / 255.0));
    CHECK(gbool(L, "hsvOk"));
    CHECK(gbool(L, "hsvRound"));
    CHECK(gstr(L, "tColor") == "Color3");
    CHECK(gstr(L, "strC") == "1, 0, 0");
    CHECK(gbool(L, "eqC"));
    CHECK(gbool(L, "neC") == false);
    CHECK(gbool(L, "badC") == false);
}

TEST_CASE("datatypes: CFrame") {
    Env env;
    env.exec(R"(
        tCF = typeof(CFrame.new())
        idOk = CFrame.new() == CFrame.identity
        posOk = CFrame.new(1, 2, 3).Position == Vector3.new(1, 2, 3)
        xyzOk = CFrame.new(4, 5, 6).X == 4
        quatOk = CFrame.new(1, 2, 3, 0, 0, 0, 1):FuzzyEq(CFrame.new(1, 2, 3))
        local a, b, c, d, e, f, g, h, i, j, k, l =
            CFrame.new(1, 2, 3):GetComponents()
        compOk = a == 1 and b == 2 and c == 3 and d == 1 and h == 1 and l == 1
        mulOk = CFrame.new(1, 0, 0) * CFrame.new(0, 2, 0) == CFrame.new(1, 2, 0)
        mulV = CFrame.new(1, 0, 0) * Vector3.new(0, 2, 0)
        mulVOk = mulV == Vector3.new(1, 2, 0)
        addOk = (CFrame.new(1, 1, 1) + Vector3.new(1, 0, 0)).X == 2
        subOk = (CFrame.new(1, 1, 1) - Vector3.new(0, 1, 0)).Y == 0
        local ci = CFrame.new(1, 2, 3)
        invOk = (ci * ci:Inverse()):FuzzyEq(CFrame.new())
        lv = CFrame.lookAt(Vector3.new(0, 0, 0), Vector3.new(0, 0, -1)).LookVector
        lookOk = lv == Vector3.new(0, 0, -1)
        rvOk = CFrame.new().RightVector == Vector3.new(1, 0, 0)
        uvOk = CFrame.new().UpVector == Vector3.new(0, 1, 0)
        rotOk = CFrame.new(5, 6, 7).Rotation == CFrame.new()
        ptOk = CFrame.new(1, 0, 0):PointToWorldSpace(Vector3.new(0, 2, 0))
               == Vector3.new(1, 2, 0)
        fm = CFrame.fromMatrix(Vector3.new(1, 2, 3), Vector3.new(1, 0, 0),
                               Vector3.new(0, 1, 0), Vector3.new(0, 0, 1))
        fmOk = fm == CFrame.new(1, 2, 3)
        aa = CFrame.fromAxisAngle(Vector3.new(0, 1, 0), math.pi)
        aaOk = aa:FuzzyEq(CFrame.new(0, 0, 0, -1, 0, 0, 0, 1, 0, 0, 0, -1))
        orthOk = CFrame.new():Orthonormalize() == CFrame.new()
        badCF = pcall(function() return CFrame.new(1, 2) end)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tCF") == "CFrame");
    CHECK(gbool(L, "idOk"));
    CHECK(gbool(L, "posOk"));
    CHECK(gbool(L, "xyzOk"));
    CHECK(gbool(L, "quatOk"));
    CHECK(gbool(L, "compOk"));
    CHECK(gbool(L, "mulOk"));
    CHECK(gbool(L, "mulVOk"));
    CHECK(gbool(L, "addOk"));
    CHECK(gbool(L, "subOk"));
    CHECK(gbool(L, "invOk"));
    CHECK(gbool(L, "lookOk"));
    CHECK(gbool(L, "rvOk"));
    CHECK(gbool(L, "uvOk"));
    CHECK(gbool(L, "rotOk"));
    CHECK(gbool(L, "ptOk"));
    CHECK(gbool(L, "fmOk"));
    CHECK(gbool(L, "aaOk"));
    CHECK(gbool(L, "orthOk"));
    CHECK(gbool(L, "badCF") == false);
}

TEST_CASE("datatypes: Vector2, UDim, UDim2, Rect, NumberRange") {
    Env env;
    env.exec(R"(
        tV2 = typeof(Vector2.new(1, 2))
        v2Ok = Vector2.new(3, 4).X == 3 and Vector2.new(3, 4).Y == 4
        v2Mag = Vector2.new(3, 4).Magnitude
        tUD = typeof(UDim.new(0.5, 100))
        udOk = UDim.new(0.5, 100).Scale == 0.5 and UDim.new(0.5, 100).Offset == 100
        local u2 = UDim2.new(0.5, 100, 0.25, 50)
        tU2 = typeof(u2)
        u2Ok = u2.X.Scale == 0.5 and u2.Y.Offset == 50
        u2WH = u2.Width == 100 and u2.Height == 50
        u2FS = UDim2.fromScale(1, 2) == UDim2.new(1, 0, 2, 0)
        u2FO = UDim2.fromOffset(3, 4) == UDim2.new(0, 3, 0, 4)
        local r = Rect.new(1, 2, 5, 8)
        tRect = typeof(r)
        rectOk = r.Min == Vector2.new(1, 2) and r.Max == Vector2.new(5, 8)
        rectWH = r.Width == 4 and r.Height == 6
        local nr = NumberRange.new(2, 9)
        tNR = typeof(nr)
        nrOk = nr.Min == 2 and nr.Max == 9
        nrSingle = NumberRange.new(7)
        nrSingleOk = nrSingle.Min == 7 and nrSingle.Max == 7
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tV2") == "Vector2");
    CHECK(gbool(L, "v2Ok"));
    CHECK(gnum(L, "v2Mag") == doctest::Approx(5.0));
    CHECK(gstr(L, "tUD") == "UDim");
    CHECK(gbool(L, "udOk"));
    CHECK(gstr(L, "tU2") == "UDim2");
    CHECK(gbool(L, "u2Ok"));
    CHECK(gbool(L, "u2WH"));
    CHECK(gbool(L, "u2FS"));
    CHECK(gbool(L, "u2FO"));
    CHECK(gstr(L, "tRect") == "Rect");
    CHECK(gbool(L, "rectOk"));
    CHECK(gbool(L, "rectWH"));
    CHECK(gstr(L, "tNR") == "NumberRange");
    CHECK(gbool(L, "nrOk"));
    CHECK(gbool(L, "nrSingleOk"));
}

TEST_CASE("datatypes: BrickColor") {
    Env env;
    env.exec(R"(
        tBC = typeof(BrickColor.new(21))
        numOk = BrickColor.new(21).Number == 21
        nameOk = BrickColor.new(21).Name == "Bright red"
        strOk = tostring(BrickColor.new(21)) == "Bright red"
        byName = BrickColor.new("Bright red") == BrickColor.new(21)
        exactOk = BrickColor.new(242, 243, 243) == BrickColor.new("White")
        fromC = BrickColor.new(Color3.new(1, 1, 1))
        -- closest-match consistency: the Color3 and rgb paths agree
        fromCOk = fromC == BrickColor.new(255, 255, 255)
        colOk = BrickColor.new(21).Color == Color3.fromRGB(196, 40, 28)
        rgbOk = BrickColor.new(21).r == 196
        whiteOk = BrickColor.White().Number == 1
        redOk = BrickColor.Red() == BrickColor.new(21)
        randOk = typeof(BrickColor.random()) == "BrickColor"
        badName = pcall(function() return BrickColor.new("Nope") end)
        badNum = pcall(function() return BrickColor.new(99999) end)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tBC") == "BrickColor");
    CHECK(gbool(L, "numOk"));
    CHECK(gbool(L, "nameOk"));
    CHECK(gbool(L, "strOk"));
    CHECK(gbool(L, "byName"));
    CHECK(gbool(L, "exactOk"));
    CHECK(gbool(L, "fromCOk"));
    CHECK(gbool(L, "colOk"));
    CHECK(gbool(L, "rgbOk"));
    CHECK(gbool(L, "whiteOk"));
    CHECK(gbool(L, "redOk"));
    CHECK(gbool(L, "randOk"));
    CHECK(gbool(L, "badName") == false);
    CHECK(gbool(L, "badNum") == false);
}

TEST_CASE("datatypes: sequences + keypoints") {
    Env env;
    env.exec(R"(
        tNS = typeof(NumberSequence.new(5))
        n1 = NumberSequence.new(5)
        n1Ok = #n1.Keypoints == 1 and n1.Keypoints[1].Value == 5
        n2 = NumberSequence.new(1, 2)
        n2Ok = #n2.Keypoints == 2 and n2.Keypoints[2].Value == 2
               and n2.Keypoints[2].Time == 1
        local kps = {NumberSequenceKeypoint.new(0, 1, 0),
                     NumberSequenceKeypoint.new(1, 9, 0.5)}
        n3 = NumberSequence.new(kps)
        n3Ok = #n3.Keypoints == 2 and n3.Keypoints[2].Envelope == 0.5
        tNSK = typeof(NumberSequenceKeypoint.new(0, 0, 0))
        nsEq = NumberSequence.new(3) == NumberSequence.new(3)
        tCS = typeof(ColorSequence.new(Color3.new(1, 0, 0)))
        cs1 = ColorSequence.new(Color3.new(1, 0, 0))
        csOk = #cs1.Keypoints == 1 and cs1.Keypoints[1].Color == Color3.new(1, 0, 0)
        tCSK = typeof(ColorSequenceKeypoint.new(0, Color3.new(0, 0, 0), 0))
        badNS = pcall(function() return NumberSequence.new({1, 2}) end)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tNS") == "NumberSequence");
    CHECK(gbool(L, "n1Ok"));
    CHECK(gbool(L, "n2Ok"));
    CHECK(gbool(L, "n3Ok"));
    CHECK(gstr(L, "tNSK") == "NumberSequenceKeypoint");
    CHECK(gbool(L, "nsEq"));
    CHECK(gstr(L, "tCS") == "ColorSequence");
    CHECK(gbool(L, "csOk"));
    CHECK(gstr(L, "tCSK") == "ColorSequenceKeypoint");
    CHECK(gbool(L, "badNS") == false);
}

TEST_CASE("datatypes: Content, PhysicalProperties, Ray, Region3, DateTime") {
    Env env;
    env.exec(R"(
        tContent = typeof(Content.fromUri("x"))
        uriOk = Content.fromUri("rbxasset://a").Uri == "rbxasset://a"
        uriSrc = Content.fromUri("x").SourceType == 1
        aidOk = Content.fromAssetId(123).Uri == "rbxassetid://123"
        noneSrc = Content.none.SourceType == 0
        noneUri = Content.none.Uri == nil
        local p0 = Instance.new("Part")
        objOk = Content.fromObject(p0).Object == p0
        objSrc = Content.fromObject(p0).SourceType == 2
        tPP = typeof(PhysicalProperties.new(1, 2, 3))
        ppOk = PhysicalProperties.new(1, 2, 3).Density == 1
        ppW = PhysicalProperties.new(1, 2, 3, 4, 5)
        ppWOk = ppW.FrictionWeight == 4 and ppW.ElasticityWeight == 5
        local ray = Ray.new(Vector3.new(0, 0, 0), Vector3.new(1, 0, 0))
        tRay = typeof(ray)
        rayOk = ray.Origin == Vector3.new(0, 0, 0)
        distOk = ray:Distance(Vector3.new(5, 1, 0)) == 1
        cpOk = ray:ClosestPoint(Vector3.new(5, 1, 0)) == Vector3.new(5, 0, 0)
        local rg = Region3.new(Vector3.new(0, 0, 0), Vector3.new(4, 2, 0))
        tRg = typeof(rg)
        rgOk = rg.Size == Vector3.new(4, 2, 0)
        rgCFOk = rg.CFrame.Position == Vector3.new(2, 1, 0)
        local ex = Region3.new(Vector3.new(1, 1, 1),
                               Vector3.new(5, 5, 5)):ExpandToGrid(4)
        exOk = ex.Size == Vector3.new(8, 8, 8)
        tDT = typeof(DateTime.fromUnixTimestamp(0))
        dtOk = DateTime.fromUnixTimestamp(1700000000).UnixTimestamp == 1700000000
        dtMs = DateTime.fromUnixTimestampMillis(1700000000123).UnixTimestampMillis
               == 1700000000123
        isoOk = DateTime.fromUniversalTime(2024, 1, 2, 3, 4, 5, 6):ToIsoDate()
                == "2024-01-02T03:04:05.006Z"
        isoRound = DateTime.fromIsoDate("2024-01-02T03:04:05.006Z") ==
                   DateTime.fromUniversalTime(2024, 1, 2, 3, 4, 5, 6)
        uniTab = DateTime.fromUniversalTime(2024, 1, 2, 3, 4, 5, 6):ToUniversalTime()
        uniOk = uniTab.Year == 2024 and uniTab.Month == 1 and uniTab.Day == 2
                and uniTab.Hour == 3 and uniTab.Minute == 4 and uniTab.Second == 5
                and uniTab.Millisecond == 6
        nowOk = typeof(DateTime.now()) == "DateTime"
        badIso = pcall(function() return DateTime.fromIsoDate("nope") end)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tContent") == "Content");
    CHECK(gbool(L, "uriOk"));
    CHECK(gbool(L, "uriSrc"));
    CHECK(gbool(L, "aidOk"));
    CHECK(gbool(L, "noneSrc"));
    CHECK(gbool(L, "noneUri"));
    CHECK(gbool(L, "objOk"));
    CHECK(gbool(L, "objSrc"));
    CHECK(gstr(L, "tPP") == "PhysicalProperties");
    CHECK(gbool(L, "ppOk"));
    CHECK(gbool(L, "ppWOk"));
    CHECK(gstr(L, "tRay") == "Ray");
    CHECK(gbool(L, "rayOk"));
    CHECK(gbool(L, "distOk"));
    CHECK(gbool(L, "cpOk"));
    CHECK(gstr(L, "tRg") == "Region3");
    CHECK(gbool(L, "rgOk"));
    CHECK(gbool(L, "rgCFOk"));
    CHECK(gbool(L, "exOk"));
    CHECK(gstr(L, "tDT") == "DateTime");
    CHECK(gbool(L, "dtOk"));
    CHECK(gbool(L, "dtMs"));
    CHECK(gbool(L, "isoOk"));
    CHECK(gbool(L, "isoRound"));
    CHECK(gbool(L, "uniOk"));
    CHECK(gbool(L, "nowOk"));
    CHECK(gbool(L, "badIso") == false);
}

TEST_CASE("typed properties: Color3/CFrame props on Part") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        p.Color = Color3.new(1, 0, 0)
        colorOk = p.Color == Color3.new(1, 0, 0)
        p.CFrame = CFrame.new(1, 2, 3)
        cfOk = p.CFrame.Position == Vector3.new(1, 2, 3)
        badColor = pcall(function() p.Color = 5 end)
        badCF = pcall(function() p.CFrame = Vector3.zero end)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "colorOk"));
    CHECK(gbool(L, "cfOk"));
    CHECK(gbool(L, "badColor") == false);
    CHECK(gbool(L, "badCF") == false);
}

TEST_CASE("methods: generated stubs exist with legible errors") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        tBreak = typeof(p.BreakJoints)
        okStub, stubErr = pcall(function() return p:BreakJoints() end)
        tTouch = typeof(p.Touched)
        unknownMethod = pcall(function() return p.Bogus() end)
        unknownCall = pcall(function() return p.Bogus end)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tBreak") == "function");
    CHECK(gbool(L, "okStub") == false);
    CHECK(gstr(L, "stubErr").find("Part.BreakJoints is not implemented") != std::string::npos);
    CHECK(gstr(L, "tTouch") == "RBXScriptSignal");
    CHECK(gbool(L, "unknownMethod") == false);
    CHECK(gbool(L, "unknownCall") == false);
}

TEST_CASE("methods: tree navigation (ancestors, descendants, recursive find)") {
    Env env;
    env.exec(R"(
        local root = Instance.new("Part")
        root.Name = "Root"
        local mid = Instance.new("Part", root)
        mid.Name = "Mid"
        local leaf = Instance.new("Part", mid)
        leaf.Name = "Leaf"
        descN = #root:GetDescendants()
        recFind = root:FindFirstChild("Leaf", true) == leaf
        nonRec = root:FindFirstChild("Leaf") == nil
        isAnc = root:IsAncestorOf(leaf)
        notAnc = leaf:IsAncestorOf(root)
        isDesc = leaf:IsDescendantOf(root)
        selfAnc = root:IsAncestorOf(root)
        findAnc = leaf:FindFirstAncestor("Mid") == mid
        findAncCls = leaf:FindFirstAncestorOfClass("Part") == mid
        findAncIsA = leaf:FindFirstAncestorWhichIsA("Instance") == mid
        noAnc = leaf:FindFirstAncestor("Nobody") == nil
        fullName = leaf:GetFullName()
        root:ClearAllChildren()
        clearedN = #root:GetChildren()
        clearedDesc = #root:GetDescendants()
    )");
    lua_State* L = env.L();
    CHECK(gnum(L, "descN") == 2);
    CHECK(gbool(L, "recFind"));
    CHECK(gbool(L, "nonRec"));
    CHECK(gbool(L, "isAnc"));
    CHECK(gbool(L, "notAnc") == false);
    CHECK(gbool(L, "isDesc"));
    CHECK(gbool(L, "selfAnc") == false);
    CHECK(gbool(L, "findAnc"));
    CHECK(gbool(L, "findAncCls"));
    CHECK(gbool(L, "findAncIsA"));
    CHECK(gbool(L, "noAnc"));
    CHECK(gstr(L, "fullName") == "Root.Mid.Leaf");
    CHECK(gnum(L, "clearedN") == 0);
    CHECK(gnum(L, "clearedDesc") == 0);
}

TEST_CASE("names truncate at 100 chars (native behavior)") {
    Env env;
    env.exec(R"(
        local root = Instance.new("Part")
        local p = Instance.new("Part", root)
        p.Name = string.rep("x", 150)
        truncLen = #p.Name
        truncFind = root:FindFirstChild(string.rep("x", 100)) == p
        fullFindNil = root:FindFirstChild(string.rep("x", 150)) == nil
        shortOk = (function()
            p.Name = "Short"
            return p.Name == "Short" and root:FindFirstChild("Short") == p
        end)()
    )");
    lua_State* L = env.L();
    CHECK(gnum(L, "truncLen") == 100);
    CHECK(gbool(L, "truncFind"));
    CHECK(gbool(L, "fullFindNil"));
    CHECK(gbool(L, "shortOk"));
}

TEST_CASE("methods: Clone copies the subtree unparented") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        p.Name = "Orig"
        p.Anchored = true
        local kid = Instance.new("Part", p)
        kid.Name = "Kid"
        local c = p:Clone()
        cloneCls = c.ClassName
        cloneName = c.Name
        cloneAnch = c.Anchored
        cloneParentNil = c.Parent == nil
        cloneKids = #c:GetChildren()
        cloneKidName = c:GetChildren()[1].Name
        origIntact = #p:GetChildren() == 1 and p.Name == "Orig"
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "cloneCls") == "Part");
    CHECK(gstr(L, "cloneName") == "Orig");
    CHECK(gbool(L, "cloneAnch"));
    CHECK(gbool(L, "cloneParentNil"));
    CHECK(gnum(L, "cloneKids") == 1);
    CHECK(gstr(L, "cloneKidName") == "Kid");
    CHECK(gbool(L, "origIntact"));
}

TEST_CASE("methods: attributes (dynamic name/value store)") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        missing = p:GetAttribute("Nope") == nil
        p:SetAttribute("Speed", 42)
        p:SetAttribute("Title", "hi")
        p:SetAttribute("Flag", true)
        p:SetAttribute("Spot", Vector3.new(1, 2, 3))
        p:SetAttribute("Friend", p)
        speedOk = p:GetAttribute("Speed") == 42
        titleOk = p:GetAttribute("Title") == "hi"
        flagOk = p:GetAttribute("Flag") == true
        spotOk = p:GetAttribute("Spot") == Vector3.new(1, 2, 3)
        friendOk = p:GetAttribute("Friend") == p
        local names = p:GetAttributes()
        attrN = 0
        for _ in pairs(names) do attrN = attrN + 1 end
        fired = 0
        p:GetAttributeChangedSignal("Speed"):Connect(function() fired = fired + 1 end)
        p:SetAttribute("Speed", 43)
        firedOnce = fired
        p:SetAttribute("Gone", 1)
        p:SetAttribute("Gone", nil)
        goneOk = p:GetAttribute("Gone") == nil
        badAttr = pcall(function() p:SetAttribute("Fn", function() end) end)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "missing"));
    CHECK(gbool(L, "speedOk"));
    CHECK(gbool(L, "titleOk"));
    CHECK(gbool(L, "flagOk"));
    CHECK(gbool(L, "spotOk"));
    CHECK(gbool(L, "friendOk"));
    CHECK(gnum(L, "attrN") == 5);
    CHECK(gnum(L, "firedOnce") == 1);
    CHECK(gbool(L, "goneOk"));
    CHECK(gbool(L, "badAttr") == false);
}

TEST_CASE("methods: services are stable singletons (Engine only)") {
    Env env;
    env.exec(R"(
        samePlayers = game:GetService("Players") == game:GetService("Players")
        playersCls = game:GetService("Players").ClassName
        playersIsA = game:GetService("Players"):IsA("Instance")
        playersType = typeof(game:GetService("Players"))
        lightOk = game:GetService("Lighting").ClassName == "Lighting"
        wsSvc = game:GetService("workspace") == workspace
        -- RunService stays the hand table (frozen Challenge behavior)
        rsOk = game:GetService("RunService"):IsStudio() == false
        unknownFresh = game:GetService("Nope1") == game:GetService("Nope1")
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "samePlayers"));
    CHECK(gstr(L, "playersCls") == "Players");
    CHECK(gbool(L, "playersIsA"));
    CHECK(gstr(L, "playersType") == "Instance");
    CHECK(gbool(L, "lightOk"));
    CHECK(gbool(L, "wsSvc"));
    CHECK(gbool(L, "rsOk"));
    CHECK(gbool(L, "unknownFresh") == false); // fresh table per call, as frozen
}

TEST_CASE("methods: Humanoid TakeDamage fires Died") {
    Env env;
    env.exec(R"(
        local h = Instance.new("Humanoid")
        hCls = h.ClassName
        h.Health = 100
        h.MaxHealth = 100
        diedN = 0
        h.Died:Connect(function() diedN = diedN + 1 end)
        tDied = typeof(h.Died)
        h:TakeDamage(25)
        hp1 = h.Health
        diedAfter25 = diedN
        h:TakeDamage(200)
        hp2 = h.Health
        diedAfter200 = diedN
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "hCls") == "Humanoid");
    CHECK(gstr(L, "tDied") == "RBXScriptSignal");
    CHECK(gnum(L, "hp1") == doctest::Approx(75.0));
    CHECK(gnum(L, "diedAfter25") == 0);
    CHECK(gnum(L, "hp2") == doctest::Approx(-125.0));
    CHECK(gnum(L, "diedAfter200") == 1);
}

TEST_CASE("methods: WaitForChild (immediate, delayed, timeout)") {
    Env env;
    env.exec(R"(
        root = Instance.new("Part")
        local there = Instance.new("Part", root)
        there.Name = "There"
        immed = root:WaitForChild("There") == there
    )");
    CHECK(gbool(env.L(), "immed"));
    env.exec("task.spawn(function() spawnAlive = true end)");
    CHECK(gbool(env.L(), "spawnAlive"));
    env.exec(R"(
        task.spawn(function()
            immedSpawn = root:WaitForChild("There") ~= nil
        end)
    )");
    CHECK(gbool(env.L(), "immedSpawn"));
    env.exec(R"(
        gotLate = "no"
        task.spawn(function()
            local c = root:WaitForChild("Late", 10)
            gotLate = (c ~= nil and c.Name) or "nil"
        end)
        beforeStep = gotLate
        task.spawn(function()
            task.wait(0.1)
            local late = Instance.new("Part", root)
            late.Name = "Late"
        end)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "immed"));
    CHECK(gstr(L, "beforeStep") == "no"); // waiter yielded, adder deferred
    env.step(0.05); // only wake@0.03: Late absent
    CHECK(gstr(L, "gotLate") == "no");
    // Event-time drain: adder creates Late@0.1, the waiter polls it up@0.12 —
    // all inside this step (chained short sleeps keep exact sim time).
    env.step(0.1);
    CHECK(gstr(L, "gotLate") == "Late");
    env.exec(R"(
        task.spawn(function() -- WaitForChild must yield: needs a task thread
            local r2 = Instance.new("Part")
            miss = r2:WaitForChild("Ghost", 0.2) == nil
        end)
    )");
    env.step(0.3); // pass the timeout
    INFO("scheduler.last_error: " << rbx::scheduler(env.L())->last_error);
    CHECK(gbool(L, "miss"));
    env.exec(R"(
        okE, errE = pcall(function() return root:WaitForChild("") end)
        emptyFail = okE == false
        emptyMsg = string.find(errE, "empty child name") ~= nil
    )");
    CHECK(gbool(L, "emptyFail"));
    CHECK(gbool(L, "emptyMsg"));
}

TEST_CASE("enums: Enum global, items, GetEnumItems") {
    Env env;
    env.exec(R"(
        tEnum = typeof(Enum)
        tMat = typeof(Enum.Material)
        tItem = typeof(Enum.Material.Plastic)
        itemName = Enum.Material.Plastic.Name
        itemVal = Enum.Material.Plastic.Value
        items = Enum.Material:GetEnumItems()
        itemN = #items
        firstOk = items[1] == Enum.Material.Plastic
        eqSame = Enum.Material.Plastic == Enum.Material.Plastic
        eqDiff = Enum.Material.Plastic == Enum.Material.SmoothPlastic
        eqCross = Enum.Material.Plastic == Enum.PartType.Block
        unkEnum = Enum.Nope == nil
        unkItem = Enum.Material.Nope == nil
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tEnum") == "Enum");
    CHECK(gstr(L, "tMat") == "Enum");
    CHECK(gstr(L, "tItem") == "EnumItem");
    CHECK(gstr(L, "itemName") == "Plastic");
    CHECK(gnum(L, "itemVal") == 256);
    CHECK(gnum(L, "itemN") == 45);
    CHECK(gbool(L, "firstOk"));
    CHECK(gbool(L, "eqSame"));
    CHECK(gbool(L, "eqDiff") == false);
    CHECK(gbool(L, "eqCross") == false);
    CHECK(gbool(L, "unkEnum"));
    CHECK(gbool(L, "unkItem"));
}

TEST_CASE("enums: enum-typed properties (strict per-enum match)") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        shapeDef = p.Shape == Enum.PartType.Block
        p.Shape = Enum.PartType.Cylinder
        shapeSet = p.Shape == Enum.PartType.Cylinder
        shapeName = p.Shape.Name
        badEnum = pcall(function() p.Shape = Enum.SurfaceType.Smooth end)
        badType = pcall(function() p.Shape = 5 end)
        p:SetAttribute("Mode", Enum.Material.Neon)
        attrEnum = p:GetAttribute("Mode") == Enum.Material.Neon
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "shapeDef"));
    CHECK(gbool(L, "shapeSet"));
    CHECK(gstr(L, "shapeName") == "Cylinder");
    CHECK(gbool(L, "badEnum") == false);
    CHECK(gbool(L, "badType") == false);
    CHECK(gbool(L, "attrEnum"));
}

TEST_CASE("Challenge profile never sees the engine surface") {
    // The 0x9B solve path must stay byte-exact: none of the engine globals
    // exist under the Challenge profile (they read as nil, not as errors).
    CHECK(run_challenge(
              "return (Instance == nil and task == nil and workspace == nil and "
              "Vector3 == nil and Color3 == nil and CFrame == nil and "
              "Vector2 == nil and BrickColor == nil and UDim == nil and "
              "UDim2 == nil and Rect == nil and NumberRange == nil and "
              "NumberSequence == nil and NumberSequenceKeypoint == nil and "
              "ColorSequence == nil and ColorSequenceKeypoint == nil and "
              "Content == nil and PhysicalProperties == nil and Ray == nil and "
              "Region3 == nil and DateTime == nil and Enum == nil) and 1 or 0") == 1);
}

TEST_CASE("engine profile keeps the challenge surface") {
    // Engine ⊇ Challenge: the historical shims still work for gameplay code
    // (RunService is a service, not a global — same as the frozen profile).
    Env env;
    env.exec("engKeepsChallenge = Random ~= nil and game ~= nil and os ~= nil "
             "and game:GetService(\"RunService\") ~= nil");
    CHECK(gbool(env.L(), "engKeepsChallenge"));
}
