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

#include "instance/audio_host.h"
#include "instance/audio_spatial.h"
#include "instance/class_registry.h"
#include "instance/contact_host.h"
#include "instance/host_input.h"
#include "instance/instance.h"
#include "instance/remote.h"
#include "instance/variant.h"
#include "solve/joindata.h"
#include "engine/live_runtime.h"
#include "solve/replicate.h"
#include "common/util.h"
#include "common/hex.h"
#include "view/mesh_asset.h"
#include "view/view.h"

#include <zstd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

struct FakeAudio final : rbx::audio::Backend {
    bool accept = true;
    bool playing = false;
    bool paused = false;
    int plays = 0;
    std::string uri;

    bool play(rbx::Instance*, const std::string& value, bool, double,
              std::string* why) override {
        ++plays;
        uri = value;
        if (!accept) {
            if (why) *why = "fake backend rejected the asset";
            playing = false;
            return false;
        }
        playing = true;
        paused = false;
        return true;
    }
    void pause(rbx::Instance*) override { paused = true; }
    void resume(rbx::Instance*) override { paused = false; }
    void stop(rbx::Instance*) override { playing = paused = false; }
    void forget(rbx::Instance*) override { playing = paused = false; }
    void set_volume(rbx::Instance*, double) override {}
    bool is_playing(rbx::Instance*) override { return playing && !paused; }
    void poll() override {}
};

double gnum(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    // int64 properties (UserId, NumPlayers) use lua_pushinteger64; the
    // narrow lua_tonumber path does not read that representation.
    double v = lua_isinteger64(L, -1) ? double(lua_tointeger64(L, -1, nullptr))
                                    : lua_tonumber(L, -1);
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

TEST_CASE("Instance.Changed exists on every instance and carries the name") {
    Env env;
    lua_State* L = env.L();
    env.exec(R"(
        local p = Instance.new("Part")
        count, seen = 0, nil
        p.Changed:Connect(function(name) count = count + 1; seen = name end)
        p.Name = "Renamed"
        fired = count
        got = seen
    )");
    CHECK(gnum(L, "fired") == doctest::Approx(1.0));
    CHECK(gstr(L, "got") == "Name");
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

TEST_CASE("replicated RemoteEvent arguments reach OnClientEvent typed") {
    Env env;
    lua_State* L = env.L();
    const rbx::ClassInfo* remote_cls = rbx::find_class("RemoteEvent");
    REQUIRE(remote_cls != nullptr);
    rbx::Ref<rbx::Instance> remote = rbx::instantiate(remote_cls);
    REQUIRE(remote.get() != nullptr);
    rbx::Instance::push(L, remote.get());
    lua_setglobal(L, "remote");
    env.exec(R"(
        remote.OnClientEvent:Connect(function(inst, s, n, b, v, tab)
            gotSelf = (inst == remote)
            gotS, gotN, gotB, gotX = s, n, b, v.X
            gotA, gotBKey = tab.a, tab.b
        end)
    )");

    rbxclient::JoinSchema schema;
    rbxclient::SchemaClass cls;
    cls.name = "RemoteEvent";
    rbxclient::SchemaEvent event;
    event.name = "OnClientEvent";
    cls.events.push_back(event);
    schema.classes.push_back(cls);

    rbxclient::ApplyReport rep;
    rep.root = remote;
    rep.instances.push_back(remote);
    rep.by_id[7] = 0;

    rbxclient::EventItem item;
    item.lo = 1;
    item.entity = 7;
    item.event = 0;
    item.cls_idx = 0;
    item.ev_idx = 0;

    rbxclient::EventArg self;
    self.tag = 28;
    self.value = {1, 7, 0, 0, 0}; // lo=1, entity id=7
    rbxclient::EventArg text;
    text.tag = 2;
    text.value = {5, 'h', 'e', 'l', 'l', 'o'};
    rbxclient::EventArg number;
    number.tag = 10; // zigzag 42
    number.value = {84};
    rbxclient::EventArg flag;
    flag.tag = 9;
    flag.value = {1};
    rbxclient::EventArg vec;
    vec.tag = 22;
    const float xyz[3] = {1.0f, 2.0f, 3.0f};
    vec.value.resize(12);
    std::memcpy(vec.value.data(), xyz, sizeof(xyz));
    rbxclient::EventArg table;
    table.tag = 31; // ValueTable: string-keyed
    table.keys = {"a", "b"};
    for (const char* v : {"x", "y"}) {
        rbxclient::EventArg el;
        el.tag = 2;
        el.value = {1, uint8_t(v[0])};
        table.elements.push_back(el);
    }
    item.args = {self, text, number, flag, vec, table};

    rbxclient::StreamStats st;
    CHECK(rbxclient::fireStreamEvent(rep, schema, item, L, st));
    CHECK(st.events_fired == 1);
    CHECK(gbool(L, "gotSelf"));
    CHECK(gstr(L, "gotS") == "hello");
    CHECK(gnum(L, "gotN") == doctest::Approx(42.0));
    CHECK(gbool(L, "gotB"));
    CHECK(gnum(L, "gotX") == doctest::Approx(1.0));
    CHECK(gstr(L, "gotA") == "x");
    CHECK(gstr(L, "gotBKey") == "y");
}

TEST_CASE("replicated RemoteEvent Wait returns the event arguments") {
    Env env;
    lua_State* L = env.L();
    const rbx::ClassInfo* remote_cls = rbx::find_class("RemoteEvent");
    REQUIRE(remote_cls != nullptr);
    rbx::Ref<rbx::Instance> remote = rbx::instantiate(remote_cls);
    REQUIRE(remote.get() != nullptr);
    rbx::Instance::push(L, remote.get());
    lua_setglobal(L, "remote");
    env.exec(R"(
        task.spawn(function()
            local a, b = remote.OnClientEvent:Wait()
            waitedA, waitedB = a, b
            waitDone = true
        end)
    )");
    CHECK_FALSE(gbool(L, "waitDone"));

    rbxclient::JoinSchema schema;
    rbxclient::SchemaClass cls;
    cls.name = "RemoteEvent";
    rbxclient::SchemaEvent event;
    event.name = "OnClientEvent";
    cls.events.push_back(event);
    schema.classes.push_back(cls);

    rbxclient::ApplyReport rep;
    rep.root = remote;
    rep.instances.push_back(remote);
    rep.by_id[9] = 0;
    rbxclient::EventItem item;
    item.lo = 1;
    item.entity = 9;
    item.event = 0;
    item.cls_idx = 0;
    item.ev_idx = 0;
    for (const char* text : {"left", "right"}) {
        rbxclient::EventArg a;
        a.tag = 2;
        a.value.push_back(uint8_t(std::strlen(text)));
        a.value.insert(a.value.end(), text, text + std::strlen(text));
        item.args.push_back(a);
    }
    rbxclient::StreamStats st;
    CHECK(rbxclient::fireStreamEvent(rep, schema, item, L, st));
    CHECK(env.step(0.0) >= 1);
    CHECK(gbool(L, "waitDone"));
    CHECK(gstr(L, "waitedA") == "left");
    CHECK(gstr(L, "waitedB") == "right");
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

TEST_CASE("service methods: core GUI, policy info, CollectionService tags, warn") {
    Env env;
    env.exec(R"(
        local sg = game:GetService("StarterGui")
        coreDefault = sg:GetCoreGuiEnabled(Enum.CoreGuiType.All)
        sg:SetCoreGuiEnabled(Enum.CoreGuiType.All, false)
        coreOff = sg:GetCoreGuiEnabled(Enum.CoreGuiType.All)

        local policy = game:GetService("PolicyService"):GetPolicyInfoForPlayerAsync(
            game:GetService("Players").LocalPlayer)
        policyOk = policy ~= nil
            and policy.ArePaidRandomItemsRestricted == false
            and policy.IsPaidItemTradingAllowed == true
            and type(policy.AllowedExternalLinkReferences) == "table"

        local cs = game:GetService("CollectionService")
        local part = Instance.new("Part")
        part.Name = "TaggedPart"
        part.Parent = workspace
        cs:AddTag(part, "Hazard")
        tagAdded = cs:HasTag(part, "Hazard")
        local list = cs:GetTagged("Hazard")
        taggedExact = #list == 1 and list[1] == part
        local tags = cs:GetTags(part)
        tagsExact = #tags == 1 and tags[1] == "Hazard"
        cs:RemoveTag(part, "Hazard")
        tagRemoved = not cs:HasTag(part, "Hazard")

        warnOk = type(warn) == "function"
        warn("unit-test warn", 7)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "coreDefault"));
    CHECK_FALSE(gbool(L, "coreOff"));
    CHECK(gbool(L, "policyOk"));
    CHECK(gbool(L, "tagAdded"));
    CHECK(gbool(L, "taggedExact"));
    CHECK(gbool(L, "tagsExact"));
    CHECK(gbool(L, "tagRemoved"));
    CHECK(gbool(L, "warnOk"));
}

TEST_CASE("CollectionService tag signals fire on AddTag/RemoveTag") {
    Env env;
    env.exec(R"(
        local cs = game:GetService("CollectionService")
        local part = Instance.new("Part")
        local added, removed
        cs:GetInstanceAddedSignal("Sensor"):connect(function(inst) added = inst end)
        cs:GetInstanceRemovedSignal("Sensor"):connect(function(inst) removed = inst end)
        cs:AddTag(part, "Sensor")
        addHit = added == part
        sameSignal = cs:GetInstanceAddedSignal("Sensor")
            == cs:GetInstanceAddedSignal("Sensor")
        cs:RemoveTag(part, "Sensor")
        removeHit = removed == part
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "addHit"));
    CHECK(gbool(L, "sameSignal"));
    CHECK(gbool(L, "removeHit"));
}

TEST_CASE("TweenService: Create/Play/Pause animates and completes") {
    Env env;
    env.exec(R"(
        P = Instance.new("Part")
        P.Transparency = 0
        local info = TweenInfo.new(1, Enum.EasingStyle.Linear, Enum.EasingDirection.Out)
        tiType = typeof(info)
        tiTime = info.Time
        tiStyle = info.EasingStyle == Enum.EasingStyle.Linear
        tiDefaults = TweenInfo.new().EasingStyle == Enum.EasingStyle.Quad
        local ts = game:GetService("TweenService")
        T = ts:Create(P, info, {Transparency = 1})
        stateBegin = T.PlaybackState == Enum.PlaybackState.Begin
        completed = false
        T.Completed:Connect(function() completed = true end)
        T:Play()
        statePlay = T.PlaybackState == Enum.PlaybackState.Playing
        badGoal = pcall(function() ts:Create(P, info, {Nope = 1}) end)
        badType = pcall(function() ts:Create(P, info, {Name = "x"}) end)
        T:Pause()
        statePause = T.PlaybackState == Enum.PlaybackState.Paused
        T:Play() -- restart from the paused value (0 here)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tiType") == "TweenInfo");
    CHECK(gnum(L, "tiTime") == doctest::Approx(1.0));
    CHECK(gbool(L, "tiStyle"));
    CHECK(gbool(L, "tiDefaults"));
    CHECK(gbool(L, "stateBegin"));
    CHECK(gbool(L, "statePlay"));
    CHECK(gbool(L, "statePause"));
    CHECK_FALSE(gbool(L, "badGoal"));
    CHECK_FALSE(gbool(L, "badType"));
    CHECK_FALSE(gbool(L, "completed"));

    env.step(0.5);
    env.exec("probe = P.Transparency");
    CHECK(gnum(L, "probe") == doctest::Approx(0.5));
    CHECK_FALSE(gbool(L, "completed"));

    env.step(0.5);
    env.exec("probe = P.Transparency\nprobeState = T.PlaybackState == Enum.PlaybackState.Completed");
    CHECK(gnum(L, "probe") == doctest::Approx(1.0));
    CHECK(gbool(L, "completed"));
    CHECK(gbool(L, "probeState"));
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
        sub = (v - Vector3.new(1, 1, 1)) == Vector3.new(0, 1, 2)
        add = (v + Vector3.new(1, 0, 0)) == Vector3.new(2, 2, 3)
        mulScalar = (v * 2) == Vector3.new(2, 4, 6)
        mulScalarLeft = (2 * v) == Vector3.new(2, 4, 6)
        mulVec = (v * Vector3.new(2, 2, 2)) == Vector3.new(2, 4, 6)
        divScalar = (v / 2) == Vector3.new(0.5, 1, 1.5)
        divVec = (v / Vector3.new(1, 2, 3)) == Vector3.new(1, 1, 1)
        neg = (-v) == Vector3.new(-1, -2, -3)
        cross = v:Cross(Vector3.new(0, 1, 0)) == Vector3.new(-3, 0, 1)
        unitOk = math.abs(v.Unit.Magnitude - 1) < 1e-9
        lerpOk = v:Lerp(Vector3.new(3, 4, 5), 0.5) == Vector3.new(2, 3, 4)
        fuzzyOk = v:FuzzyEq(Vector3.new(1, 2, 3))
        axisOk = Vector3.xAxis == Vector3.new(1, 0, 0)
               and Vector3.yAxis == Vector3.new(0, 1, 0)
               and Vector3.zAxis == Vector3.new(0, 0, 1)
               and Vector3.one == Vector3.new(1, 1, 1)
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
    CHECK(gbool(L, "sub"));
    CHECK(gbool(L, "add"));
    CHECK(gbool(L, "mulScalar"));
    CHECK(gbool(L, "mulScalarLeft"));
    CHECK(gbool(L, "mulVec"));
    CHECK(gbool(L, "divScalar"));
    CHECK(gbool(L, "divVec"));
    CHECK(gbool(L, "neg"));
    CHECK(gbool(L, "cross"));
    CHECK(gbool(L, "unitOk"));
    CHECK(gbool(L, "lerpOk"));
    CHECK(gbool(L, "fuzzyOk"));
    CHECK(gbool(L, "axisOk"));
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

TEST_CASE("generated registry: dump classes with dump-faithful hierarchy") {
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

TEST_CASE("view boxes carry Part color, transparency, and shape") {
    Env env;
    env.exec(R"(
        folder = Instance.new("Folder")
        local p = Instance.new("Part", folder)
        p.Color = Color3.new(1.0, 0.5, 0.25)
        p.Transparency = 0.25
        p.Shape = Enum.PartType.Ball
    )");
    lua_State* L = env.L();
    lua_getglobal(L, "folder");
    const rbx::Instance* folder = rbx::Instance::check(L, -1);
    REQUIRE(folder != nullptr);
    lua_pop(L, 1);
    const std::vector<rbxclient::ViewBox> boxes =
        rbxclient::collectBoxes(folder);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].r == doctest::Approx(1.0));
    CHECK(boxes[0].g == doctest::Approx(0.5));
    CHECK(boxes[0].b == doctest::Approx(0.25));
    CHECK(boxes[0].transparency == doctest::Approx(0.25));
    CHECK(boxes[0].shape == 0); // Enum.PartType.Ball
    // Defaults stay Block / opaque for an unconfigured part (Color is the
    // Part gray default, so only the shape/transparency are asserted).
    env.exec(R"(Instance.new("Part", folder))");
    const std::vector<rbxclient::ViewBox> all =
        rbxclient::collectBoxes(folder);
    REQUIRE(all.size() == 2);
    bool sawBlock = false;
    for (const auto& b : all) {
        if (b.shape == 1) {
            sawBlock = true;
            CHECK(b.transparency == doctest::Approx(0.0));
        }
    }
    CHECK(sawBlock);
}

TEST_CASE("native mesh v2 parser accepts the 36-byte vertex layout") {
    std::vector<uint8_t> bytes;
    const char magic[] = "version 2.00\n";
    bytes.insert(bytes.end(), magic, magic + sizeof(magic) - 1);
    auto u16 = [&](uint16_t value) {
        bytes.push_back(uint8_t(value));
        bytes.push_back(uint8_t(value >> 8));
    };
    auto u32 = [&](uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(uint8_t(value >> (8 * i)));
    };
    auto f32 = [&](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    };
    u16(12);      // header bytes
    u16(0x0c24);  // native format flags
    u32(3);       // vertices
    u32(1);       // faces
    for (int i = 0; i < 3; ++i) {
        f32(float(i));
        f32(float(i + 1));
        f32(float(i + 2));
        f32(0.0f);
        f32(0.0f);
        f32(1.0f);
        f32(float(i) * 0.5f);
        f32(0.25f);
        f32(0.0f); // native per-vertex padding
    }
    u32(0);
    u32(1);
    u32(2);

    rbxclient::MeshAsset mesh;
    std::string error;
    REQUIRE(rbxclient::parse_mesh(bytes.data(), bytes.size(), &mesh, &error));
    CHECK(mesh.valid);
    CHECK(mesh.vertices.size() == 3 * 8);
    CHECK(mesh.indices.size() == 3);
    CHECK(mesh.min[0] == doctest::Approx(0.0f));
    CHECK(mesh.max[0] == doctest::Approx(2.0f));
    CHECK(mesh.max[2] == doctest::Approx(4.0f));
    bytes[0] = 'V';
    CHECK_FALSE(rbxclient::parse_mesh(bytes.data(), bytes.size(), &mesh));
}

TEST_CASE("view boxes carry MeshPart and SpecialMesh geometry") {
    Env env;
    env.exec(R"(
        folder = Instance.new("Folder")
        meshPart = Instance.new("MeshPart", folder)
        meshPart.MeshId = "rbxasset://avatar/meshes/torso.mesh"
        meshPart.TextureID = "rbxasset://textures/face.png"
        meshPart.Size = Vector3.new(2, 2, 1)
        special = Instance.new("Part", folder)
        local sm = Instance.new("SpecialMesh", special)
        sm.MeshId = "rbxasset://avatar/heads/head.mesh"
        sm.TextureId = "rbxasset://textures/face.png"
        sm.Scale = Vector3.new(2, 3, 4)
        sm.Offset = Vector3.new(0.5, -0.5, 0)
        primitive = Instance.new("Part", folder)
        local sphere = Instance.new("SpecialMesh", primitive)
        sphere.MeshType = Enum.MeshType.Sphere
    )");
    lua_State* L = env.L();
    lua_getglobal(L, "folder");
    const rbx::Instance* folder = rbx::Instance::check(L, -1);
    REQUIRE(folder != nullptr);
    lua_pop(L, 1);
    const auto boxes = rbxclient::collectBoxes(folder);
    REQUIRE(boxes.size() == 3);
    bool sawMeshPart = false, sawSpecial = false, sawSphere = false;
    for (const auto& b : boxes) {
        if (b.mesh_mode == 1) {
            sawMeshPart = true;
            CHECK(b.mesh_uri == "rbxasset://avatar/meshes/torso.mesh");
            CHECK(b.texture_uri == "rbxasset://textures/face.png");
        } else if (b.mesh_mode == 2) {
            sawSpecial = true;
            CHECK(b.mesh_uri == "rbxasset://avatar/heads/head.mesh");
            CHECK(b.msx == doctest::Approx(2.0));
            CHECK(b.msy == doctest::Approx(3.0));
            CHECK(b.msz == doctest::Approx(4.0));
            CHECK(b.mox == doctest::Approx(0.5));
            CHECK(b.moy == doctest::Approx(-0.5));
        } else if (b.shape == 0) {
            sawSphere = true;
        }
    }
    CHECK(sawMeshPart);
    CHECK(sawSpecial);
    CHECK(sawSphere);
}

TEST_CASE("native mesh v5 and v6 containers decode") {
    auto put_u16 = [](std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(uint8_t(v));
        b.push_back(uint8_t(v >> 8));
    };
    auto put_u32 = [](std::vector<uint8_t>& b, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b.push_back(uint8_t(v >> (8 * i)));
    };
    auto put_vertex = [&](std::vector<uint8_t>& b, int i) {
        const float values[10] = {float(i), float(i + 1), 2.0f, 0, 0, 1,
                                  float(i) * 0.5f, 0.75f, 0, 0};
        for (float value : values) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            put_u32(b, bits);
        }
        (void)b;
    };

    // v5: 32-byte header, 40-byte vertices, no skin envelopes.
    std::vector<uint8_t> v5;
    const char v5magic[] = "version 5.00\n";
    v5.insert(v5.end(), v5magic, v5magic + sizeof(v5magic) - 1);
    put_u16(v5, 32);
    put_u16(v5, 0);
    put_u32(v5, 3);
    put_u32(v5, 1);
    put_u16(v5, 1);
    put_u16(v5, 0);
    put_u32(v5, 0);
    put_u16(v5, 0);
    v5.push_back(0);
    v5.push_back(0);
    put_u32(v5, 0);
    put_u32(v5, 0);
    for (int i = 0; i < 3; ++i)
        put_vertex(v5, i);
    put_u32(v5, 0);
    put_u32(v5, 1);
    put_u32(v5, 2);
    rbxclient::MeshAsset mesh;
    REQUIRE(rbxclient::parse_mesh(v5.data(), v5.size(), &mesh));
    CHECK(mesh.indices.size() == 3);

    // v6: COREMESH chunk version 1, then u32 count, vertices, u32 faces.
    std::vector<uint8_t> v6;
    const char v6magic[] = "version 6.00\n";
    v6.insert(v6.end(), v6magic, v6magic + sizeof(v6magic) - 1);
    v6.insert(v6.end(), {'C', 'O', 'R', 'E', 'M', 'E', 'S', 'H'});
    put_u32(v6, 1);
    put_u32(v6, 0);
    put_u32(v6, 3);
    for (int i = 0; i < 3; ++i)
        put_vertex(v6, i);
    put_u32(v6, 1);
    put_u32(v6, 0);
    put_u32(v6, 1);
    put_u32(v6, 2);
    REQUIRE(rbxclient::parse_mesh(v6.data(), v6.size(), &mesh));
    CHECK(mesh.vertices.size() == 24);
    CHECK(mesh.indices.size() == 3);
}

TEST_CASE("native mesh v7 Draco container decodes") {
    const fs::path path = fs::path(REPO_ROOT) / "tests/data/mesh_v7_draco.mesh";
    REQUIRE(fs::exists(path));
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
    REQUIRE(bytes.size() > 33);
    rbxclient::MeshAsset mesh;
    std::string error;
    REQUIRE(rbxclient::parse_mesh(bytes.data(), bytes.size(), &mesh, &error));
    CHECK(mesh.valid);
    CHECK(mesh.indices.size() > 300);
    CHECK(mesh.indices.size() % 3 == 0);
    CHECK(mesh.vertices.size() == mesh.indices.size() * 8);
    CHECK(mesh.max[0] > mesh.min[0]);
    CHECK(mesh.max[1] > mesh.min[1]);
    CHECK(mesh.max[2] > mesh.min[2]);
}

TEST_CASE("methods: generated stubs return type-appropriate defaults") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        tBreak = typeof(p.BreakJoints)
        okStub, retCount = pcall(function() return select('#', p:BreakJoints()) end)
        okMass, mass = pcall(function() return p:GetMass() end)
        massType = typeof(mass)
        tTouch = typeof(p.Touched)
        unknownMethod = pcall(function() return p.Bogus() end)
        unknownCall = pcall(function() return p.Bogus end)
        local function massFor(shape)
            local q = Instance.new('Part')
            q.Size = Vector3.new(4, 1, 2)
            q.Shape = Enum.PartType[shape]
            return q:GetMass()
        end
        massBall = massFor('Ball')
        massBlock = massFor('Block')
        massCylinder = massFor('Cylinder')
        massWedge = massFor('Wedge')
        massCornerWedge = massFor('CornerWedge')
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "tBreak") == "function");
    CHECK(gbool(L, "okStub"));
    CHECK(gnum(L, "retCount") == 0); // Void
    CHECK(gbool(L, "okMass"));
    CHECK(gstr(L, "massType") == "number");
    // GetMass uses the default 0.7 density and native primitive volume,
    // rounded through float like the native API.
    CHECK(gnum(L, "mass") == doctest::Approx(5.599999904632568));
    // Native 1818 probe for the same 4x1x2 size and 0.7 density:
    // Ball=0.366519, Block=5.6, Cylinder=2.199115, Wedge=2.8,
    // CornerWedge=1.866667.
    CHECK(gnum(L, "massBall") == doctest::Approx(0.36651915311813354));
    CHECK(gnum(L, "massBlock") == doctest::Approx(5.599999904632568));
    CHECK(gnum(L, "massCylinder") == doctest::Approx(2.1991147994995117));
    CHECK(gnum(L, "massWedge") == doctest::Approx(2.799999952316284));
    CHECK(gnum(L, "massCornerWedge") == doctest::Approx(1.8666666746139526));
    CHECK(gstr(L, "tTouch") == "RBXScriptSignal");
    CHECK(gbool(L, "unknownMethod") == false);
    CHECK(gbool(L, "unknownCall") == false);
}

TEST_CASE("methods: generic property getters/setters on stubs") {
    Env env;
    env.exec(R"(
        local a = Instance.new("Attachment")
        a.Axis = Vector3.new(1, 2, 3)
        okGet, got = pcall(function() return a:GetAxis() end)
        axisGet = got == Vector3.new(1, 2, 3)
        okSet = pcall(function() a:SetAxis(Vector3.new(4, 5, 6)) end)
        axisSet = a.Axis == Vector3.new(4, 5, 6)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "okGet"));
    CHECK(gbool(L, "axisGet"));
    CHECK(gbool(L, "okSet"));
    CHECK(gbool(L, "axisSet"));
}

TEST_CASE("LocalScript.AwaitPersistentModels prefers a real child") {
    Env env;
    env.exec(R"(
        local s = Instance.new("LocalScript")
        local child = Instance.new("ModuleScript", s)
        child.Name = "AwaitPersistentModels"
        isChild = s.AwaitPersistentModels == child
        local bare = Instance.new("LocalScript")
        fallbackType = typeof(bare.AwaitPersistentModels)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "isChild"));
    CHECK(gstr(L, "fallbackType") == "function");
}

TEST_CASE("globals: legacy wait/spawn/delay and clock aliases") {
    Env env;
    env.exec(R"(
        waitType = typeof(wait)
        spawnType = typeof(spawn)
        delayType = typeof(delay)
        tick0 = tick()
        time0 = time()
        elapsed0 = elapsedTime()
        task.spawn(function() task.wait(0.1); waitDone = true end)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "waitType") == "function");
    CHECK(gstr(L, "spawnType") == "function");
    CHECK(gstr(L, "delayType") == "function");
    CHECK(gnum(L, "tick0") >= 0.0);
    env.step(0.2);
    CHECK(gbool(L, "waitDone"));
    env.exec("tick1 = tick(); time1 = time(); elapsed1 = elapsedTime()");
    CHECK(gnum(L, "tick1") > gnum(L, "tick0"));
    CHECK(gnum(L, "time1") > gnum(L, "time0"));
    CHECK(gnum(L, "elapsed1") > gnum(L, "elapsed0"));
}

TEST_CASE("methods: property-modified/debug-id/reset and workspace math") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part")
        debugId = p:GetDebugId()
        modified0 = p:IsPropertyModified("Size")
        p.Size = Vector3.new(8, 8, 8)
        modified1 = p:IsPropertyModified("Size")
        p:ResetPropertyToDefault("Size")
        resetOk = p.Size == Vector3.new(4, 1, 2)
        fps = workspace:GetRealPhysicsFPS()
        awake = workspace:GetNumAwakeParts()
        jumpH = workspace:CalculateJumpHeight(196.2, 50)
        jumpD = workspace:CalculateJumpDistance(196.2, 50, 16)
        jumpP = workspace:CalculateJumpPower(196.2, jumpH)
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "debugId").find("Part@") == 0);
    CHECK(gbool(L, "modified0") == false);
    CHECK(gbool(L, "modified1"));
    CHECK(gbool(L, "resetOk"));
    CHECK(gnum(L, "fps") == doctest::Approx(60.0));
    CHECK(gnum(L, "awake") == doctest::Approx(0.0));
    CHECK(gnum(L, "jumpH") == doctest::Approx(50.0 * 50.0 / (2 * 196.2)));
    CHECK(gnum(L, "jumpD") == doctest::Approx(16.0 * 100.0 / 196.2));
    CHECK(gnum(L, "jumpP") == doctest::Approx(50.0));
}

TEST_CASE("registry: reference-only properties resolve") {
    Env env;
    env.exec(R"(
        local arc = Instance.new("ArcHandles")
        local hand = Instance.new("Handles")
        local part = Instance.new("Part")
        refAxesType = type(arc.Axes)
        refAxesX = arc.Axes.X
        refFacesType = type(hand.Faces)
        refFacesTop = hand.Faces.Top
        refResizeType = type(part.ResizeableFaces)
        refResizeFront = part.ResizeableFaces.Front
        local f = Faces.new(true, false, true)
        local a = Axes.new(true, false, true)
        faceConstruct = f.Top and not f.Bottom and f.Left
        axisConstruct = a.X and not a.Y and a.Z
        hand.Faces = f
        arc.Axes = a
        faceSet = hand.Faces.Top and not hand.Faces.Bottom
        axisSet = arc.Axes.X and arc.Axes.Z and not arc.Axes.Y
    )");
    lua_State* L = env.L();
    lua_getglobal(L, "refAxesType");
    CHECK(std::string(lua_tostring(L, -1)) == "table");
    lua_pop(L, 1);
    CHECK(!gbool(L, "refAxesX"));
    lua_getglobal(L, "refFacesType");
    CHECK(std::string(lua_tostring(L, -1)) == "table");
    lua_pop(L, 1);
    CHECK(!gbool(L, "refFacesTop"));
    lua_getglobal(L, "refResizeType");
    CHECK(std::string(lua_tostring(L, -1)) == "table");
    lua_pop(L, 1);
    CHECK(!gbool(L, "refResizeFront"));
    CHECK(gbool(L, "faceConstruct"));
    CHECK(gbool(L, "axisConstruct"));
    CHECK(gbool(L, "faceSet"));
    CHECK(gbool(L, "axisSet"));
}

TEST_CASE("DataStore: optional local-file persistence") {
    const fs::path dir = fs::temp_directory_path() / "boblox_ds_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    setenv("RBX_DATASTORE_DIR", dir.c_str(), 1);
    {
        Env env;
        env.exec(R"(
            local s = game:GetService("DataStoreService"):GetDataStore("persist")
            s:SetAsync("k", {a = 7, b = {"x", true}})
            s:IncrementAsync("n", 3)
        )");
    }
    {
        Env env;
        env.exec(R"(
            local s = game:GetService("DataStoreService"):GetDataStore("persist")
            local v = s:GetAsync("k")
            persisted = type(v) == "table" and v.a == 7 and v.b[1] == "x" and
                        v.b[2] == true
            n = s:GetAsync("n")
        )");
        CHECK(gbool(env.L(), "persisted"));
        CHECK(gnum(env.L(), "n") == doctest::Approx(3.0));
    }
    unsetenv("RBX_DATASTORE_DIR");
    fs::remove_all(dir, ec);
}

TEST_CASE("DataStore: list keys and ordered pages") {
    Env env;
    env.exec(R"(
        local ds = game:GetService("DataStoreService")
        local s = ds:GetDataStore("list")
        s:SetAsync("a1", 1); s:SetAsync("a2", 2); s:SetAsync("b1", 3)
        local pages = s:ListKeysAsync("a")
        local keys = pages:GetCurrentPage()
        listOk = #keys == 2 and
                 ((keys[1] == "a1" and keys[2] == "a2") or
                  (keys[1] == "a2" and keys[2] == "a1"))
        local o = ds:GetOrderedDataStore("ord")
        o:SetAsync("x", 10); o:SetAsync("y", 5)
        local op = o:GetSortedAsync(true)
        local arr = op:GetCurrentPage()
        ordOk = #arr == 2 and arr[1].key == "y" and arr[2].key == "x"
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "listOk"));
    CHECK(gbool(L, "ordOk"));
}

TEST_CASE("HttpService: JSON encode/decode round-trip") {
    Env env;
    env.exec(R"(
        local h = game:GetService("HttpService")
        enc = h:JSONEncode({a = 1, b = true, c = {"x", 2}, d = {k = "v"}})
        dec = h:JSONDecode('{"a":1,"b":[true,null,"x"],"n":-2.5}')
        decOk = dec.a == 1 and dec.b[1] == true and dec.b[2] == nil and
                dec.b[3] == "x" and dec.n == -2.5
        encA = string.find(enc, '"a":1', 1, true) ~= nil
        encC = string.find(enc, '"c":["x",2]', 1, true) ~= nil
        encD = string.find(enc, '"k":"v"', 1, true) ~= nil
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "decOk"));
    CHECK(gbool(L, "encA"));
    CHECK(gbool(L, "encC"));
    CHECK(gbool(L, "encD"));
}

TEST_CASE("HttpService: GUID, URL encoding and user agent") {
    Env env;
    env.exec(R"(
        local h = game:GetService("HttpService")
        g1 = h:GenerateGUID()
        g2 = h:GenerateGUID(false)
        enc = h:UrlEncode("a b/c?d=e")
        ua = h:GetUserAgent()
    )");
    lua_State* L = env.L();
    const std::string g1 = gstr(L, "g1");
    const std::string g2 = gstr(L, "g2");
    CHECK(g1.size() == 38);
    CHECK(g2.size() == 36);
    CHECK(g1[0] == '{');
    CHECK(g1.back() == '}');
    CHECK(g1 != g2);
    CHECK(gstr(L, "enc") == "a+b%2Fc%3Fd%3De");
    CHECK(gstr(L, "ua") == "Roblox/WinInet");
}

TEST_CASE("DataStore: client-local values round-trip and cache") {
    Env env;
    env.exec(R"(
        local ds = game:GetService("DataStoreService")
        local a = ds:GetGlobalDataStore()
        local b = ds:GetGlobalDataStore()
        same = a == b
        a:SetAsync("k", {n = 7})
        local v = a:GetAsync("k")
        readOk = type(v) == "table" and v.n == 7
        local upd = a:UpdateAsync("k", function(old)
            return {n = old.n + 1}
        end)
        updateOk = upd.n == 8 and a:GetAsync("k").n == 8
        local inc = a:IncrementAsync("counter", 2)
        incOk = inc == 2 and a:IncrementAsync("counter") == 3
        local old = a:RemoveAsync("k")
        removeOk = old.n == 8 and a:GetAsync("k") == nil
        budget = ds:GetRequestBudgetForRequestType(0)
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "same"));
    CHECK(gbool(L, "readOk"));
    CHECK(gbool(L, "updateOk"));
    CHECK(gbool(L, "incOk"));
    CHECK(gbool(L, "removeOk"));
    CHECK(gnum(L, "budget") == doctest::Approx(1000.0));
}

TEST_CASE("require: module order cannot corrupt later GETIMPORT") {
    Env env;
    rbx::Ref<rbx::Instance> script =
        rbx::instantiate(rbx::find_class("LocalScript"));
    REQUIRE(script);
    script->set_name("Load");
    auto add = [&](const char* name, const char* src) {
        rbx::Ref<rbx::Instance> m =
            rbx::instantiate(rbx::find_class("ModuleScript"));
        REQUIRE(m);
        m->set_name(name);
        REQUIRE(m->reparent(script.get(), nullptr));
        std::vector<char> bc = compile(src);
        rbxch::engine::set_module_source(
            *env.e, m.get(), std::vector<uint8_t>(bc.begin(), bc.end()));
    };
    add("LoadTimers", "return { create_session = function() return nil end }");
    add("NetworkStats", "return { start = function() return 42 end }");
    const std::string source =
        "local lt = require(script.LoadTimers)\n"
        "local ns = require(script.NetworkStats)\n"
        "local _ = lt.create_session('x')\n"
        "probe.answer = ns.start()\n";
    lua_newtable(env.L());
    lua_setglobal(env.L(), "probe");
    std::vector<char> bc = compile(source);
    rbxch::Result r =
        rbxch::engine::execute(*env.e, bc.data(), bc.size(), script.get());
    INFO("script: " << source << "\nerror: " << r.error);
    REQUIRE(r.status == rbxch::Status::Ok);
    lua_State* L = env.L();
    lua_getglobal(L, "probe");
    REQUIRE(lua_istable(L, -1));
    lua_getfield(L, -1, "answer");
    CHECK(lua_tonumber(L, -1) == doctest::Approx(42.0));
    lua_pop(L, 2);
}

TEST_CASE("require: module top-level yield completes through scheduler") {
    Env env;
    rbx::Ref<rbx::Instance> script =
        rbx::instantiate(rbx::find_class("LocalScript"));
    REQUIRE(script);
    script->set_name("Load");
    rbx::Ref<rbx::Instance> m =
        rbx::instantiate(rbx::find_class("ModuleScript"));
    REQUIRE(m);
    m->set_name("Slow");
    REQUIRE(m->reparent(script.get(), nullptr));
    std::vector<char> modbc = compile("task.wait(0.2)\nreturn {x = 42}");
    rbxch::engine::set_module_source(
        *env.e, m.get(), std::vector<uint8_t>(modbc.begin(), modbc.end()));
    const std::string source =
        "local m = require(script.Slow)\nprobe.x = m.x\n";
    lua_newtable(env.L());
    lua_setglobal(env.L(), "probe");
    std::vector<char> bc = compile(source);
    rbxch::Result r =
        rbxch::engine::execute(*env.e, bc.data(), bc.size(), script.get());
    INFO("script: " << source << "\nerror: " << r.error);
    REQUIRE(r.status == rbxch::Status::Ok);
    lua_State* L = env.L();
    lua_getglobal(L, "probe");
    lua_getfield(L, -1, "x");
    CHECK(lua_tonumber(L, -1) == doctest::Approx(42.0));
    lua_pop(L, 2);
}

TEST_CASE("BindableFunction: OnInvoke callback receives args and returns") {
    Env env;
    env.exec(R"(
        local f = Instance.new("BindableFunction")
        f.OnInvoke = function(a, b) return a + b, a * b end
        r1, r2 = f:Invoke(3, 4)
    )");
    lua_State* L = env.L();
    CHECK(gnum(L, "r1") == doctest::Approx(7.0));
    CHECK(gnum(L, "r2") == doctest::Approx(12.0));
}

TEST_CASE("animation: load/play/stop tracks round-trip") {
    Env env;
    env.exec(R"(
        local anim = Instance.new("Animation")
        anim.AnimationId = "rbxassetid://123"
        local model = Instance.new("Model")
        local hum = Instance.new("Humanoid", model)
        local track = hum:LoadAnimation(anim)
        trackOk = track.ClassName == "AnimationTrack" and
                  track.Animation == anim
        track:Play()
        playing1 = #hum:GetPlayingAnimationTracks()
        byId = hum:GetTrackByAnimationId("rbxassetid://123")
        foundOk = byId == track
        track:Stop()
        playing2 = #hum:GetPlayingAnimationTracks()
        track:AdjustSpeed(2)
        speedOk = track.Speed == 2
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "trackOk"));
    CHECK(gnum(L, "playing1") == doctest::Approx(1.0));
    CHECK(gbool(L, "foundOk"));
    CHECK(gnum(L, "playing2") == doctest::Approx(0.0));
    CHECK(gbool(L, "speedOk"));
}

TEST_CASE("Model bounds/pivot and QueryDescendants") {
    Env env;
    env.exec(R"(
        local m = Instance.new("Model")
        local p1 = Instance.new("Part", m)
        p1.Name = "P1"
        p1.Size = Vector3.new(2, 2, 2)
        p1.CFrame = CFrame.new(0, 0, 0)
        local p2 = Instance.new("Part", m)
        p2.Name = "P2"
        p2.Size = Vector3.new(2, 2, 2)
        p2.CFrame = CFrame.new(4, 0, 0)
        gP1, gP2 = p1, p2
        m.PrimaryPart = p1
        ext = m:GetExtentsSize()
        box, bsize = m:GetBoundingBox()
        pivot = m:GetPivot()
        q = m:QueryDescendants("Part")
        qn = m:QueryDescendants("#P1")
    )");
    lua_State* L = env.L();
    lua_getglobal(L, "ext");
    REQUIRE(lua_isuserdata(L, -1));
    // Vector3 userdata fields are accessed through the Lua-side test below.
    lua_pop(L, 1);
    env.exec(R"(
        extType = typeof(ext)
        boxType = typeof(box)
        pivotType = typeof(pivot)
        extOk = ext == Vector3.new(6, 2, 2)
        boxOk = box ~= nil and box.Position == Vector3.new(2, 0, 0) and
                bsize == Vector3.new(6, 2, 2)
        pivotOk = pivot ~= nil and pivot.Position == gP1.Position
        queryOk = #q == 2 and q[1] == gP1 and q[2] == gP2
        queryNameOk = #qn == 1 and qn[1] == gP1
    )");
    CHECK(gstr(L, "extType") == "Vector3");
    CHECK(gstr(L, "boxType") == "CFrame");
    CHECK(gstr(L, "pivotType") == "CFrame");
    CHECK(gbool(L, "extOk"));
    CHECK(gbool(L, "boxOk"));
    CHECK(gbool(L, "pivotOk"));
    CHECK(gbool(L, "queryOk"));
    CHECK(gbool(L, "queryNameOk"));
}

TEST_CASE("workspace: ray and region queries hit real parts") {
    Env env;
    env.exec(R"(
        local p = Instance.new("Part", workspace)
        p.Name = "HitTarget"
        p.Size = Vector3.new(4, 4, 4)
        p.CFrame = CFrame.new(0, 0, 0)
        local hit, hx, hy, hz, nx, ny, nz = workspace:FindPartOnRay(
            Ray.new(Vector3.new(0, 0, -10), Vector3.new(0, 0, 1)))
        rayHit = hit == p and math.abs(hz - (-2)) < 0.001 and nz == -1
        local hit2 = workspace:Raycast(Vector3.new(0, 0, -10),
                                       Vector3.new(0, 0, 1))
        raycastHit = hit2 and hit2.Instance == p and hit2.Distance == 8
        local list = workspace:FindPartsInRegion3(
            Region3.new(Vector3.new(-1, -1, -1), Vector3.new(1, 1, 1)),
            nil, 10)
        regionHit = #list == 1 and list[1] == p
        regionEmpty = workspace:IsRegion3Empty(
            Region3.new(Vector3.new(10, 10, 10), Vector3.new(11, 11, 11)))
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "rayHit"));
    CHECK(gbool(L, "raycastHit"));
    CHECK(gbool(L, "regionHit"));
    CHECK(gbool(L, "regionEmpty"));
}

TEST_CASE("methods: lower-case aliases and class/descendant lookup") {
    Env env;
    env.exec(R"(
        local root = Instance.new("Part")
        root.Name = "Root"
        local mid = Instance.new("Model", root)
        mid.Name = "Mid"
        local leaf = Instance.new("Part", mid)
        leaf.Name = "Leaf"
        aliasFind = root:findFirstChild("Mid") == mid
        aliasChildren = #root:getChildren() == 1
        aliasIsa = root:isA("Part")
        aliasDesc = leaf:isDescendantOf(root)
        byClass = root:FindFirstChildWhichIsA("Part", true) == leaf
        byClassDirect = root:FindFirstChildWhichIsA("Part") == nil
        byName = root:FindFirstDescendant("Leaf") == leaf
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "aliasFind"));
    CHECK(gbool(L, "aliasChildren"));
    CHECK(gbool(L, "aliasIsa"));
    CHECK(gbool(L, "aliasDesc"));
    CHECK(gbool(L, "byClass"));
    CHECK(gbool(L, "byClassDirect"));
    CHECK(gbool(L, "byName"));
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

TEST_CASE("RunService: client frame signals and render-step bindings") {
    Env env;
    // A LocalScript must be able to yield on a client signal at top level.
    env.exec(R"(
        local rs = game:GetService("RunService")
        rsType = typeof(rs)
        rsCls = rs.ClassName
        renders = 0
        lastDt = 0
        rs.RenderStepped:Connect(function(dt)
            renders = renders + 1
            lastDt = dt
        end)
        order = ""
        rs:BindToRenderStep("late", 300, function() order = order .. "L" end)
        rs:BindToRenderStep("early", 100, function() order = order .. "E" end)
        rs:BindToRenderStep("early", 50, function() order = order .. "e" end)
        topLevelWaited = 0
        rs.Heartbeat:Wait()
        topLevelWaited = 1
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "rsType") == "Instance");
    CHECK(gstr(L, "rsCls") == "RunService");

    env.step(0.5);
    CHECK(gnum(L, "renders") == 1);
    CHECK(gnum(L, "lastDt") == doctest::Approx(0.5));
    // BindToRenderStep replaces by name; lower priority runs first.
    CHECK(gstr(L, "order") == "eL");
    CHECK(gnum(L, "topLevelWaited") == 0); // resumed on the next pump

    env.step(0.0);
    CHECK(gnum(L, "topLevelWaited") == 1);
    CHECK(gnum(L, "renders") == 2);
    CHECK(gstr(L, "order") == "eLeL");
}

TEST_CASE("Humanoid: MoveTo updates local RootPart and Position") {
    Env env;
    env.exec(R"(
        local model = Instance.new("Model")
        local root = Instance.new("Part", model)
        root.Name = "HumanoidRootPart"
        root.CFrame = CFrame.new(1, 2, 3)
        local hum = Instance.new("Humanoid", model)
        rootIsRoot = hum.RootPart == root
        beforeX = root.Position.X
        hum:MoveTo(Vector3.new(10, 20, 30))
        after = root.Position
        moved = after.X == 10 and after.Y == 20 and after.Z == 30
        cframeMoved = root.CFrame.Position.X == 10
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "rootIsRoot"));
    CHECK(gbool(L, "moved"));
    CHECK(gbool(L, "cframeMoved"));
    CHECK(gnum(L, "beforeX") == doctest::Approx(1.0));
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

TEST_CASE("Players: LocalPlayer, player lookup, character distance") {
    Env env;
    lua_State* L = env.L();
    rbx::Ref<rbx::Instance> players =
        rbx::instantiate(rbx::find_class("Players"));
    rbx::Ref<rbx::Instance> alice =
        rbx::instantiate(rbx::find_class("Player"));
    REQUIRE(players);
    REQUIRE(alice);
    std::string err;
    REQUIRE(alice->set_prop("Name", rbx::Variant(std::string("Alice")), &err));
    REQUIRE(alice->set_prop("UserId", rbx::Variant(int64_t(42)), &err));
    REQUIRE(alice->reparent(players.get(), &err));
    rbx::Ref<rbx::Instance> character =
        rbx::instantiate(rbx::find_class("Model"));
    REQUIRE(character);
    rbx::Ref<rbx::Instance> root =
        rbx::instantiate(rbx::find_class("Part"));
    REQUIRE(root);
    rbx::CFrame cf;
    cf.x = 3.0;
    cf.y = 4.0;
    REQUIRE(root->set_prop("CFrame", rbx::Variant(cf), &err));
    REQUIRE(root->reparent(character.get(), &err));
    REQUIRE(root->set_prop("Name", rbx::Variant(std::string("HumanoidRootPart")),
                           &err));
    REQUIRE(alice->set_prop("Character",
                            rbx::Variant(static_cast<rbx::Instance*>(character.get())),
                            &err));
    rbx::Instance::push(L, players.get());
    lua_setglobal(L, "players");
    rbx::Instance::push(L, alice.get());
    lua_setglobal(L, "alice");
    rbx::Instance::push(L, character.get());
    lua_setglobal(L, "character");
    env.exec(R"(
        local list = players:GetPlayers()
        count = #list
        one = list[1] == alice
        lp = players.LocalPlayer == alice
        num = players.NumPlayers
        byId = players:GetPlayerByUserId(42) == alice
        missing = players:GetPlayerByUserId(7) == nil
        fromChar = players:GetPlayerFromCharacter(character) == alice
        dist = alice:DistanceFromCharacter(Vector3.new(0, 0, 0))
        ping = alice:GetNetworkPing()
    )");
    CHECK(gnum(L, "count") == doctest::Approx(1.0));
    CHECK(gbool(L, "one"));
    CHECK(gbool(L, "lp"));
    CHECK(gnum(L, "num") == doctest::Approx(1.0));
    CHECK(gbool(L, "byId"));
    CHECK(gbool(L, "missing"));
    CHECK(gbool(L, "fromChar"));
    CHECK(gnum(L, "dist") == doctest::Approx(5.0));
    CHECK(gnum(L, "ping") == doctest::Approx(0.0));
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
        itemStr = tostring(Enum.Material.Plastic)
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
    CHECK(gstr(L, "itemStr") == "Enum.Material.Plastic");
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

TEST_CASE("joindata apply: snapshot batches become a live world") {
    // Frozen Crossroads 1818 snapshot (frame012: 146 script/sound records,
    // frame030: 29 Parts + Terrain) applied onto a fresh DataModel root.
    // Pins the tree (parents/refs), typed props, and the audit counters.
    Env env;
    const fs::path root(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root / "run" / "join97_1818.raw");
    rbxclient::JoinSchema schema;
    std::string err;
    REQUIRE(rbxclient::parseJoinSchema(inner.data(), inner.size(), schema,
                                       &err));
    std::vector<rbxclient::JoinRecord> recs;
    for (const char* fr : {"frame012.bin", "frame030.bin"}) {
        const auto f = slurp(root / "tests" / "vectors" / "repl" / fr);
        const size_t k = size_t(1) << (f[0] >> 6);
        REQUIRE(k < f.size());
        const uint8_t* body = f.data() + k;
        const size_t blen = f.size() - k;
        std::vector<uint8_t> batch;
        REQUIRE(rbxclient::parseDeltaBatch(body, blen, batch, &err));
        std::vector<rbxclient::JoinRecord> one;
        REQUIRE(rbxclient::parseJoinRecords(batch.data(), batch.size(),
                                            schema, one, &err));
        recs.insert(recs.end(), one.begin(), one.end());
    }
    REQUIRE(recs.size() == 176);
    rbxclient::ApplyReport rep =
        rbxclient::applyJoinSnapshot(schema, recs, &err);
    REQUIRE(rep.root);
    CHECK(rep.root->class_name() == "DataModel");
    MESSAGE("spawned=" << rep.stats.spawned
                       << " skipped_records=" << rep.stats.skipped_records
                       << " props_set=" << rep.stats.props_set
                       << " undeclared=" << rep.stats.props_skipped_undeclared
                       << " readonly=" << rep.stats.props_skipped_readonly
                       << " typemismatch="
                       << rep.stats.props_skipped_typemismatch
                       << " unresolvable="
                       << rep.stats.props_skipped_unresolvable
                       << " refs_missing=" << rep.stats.refs_missing
                       << " grafted=" << rep.stats.grafted_to_root);
    for (const auto& a : rep.stats.aliases)
        MESSAGE("alias " << a.first << " -> " << a.second);
    for (const auto& sk : rep.stats.skip_names)
        MESSAGE("skip " << sk.first << " x" << sk.second);
    CHECK(rep.stats.spawned == 176);
    CHECK(rep.stats.skipped_records == 0);
    // Sound#16098 (Ping): scalar props land, missing parent grafts to root.
    {
        REQUIRE(rep.by_id.count(16098) == 1);
        rbx::Instance* s =
            rep.instances[size_t(rep.by_id[16098])].get();
        CHECK(s->class_name() == "Sound");
        rbx::Variant v;
        REQUIRE(s->get_prop("Name", v));
        CHECK(std::get<std::string>(v) == "Ping");
        REQUIRE(s->get_prop("AudioContent", v));
        const rbx::Content& c = std::get<rbx::Content>(v);
        CHECK(c.source == 1);
        CHECK(c.uri == "rbxasset://Sounds/electronicpingshort.wav");
        CHECK(s->parent() == rep.root.get());
    }
    // Part#4363439 (Tool Handle): shared name, aliased size, identity
    // CFrame rotation with an exact decoded position.
    {
        REQUIRE(rep.by_id.count(4363439) == 1);
        rbx::Instance* p =
            rep.instances[size_t(rep.by_id[4363439])].get();
        CHECK(p->class_name() == "Part");
        rbx::Variant v;
        REQUIRE(p->get_prop("Name", v));
        CHECK(std::get<std::string>(v) == "Handle");
        REQUIRE(p->get_prop("Size", v));
        const rbx::Vector3& sz = std::get<rbx::Vector3>(v);
        CHECK(sz.x == doctest::Approx(2.0));
        CHECK(sz.y == doctest::Approx(2.4000000953674316));
        CHECK(sz.z == doctest::Approx(1.0));
        REQUIRE(p->get_prop("CFrame", v));
        const rbx::CFrame& cf = std::get<rbx::CFrame>(v);
        CHECK(cf.x == doctest::Approx(-6.820067882537842));
        CHECK(cf.y == doctest::Approx(3.2196359634399414));
        CHECK(cf.z == doctest::Approx(34.07008361816406));
        CHECK(cf.r00 == 1.0);
        CHECK(cf.r11 == 1.0);
        CHECK(cf.r22 == 1.0);
        CHECK(cf.r01 == 0.0);
        CHECK(cf.r10 == 0.0);
        REQUIRE(p->get_prop("CanCollide", v));
        CHECK(std::get<bool>(v) == false);
        CHECK(p->parent() == rep.root.get()); // Tool#4363438 not in subset
    }
    // Wire-name alias took exactly the version-skew renames.
    CHECK(rep.stats.aliases.count("size") == 1);
    CHECK(rep.stats.aliases["size"] == "Size");
    // Snapshot walk (pure, no GL): every Part-class box lands with its
    // CFrame position and Size extents; the Tool Handle is among them.
    {
        const std::vector<rbxclient::ViewBox> boxes =
            rbxclient::collectBoxes(rep.root.get());
        CHECK(!boxes.empty());
        bool found_handle = false;
        for (const auto& b : boxes) {
            if (b.name == "Handle" &&
                b.px == doctest::Approx(-6.820067882537842)) {
                found_handle = true;
                CHECK(b.sx == doctest::Approx(2.0));
                CHECK(b.sy == doctest::Approx(2.4000000953674316));
                CHECK(b.sz == doctest::Approx(1.0));
                CHECK(b.r00 == 1.0);
                CHECK(b.r11 == 1.0);
                CHECK(b.r22 == 1.0);
            }
        }
        CHECK(found_handle);
    }
}

TEST_CASE("joindata apply: full snapshot becomes the place tree") {
    // All 8 snapshot batches (2910 unique instances after dedupe) under
    // one DataModel root: services graft at top, Workspace subtree lands
    // whole, dynamics graft with counts. Pins the milestone end state.
    Env env;
    const fs::path root(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root / "run" / "join97_1818.raw");
    rbxclient::JoinSchema schema;
    std::string err;
    REQUIRE(rbxclient::parseJoinSchema(inner.data(), inner.size(), schema,
                                       &err));
    std::vector<rbxclient::JoinRecord> recs;
    for (const char* fr :
         {"frame012.bin", "frame022.bin", "frame024.bin", "frame026.bin",
          "frame028.bin", "frame030.bin", "frame032.bin", "frame034.bin",
          "frame036.bin"}) {
        const auto f = slurp(root / "tests" / "vectors" / "repl" / fr);
        const size_t k = size_t(1) << (f[0] >> 6);
        REQUIRE(k < f.size());
        std::vector<uint8_t> batch;
        REQUIRE(rbxclient::parseDeltaBatch(f.data() + k, f.size() - k, batch,
                                           &err));
        std::vector<rbxclient::JoinRecord> one;
        const bool ok = rbxclient::parseJoinRecords(
            batch.data(), batch.size(), schema, one, &err);
        INFO("frame " << std::string(fr) << ": " << err);
        REQUIRE(ok);
        recs.insert(recs.end(), one.begin(), one.end());
    }
    REQUIRE(recs.size() == 2915);
    rbxclient::ApplyReport rep =
        rbxclient::applyJoinSnapshot(schema, recs, &err);
    REQUIRE(rep.root);
    MESSAGE("full: spawned=" << rep.stats.spawned << " skipped_records="
                              << rep.stats.skipped_records << " props_set="
                              << rep.stats.props_set << " undeclared="
                              << rep.stats.props_skipped_undeclared
                              << " typemismatch="
                              << rep.stats.props_skipped_typemismatch
                              << " refs_missing=" << rep.stats.refs_missing
                              << " grafted=" << rep.stats.grafted_to_root);
    CHECK(rep.stats.spawned == 2910);
    CHECK(rep.stats.skipped_records == 5); // retransmitted ids, first wins
    // Top-level services graft under the fresh DataModel root.
    auto child_of_class = [&](const std::string& cls) {
        for (const auto& ch : rep.root->children()) {
            if (ch->class_name() == cls)
                return ch.get();
        }
        return (rbx::Instance*)nullptr;
    };
    rbx::Instance* ws = child_of_class("Workspace");
    REQUIRE(ws != nullptr);
    REQUIRE(child_of_class("Players") != nullptr);
    REQUIRE(child_of_class("Lighting") != nullptr);
    REQUIRE(child_of_class("SoundService") != nullptr);
    REQUIRE(child_of_class("Teams") != nullptr);
    REQUIRE(child_of_class("ReplicatedStorage") != nullptr);
    // Tool Handle parents to its Tool (both in-snapshot), not the root.
    REQUIRE(rep.by_id.count(4363439) == 1);
    REQUIRE(rep.by_id.count(4363438) == 1);
    rbx::Instance* handle =
        rep.instances[size_t(rep.by_id[4363439])].get();
    rbx::Instance* tool = rep.instances[size_t(rep.by_id[4363438])].get();
    CHECK(tool->class_name() == "Tool");
    CHECK(handle->parent() == tool);
    CHECK(tool->parent() != nullptr);
    // Tag-44 wire values are zigzag (sub_340A3E4 case 0x2C), not plain:
    // IntValue#4058575 Value carries wire 30 -> applies as 15 (a plain
    // decode would land 30).
    {
        REQUIRE(rep.by_id.count(4058575) == 1);
        rbx::Instance* iv =
            rep.instances[size_t(rep.by_id[4058575])].get();
        CHECK(iv->class_name() == "IntValue");
        rbx::Variant v;
        REQUIRE(iv->get_prop("Value", v));
        CHECK(std::get<int64_t>(v) == 15);
    }
    // Every Part CFrame in the world is orthonormal with det +1 — this
    // validates the quantized-rotation port on the 28 kind==0 cases plus
    // all table-path cases end to end.
    int cframes = 0;
    for (const auto& inst : rep.instances) {
        if (inst->class_name() != "Part")
            continue;
        rbx::Variant v;
        REQUIRE(inst->get_prop("CFrame", v));
        const rbx::CFrame& cf = std::get<rbx::CFrame>(v);
        const double rows[3][3] = {{cf.r00, cf.r01, cf.r02},
                                   {cf.r10, cf.r11, cf.r12},
                                   {cf.r20, cf.r21, cf.r22}};
        for (int i = 0; i < 3; ++i) {
            const double n = rows[i][0] * rows[i][0] +
                             rows[i][1] * rows[i][1] +
                             rows[i][2] * rows[i][2];
            CHECK(n == doctest::Approx(1.0));
        }
        const double det =
            rows[0][0] * (rows[1][1] * rows[2][2] - rows[1][2] * rows[2][1]) -
            rows[0][1] * (rows[1][0] * rows[2][2] - rows[1][2] * rows[2][0]) +
            rows[0][2] * (rows[1][0] * rows[2][1] - rows[1][1] * rows[2][0]);
        CHECK(det == doctest::Approx(1.0));
        ++cframes;
    }
    MESSAGE("parts with CFrame checked: " << cframes);
    CHECK(cframes == 58); // exact-"Part" instances in the snapshot
    // Audit pins: every prop either lands typed or skips counted.
    // The reference property merge resolves 72 more names, plus the six
    // NetworkOwnerV3 grants the snapshot carries (6101), and removes 87
    // undeclared (1972). Script Source hashes are content keys,
    // not string properties, so they no longer count as mismatches. The
    // 15 remaining mismatches are the nearest-representation mappings for
    // rare reference-only value types (Faces/Axes and friends).
    CHECK(rep.stats.props_set == 6101);
    CHECK(rep.stats.props_skipped_undeclared == 1972);
    CHECK(rep.stats.source_hashes == 236);
    CHECK(rep.stats.props_skipped_typemismatch == 15);
    CHECK(rep.stats.refs_missing == 138); // live refs outside the snapshot
    CHECK(rep.stats.grafted_to_root == 112); // 53 top-level + 59 dynamics
}

TEST_CASE("streaming apply: spawn/update/entries/events fold into the world") {
    // Snapshot subset (frame030) + live items: od124's Part spawn defers
    // because its parent is absent, the parent's later arrival grafts it,
    // re-apply updates, od124's entry is the same Part, msg07's 8 events only
    // log. Pins the recv-complete apply path.
    Env env;
    const fs::path root(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root / "run" / "join97_1818.raw");
    rbxclient::JoinSchema schema;
    std::string err;
    REQUIRE(rbxclient::parseJoinSchema(inner.data(), inner.size(), schema,
                                       &err));
    const auto f030 = slurp(root / "tests" / "vectors" / "repl" / "frame030.bin");
    const size_t k = size_t(1) << (f030[0] >> 6);
    REQUIRE(k < f030.size());
    std::vector<uint8_t> batch;
    REQUIRE(rbxclient::parseDeltaBatch(f030.data() + k, f030.size() - k,
                                       batch, &err));
    std::vector<rbxclient::JoinRecord> recs;
    REQUIRE(rbxclient::parseJoinRecords(batch.data(), batch.size(), schema,
                                        recs, &err));
    rbxclient::ApplyReport rep =
        rbxclient::applyJoinSnapshot(schema, recs, &err);
    REQUIRE(rep.root);
    const size_t base_instances = rep.instances.size();
    const int deferred_before = rep.stats.deferred_to_parent;
    rbxclient::StreamStats sst;
    uint16_t part_cls = 0;
    // od124: one Part spawn (id 4342983, parent 4342845 absent -> deferred).
    {
        const auto m = slurp(root / "tests" / "vectors" / "repl" / "od124.bin");
        rbxclient::Walk83 w;
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), schema, w, &err));
        REQUIRE(w.complete);
        for (const auto& it : w.items) {
            if (it.type != 0x0d)
                continue;
            for (const auto& rec : it.delta0d.entries.records) {
                if (rec.id == 4342983)
                    part_cls = rec.cls;
                rbxclient::applyStreamRecord(rep, schema, rec.id, rec.cls,
                                             rec.props, rec.parent, sst);
                ++sst.entries_applied;
            }
        }
    }
    CHECK(sst.spawned == 1);
    CHECK(sst.entries_applied == 1);
    CHECK(sst.stream_grafted == 0);
    CHECK(rep.stats.deferred_to_parent == deferred_before + 1);
    CHECK(rep.instances.size() == base_instances + 1);
    REQUIRE(rep.by_id.count(4342983) == 1);
    rbx::Instance* part =
        rep.instances[size_t(rep.by_id[4342983])].get();
    CHECK(part->class_name() == "Part");
    CHECK(part->parent() == nullptr); // parent absent: deferred
    // Re-apply the same record: update path, no new instance.
    {
        const auto m = slurp(root / "tests" / "vectors" / "repl" / "od124.bin");
        rbxclient::Walk83 w;
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), schema, w, &err));
        for (const auto& it : w.items) {
            if (it.type != 0x0d)
                continue;
            for (const auto& rec : it.delta0d.entries.records) {
                rbxclient::applyStreamRecord(rep, schema, rec.id, rec.cls,
                                             rec.props, rec.parent, sst);
                ++sst.entries_applied;
            }
        }
    }
    CHECK(sst.spawned == 1);
    CHECK(sst.updated == 1);
    CHECK(rep.instances.size() == base_instances + 1);
    CHECK(part->parent() == nullptr);
    // The missing parent arrives: attachPending re-parents the deferred child.
    {
        const std::vector<rbxclient::JoinProp> no_props;
        rbxclient::applyStreamRecord(rep, schema, 4342845, part_cls, no_props,
                                     -1, sst);
    }
    REQUIRE(rep.by_id.count(4342845) == 1);
    rbx::Instance* part_parent =
        rep.instances[size_t(rep.by_id[4342845])].get();
    CHECK(part->parent() == part_parent);
    CHECK(sst.spawned == 2);
    CHECK(rep.instances.size() == base_instances + 2);
    // msg07: 8 events log only (no instances, no props).
    {
        const auto m = slurp(root / "tests" / "vectors" / "repl" / "msg07.bin");
        rbxclient::Walk83 w;
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), schema, w, &err));
        for (const auto& it : w.items) {
            if (it.type == 0x07)
                rbxclient::logStreamEvent(schema, it.event, sst);
        }
    }
    CHECK(sst.events_logged == 8);
    CHECK(sst.events_by_name["FriendService.RemoteFriendStatusSignal"] == 8);
    CHECK(rep.instances.size() == base_instances + 2);
    // Out-of-order 0x03: the triple names an entity that has not spawned.
    // The native replicator holds it and applies on spawn; dropping it loses
    // the property (MM2 applies BallSocketConstraint.Attachment0/1 early).
    {
        const uint32_t late_id = 4349999;
        const int applied_before = sst.prop_applied;
        rbxclient::JoinProp prop;
        prop.name = "Transparency";
        prop.tag = 12; // f64
        const double tv = 0.25;
        uint8_t bytes[8];
        std::memcpy(bytes, &tv, sizeof bytes);
        prop.value.assign(bytes, bytes + sizeof bytes);
        CHECK(rbxclient::applyStreamProp(rep, schema, late_id, part_cls,
                                         prop, sst));
        CHECK(sst.prop_deferred == 1);
        CHECK(sst.prop_applied == applied_before);
        REQUIRE(rep.pending_props.count(late_id) == 1);
        // The spawn arrives: the queued prop is applied and the queue drains.
        const std::vector<rbxclient::JoinProp> no_props;
        rbxclient::applyStreamRecord(rep, schema, late_id, part_cls, no_props,
                                     -1, sst);
        CHECK(rep.pending_props.count(late_id) == 0);
        CHECK(sst.prop_applied == applied_before + 1);
        REQUIRE(rep.by_id.count(late_id) == 1);
        rbx::Instance* late =
            rep.instances[size_t(rep.by_id[late_id])].get();
        rbx::Variant got;
        REQUIRE(late->get_prop("Transparency", got));
        const auto* dv = std::get_if<double>(&got);
        REQUIRE(dv != nullptr);
        CHECK(*dv == 0.25);
    }
    // Parent triples: the member-count sentinel is the out-of-band Parent
    // property. An existing parent re-parents immediately; an unspawned one
    // defers and attaches when the parent streams in (native deferral).
    {
        const uint32_t p_id = 4350001, c_id = 4350002;
        const std::vector<rbxclient::JoinProp> none;
        rbxclient::applyStreamRecord(rep, schema, p_id, part_cls, none, -1,
                                     sst);
        rbxclient::applyStreamRecord(rep, schema, c_id, part_cls, none, -1,
                                     sst);
        const int applied_before = sst.prop_applied;
        CHECK(rbxclient::applyParentUpdate(rep, c_id, int64_t(p_id), sst));
        CHECK(sst.prop_applied == applied_before + 1);
        REQUIRE(rep.by_id.count(c_id) == 1);
        rbx::Instance* child = rep.instances[size_t(rep.by_id[c_id])].get();
        CHECK(child->parent() ==
              rep.instances[size_t(rep.by_id[p_id])].get());

        const int deferred_before = sst.prop_deferred;
        const uint32_t late_parent = 4350099;
        CHECK(rbxclient::applyParentUpdate(rep, c_id, int64_t(late_parent),
                                           sst));
        CHECK(sst.prop_deferred == deferred_before + 1);
        REQUIRE(rep.pending_parent.count(c_id) == 1);
        rbxclient::applyStreamRecord(rep, schema, late_parent, part_cls, none,
                                     -1, sst);
        CHECK(rep.pending_parent.count(c_id) == 0);
        CHECK(child->parent() ==
              rep.instances[size_t(rep.by_id[late_parent])].get());
    }
}

TEST_CASE("focusCamera: spawn-focus with outlier rejection + fallback") {
    // Synthetic world: two spawn pads near (100, 0, 0), a far outlier at
    // (9000, 0, 0), and a regular part at the origin.
    auto box = [](double x, const std::string& name) {
        rbxclient::ViewBox b;
        b.px = x;
        b.sx = 4.0;
        b.sy = 1.0;
        b.sz = 2.0;
        b.name = name;
        return b;
    };
    const std::vector<rbxclient::ViewBox> boxes = {
        box(100.0, "SpawnLocation"), box(120.0, "SpawnLocation"),
        box(9000.0, "FarAway"), box(0.0, "Part")};
    const rbxclient::CameraFocus f = rbxclient::focusCamera(boxes);
    // Focused on the spawn pads (outlier + origin excluded).
    CHECK(f.cx == doctest::Approx(110.0));
    CHECK(f.cy == doctest::Approx(0.0));
    CHECK(f.cz == doctest::Approx(0.0));
    // Radius covers the pads plus half-extent (~10 + ~2.29), far below the
    // whole-map radius (~4500) that shrank everything to specks.
    CHECK(f.radius == doctest::Approx(12.291).epsilon(0.01));
    CHECK(f.radius < 100.0);
    // No spawns: falls back to all boxes.
    const std::vector<rbxclient::ViewBox> plain = {box(0.0, "A"),
                                                  box(10.0, "B")};
    const rbxclient::CameraFocus g = rbxclient::focusCamera(plain);
    CHECK(g.cx == doctest::Approx(5.0));
    // Empty: safe defaults, never NaN.
    const rbxclient::CameraFocus h =
        rbxclient::focusCamera(std::vector<rbxclient::ViewBox>{});
    CHECK(h.radius == doctest::Approx(1.0));
    // Non-finite positions (NaN/Inf ride live physics) are skipped, never
    // allowed to NaN the focus; all-infinite falls back to safe defaults.
    auto nanbox = [](double x) {
        rbxclient::ViewBox b;
        b.px = x;
        b.sx = 4.0;
        b.sy = 1.0;
        b.sz = 2.0;
        return b;
    };
    const std::vector<rbxclient::ViewBox> poisoned = {
        nanbox(std::numeric_limits<double>::quiet_NaN()), box(50.0, "Ok")};
    const rbxclient::CameraFocus pf = rbxclient::focusCamera(poisoned);
    CHECK(pf.cx == doctest::Approx(50.0));
    const std::vector<rbxclient::ViewBox> allbad = {
        nanbox(std::numeric_limits<double>::infinity())};
    const rbxclient::CameraFocus ab = rbxclient::focusCamera(allbad);
    CHECK(ab.radius == doctest::Approx(1.0));
    // Finite-but-absurd outliers (167k studs live): the median center
    // ignores them and the radius is the 90th-percentile reach.
    const std::vector<rbxclient::ViewBox> wild = {
        box(100.0, "Part"), box(120.0, "Part"), box(167432.0, "Part")};
    const rbxclient::CameraFocus wf = rbxclient::focusCamera(wild);
    CHECK(wf.cx == doctest::Approx(120.0)); // median, not mean (~55884)
    CHECK(wf.radius < 100.0);
    CHECK(wf.radius == doctest::Approx(22.291).epsilon(0.01));
    // Gradual outlier chain (a live map's sky/decor parts): the old
    // "stop at the first 10x jump" rule kept every step because each
    // distance was exactly 10x the previous, framing the whole map and
    // shrinking the playable cluster to specks. The percentile must keep
    // the dense cluster (30 close boxes) and drop the four outliers.
    std::vector<rbxclient::ViewBox> clustered;
    for (int i = 0; i < 30; ++i)
        clustered.push_back(box(double(i) * 2.0, "Part"));
    clustered.push_back(box(500.0, "Sky"));
    clustered.push_back(box(1000.0, "Sky"));
    clustered.push_back(box(2000.0, "Sky"));
    clustered.push_back(box(5000.0, "Sky"));
    const rbxclient::CameraFocus cf = rbxclient::focusCamera(clustered);
    CHECK(cf.radius < 100.0); // dense cluster only, not the 5,000-stud tail
    CHECK(cf.radius > 30.0);  // still covers the near cluster
}

TEST_CASE("live runtime: framed 0x97 + snapshot frames become an Engine world") {
    // Tracked fixtures only: the frozen 0x97 inner plus two snapshot batches
    // from the Crossroads capture, re-framed exactly like the transport
    // (compact-varint frame, compressed JoinData outer, 0x84 marker).
    const fs::path root(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root / "run" / "join97_1818.raw");

    std::vector<uint8_t> compressed(ZSTD_compressBound(inner.size()));
    const size_t csize = ZSTD_compress(compressed.data(), compressed.size(),
                                       inner.data(), inner.size(), 3);
    REQUIRE(!ZSTD_isError(csize));
    compressed.resize(csize);

    std::vector<uint8_t> body{0x97, 0xa3, 0xcf, 0x00, 0x00};
    for (int i = 0; i < 4; ++i)
        body.push_back(uint8_t((inner.size() >> (8 * i)) & 0xff));
    body.insert(body.end(), compressed.begin(), compressed.end());

    std::vector<uint8_t> stream;
    auto add = [&](const std::vector<uint8_t>& payload) {
        rbx::compactVarint(stream, payload.size());
        stream.insert(stream.end(), payload.begin(), payload.end());
    };
    add(body);
    for (const char* fr : {"frame012.bin", "frame030.bin"}) {
        const auto f = slurp(root / "tests" / "vectors" / "repl" / fr);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    add({0x84, 0x01, 0x00, 0x00, 0x00});

    rbxclient::LiveRuntime live("unit-live", [](const std::string&) {});
    live.feed(stream.data(), stream.size());
    live.pump(1.0 / 60.0);
    CHECK_FALSE(live.failed());
    CHECK(live.has_world());
    REQUIRE(live.root() != nullptr);
    CHECK(live.root()->class_name() == "DataModel");
    CHECK(live.snapshot_records() == 176);
    CHECK(live.class_stats().find("Part:") != std::string::npos);
}

TEST_CASE("live runtime: a linked Character with replicated parts passes the gate") {
    // The replicated avatar is a Model wrapper whose own ref is 0; its body
    // parts carry the live refs. The gate must accept that tree without
    // mistaking a client-created fallback (no refs anywhere) for it.
    const fs::path root_dir(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root_dir / "run" / "join97_1818.raw");
    std::vector<uint8_t> compressed(ZSTD_compressBound(inner.size()));
    const size_t csize = ZSTD_compress(compressed.data(), compressed.size(),
                                       inner.data(), inner.size(), 3);
    REQUIRE(!ZSTD_isError(csize));
    compressed.resize(csize);

    std::vector<uint8_t> stream;
    auto add = [&](const std::vector<uint8_t>& payload) {
        rbx::compactVarint(stream, payload.size());
        stream.insert(stream.end(), payload.begin(), payload.end());
    };
    std::vector<uint8_t> body{0x97, 0xa3, 0xcf, 0x00, 0x00};
    for (int i = 0; i < 4; ++i)
        body.push_back(uint8_t((inner.size() >> (8 * i)) & 0xff));
    body.insert(body.end(), compressed.begin(), compressed.end());
    add(body);
    for (const char* fr : {"frame012.bin", "frame030.bin"}) {
        const auto f = slurp(root_dir / "tests" / "vectors" / "repl" / fr);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    add({0x84, 0x01, 0x00, 0x00, 0x00});

    struct LocalUserReset {
        long long id;
        std::string name;
        ~LocalUserReset() {
            rbx::set_local_user_id(id);
            rbx::set_local_user_name(name);
        }
    } reset{rbx::local_user_id(), rbx::local_user_name()};

    rbxclient::LiveRuntime live("unit-character-gate", [](const std::string&) {},
                                false, false);
    live.setLocalUser(42, "A6Tester", "A6Tester");
    live.feed(stream.data(), stream.size());
    live.pump(1.0 / 60.0);
    REQUIRE(live.has_world());

    rbx::Instance* world = const_cast<rbx::Instance*>(live.root());
    REQUIRE(world != nullptr);
    rbx::Instance* players = world->find_first_child_of_class("Players");
    rbx::Instance* workspace = world->find_first_child_of_class("Workspace");
    REQUIRE(players != nullptr);
    REQUIRE(workspace != nullptr);
    rbx::Instance* player = nullptr;
    for (const auto& child : players->children()) {
        if (!child->is_a("Player")) continue;
        rbx::Variant uid;
        if (!child->get_prop("UserId", uid)) continue;
        const auto* id = std::get_if<int64_t>(&uid);
        if (id && *id == 42) { player = child.get(); break; }
    }
    REQUIRE(player != nullptr);

    rbx::Ref<rbx::Instance> model =
        rbx::instantiate(rbx::find_class("Model"));
    rbx::Ref<rbx::Instance> part = rbx::instantiate(rbx::find_class("Part"));
    REQUIRE(model);
    REQUIRE(part);
    model->set_name("A6Tester");
    part->set_name("HumanoidRootPart");
    std::string err;
    REQUIRE(part->reparent(model.get(), &err));
    REQUIRE(model->reparent(workspace, &err));
    REQUIRE(player->set_prop("Character",
                             rbx::Variant(static_cast<rbx::Instance*>(model.get())),
                             &err));
    CHECK_FALSE(live.local_character_replicated());

    part->set_network_identity(1, 0x555);
    CHECK(live.local_character_replicated());
}

TEST_CASE("live runtime: join watchdog separates 0x97 from a streamed world") {
    // A placed join whose 0x97 lands but whose snapshot never streams must be
    // distinguishable from one that has not been answered at all: the session
    // watchdog drops the first and waits on the second.
    const fs::path root(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root / "run" / "join97_1818.raw");
    std::vector<uint8_t> compressed(ZSTD_compressBound(inner.size()));
    const size_t csize = ZSTD_compress(compressed.data(), compressed.size(),
                                       inner.data(), inner.size(), 3);
    REQUIRE(!ZSTD_isError(csize));
    compressed.resize(csize);
    std::vector<uint8_t> body{0x97, 0xa3, 0xcf, 0x00, 0x00};
    for (int i = 0; i < 4; ++i)
        body.push_back(uint8_t((inner.size() >> (8 * i)) & 0xff));
    body.insert(body.end(), compressed.begin(), compressed.end());

    rbxclient::LiveRuntime live("unit-stall", [](const std::string&) {});
    CHECK_FALSE(live.join_data_seen());
    live.feedMessage(body.data(), body.size());
    live.pump(1.0 / 60.0);
    CHECK_FALSE(live.failed());
    CHECK(live.join_data_seen());
    CHECK_FALSE(live.has_world());
    CHECK(live.ms_since_join_data() >= 0);
    CHECK(live.ms_since_progress() >= 0);

    // The snapshot turns the same session into a live world; the watchdog is
    // disarmed by has_world(), not by the presence of 0x97.
    std::vector<uint8_t> stream;
    for (const char* fr : {"frame012.bin", "frame030.bin"}) {
        const auto f = slurp(root / "tests" / "vectors" / "repl" / fr);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    live.feed(stream.data(), stream.size());
    live.feedMessage((const uint8_t*)"\x84\x01\x00\x00\x00", 5);
    live.pump(1.0 / 60.0);
    CHECK(live.has_world());
}

TEST_CASE("live runtime: a 0x78 script bulk executes in the persistent Engine") {
    // Synthetic-but-wire-faithful chain: a 0x02 LocalScript spawn, then a
    // typeless 0x78 bulk carrying a standard Luau chunk ("return 42"). This
    // is the path a server uses for a LocalScript whose bytecode is not in
    // the client cache; the capture fixtures themselves carry no bulks.
    const fs::path root(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root / "run" / "join97_1818.raw");
    std::vector<uint8_t> compressed(ZSTD_compressBound(inner.size()));
    const size_t csize = ZSTD_compress(compressed.data(), compressed.size(),
                                       inner.data(), inner.size(), 3);
    REQUIRE(!ZSTD_isError(csize));
    compressed.resize(csize);
    std::vector<uint8_t> join_body{0x97, 0xa3, 0xcf, 0x00, 0x00};
    for (int i = 0; i < 4; ++i)
        join_body.push_back(uint8_t((inner.size() >> (8 * i)) & 0xff));
    join_body.insert(join_body.end(), compressed.begin(), compressed.end());

    std::vector<uint8_t> stream;
    auto add = [&](const std::vector<uint8_t>& payload) {
        rbx::compactVarint(stream, payload.size());
        stream.insert(stream.end(), payload.begin(), payload.end());
    };
    add(join_body);
    for (const char* fr : {"frame012.bin", "frame030.bin"}) {
        const auto f = slurp(root / "tests" / "vectors" / "repl" / fr);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    add({0x84, 0x01, 0x00, 0x00, 0x00});

    // Resolve the schema's Script class id without hardcoding a rolling build.
    rbxclient::JoinSchema schema;
    std::string err;
    REQUIRE(rbxclient::parseJoinSchema(inner.data(), inner.size(), schema,
                                       &err));
    uint16_t script_cls = 0xffff;
    for (size_t i = 0; i < schema.classes.size(); ++i) {
        if (schema.classes[i].name == "LocalScript") {
            script_cls = uint16_t(i);
            break;
        }
    }
    REQUIRE(script_cls != 0xffff);
    size_t src_member = 255;
    for (size_t i = 0; i < schema.classes[script_cls].members.size(); ++i)
        if (schema.classes[script_cls].members[i].name == "Source") {
            src_member = i;
            break;
        }
    REQUIRE(src_member < 255);

    const std::vector<char> bytecode = compile("return 42");
    const uint32_t script_id = 424242;
    // Source hash H: the spawn carries it (tag 47); the bulk below is keyed
    // with its trailing 8-byte half, which is the older standalone framing
    // (an inline bulk would key the full 16 bytes and be parsed with the
    // spawn itself).
    const uint8_t hash[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                              0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};
    std::vector<uint8_t> chain{0x83, 0x02};
    rbx::compactVarint(chain, 1); // non-null ref lo
    for (int i = 0; i < 4; ++i)
        chain.push_back(uint8_t((script_id >> (8 * i)) & 0xff));
    chain.push_back(uint8_t(script_cls & 0xff));
    chain.push_back(uint8_t(script_cls >> 8));
    chain.push_back(uint8_t(src_member)); // loop A: Source
    // Loop values carry no tag byte: the schema's member type is the reader
    // (Source is tag 47, a 16-byte hash).
    chain.insert(chain.end(), hash, hash + 16);
    chain.push_back(0xff); // prop loop A end
    chain.push_back(0xff); // prop loop B end
    chain.push_back(0x00); // parent ref null -> root
    // Typeless bulk: 8B source-key suffix, 00, varint len, payload, anchor.
    chain.insert(chain.end(), hash + 8, hash + 16);
    chain.push_back(0x00);
    rbx::compactVarint(chain, bytecode.size());
    chain.insert(chain.end(), bytecode.begin(), bytecode.end());
    chain.push_back(0x00); // anchor ref null
    chain.push_back(0x00); // chain end
    add(chain);

    rbxclient::LiveRuntime live("unit-script", [](const std::string&) {});
    live.feed(stream.data(), stream.size());
    live.pump(1.0 / 60.0);
    CHECK_FALSE(live.failed());
    CHECK(live.has_world());
    CHECK(live.stats().find("bulks=1") != std::string::npos);
    CHECK(live.scripts_run() == 1);
    CHECK(live.scripts_failed() == 0);
}

TEST_CASE("RemoteEvent FireServer emits the native wire frame") {
    Env env;
    lua_State* L = env.L();
    const rbx::ClassInfo* rcls = rbx::find_class("RemoteEvent");
    REQUIRE(rcls != nullptr);
    rbx::Ref<rbx::Instance> remote = rbx::instantiate(rcls);
    REQUIRE(remote.get() != nullptr);
    remote->set_network_identity(1, 0x36bd);
    rbx::Instance::push(L, remote.get());
    lua_setglobal(L, "remote");

    const rbx::ClassInfo* pcls = rbx::find_class("Part");
    REQUIRE(pcls != nullptr);
    rbx::Ref<rbx::Instance> part = rbx::instantiate(pcls);
    part->set_network_identity(1, 14);
    rbx::Instance::push(L, part.get());
    lua_setglobal(L, "part");

    std::vector<uint8_t> got;
    rbx::set_remote_send(
        [](void* user, const uint8_t* d, size_t n, bool unreliable) {
            CHECK_FALSE(unreliable);
            auto* v = static_cast<std::vector<uint8_t>*>(user);
            v->assign(d, d + n);
            return true;
        },
        &got);
    env.exec(R"(
        remote:FireServer(
            {x = 1, y = "two"},
            {10, 20},
            nil,
            Vector3.new(1, 2, 3),
            CFrame.new(4, 5, 6),
            Color3.new(1, 0, 0),
            Enum.Material.Plastic,
            UDim2.new(1, 2, 3, 4),
            part)
    )");
    // Exact capture: native FireSuperball call, place 1818 (2026-11-05).
    const std::vector<uint8_t> want = rbx::hexDecode(
        "830701bd360000bb11091f0201780c000000000000f03f0179020374776f"
        "1e020c00000000000024400c00000000000034400016"
        "0000803f0000004000004040"
        "1b000080400000a0400000c04002"
        "130000803f0000000000000000"
        "0731018002"
        "0e0000803f020000000000404004000000"
        "1c010e000000"
        "00");
    INFO("got  " << rbx::hexEncode(got.data(), got.size()));
    INFO("want " << rbx::hexEncode(want.data(), want.size()));
    CHECK(got == want);
    rbx::set_remote_send(nullptr, nullptr);
}

TEST_CASE("RemoteEvent CFrame uses the native quantized quaternion") {
    // CFrame.Angles(0.3, 0.4, 0.5), captured natively as kind 0 +
    // 3e 11 4d 18 f9 c6. The matrix below is the same rotation; the
    // encoder must reproduce the captured quantization exactly.
    rbx::CFrame cf;
    cf.x = 4;
    cf.y = 5;
    cf.z = 6;
    cf.r00 = 0.8083072899913;
    cf.r01 = -0.4415801564447;
    cf.r02 = 0.3894183423087;
    cf.r10 = 0.5590067178409;
    cf.r11 = 0.7832141862730;
    cf.r12 = -0.2721920623681;
    cf.r20 = -0.1848031461276;
    cf.r21 = 0.4377020695733;
    cf.r22 = 0.8799226752277;
    std::vector<uint8_t> body;
    rbx::encode_cframe_body(cf, &body);
    CHECK(body == rbx::hexDecode("000080400000a0400000c040003e114d18f9c6"));
}

TEST_CASE("Font and FontFace exist even when the GUI tree is off") {
    // A replicated LocalScript constructs Font values and sets FontFace on a
    // TextLabel regardless of whether the client draws anything. Dropping the
    // datatype with the --gui tree turned script_12097 line 72 into
    // "attempt to index nil with 'new'".
    rbxch::Options o;
    o.profile = rbxch::Profile::Engine;
    o.gui = false;
    rbxch::engine::Environment* e = rbxch::engine::create(o);
    REQUIRE(e != nullptr);
    const std::vector<char> bc = compile(R"(
        fontType = typeof(Font.new(Enum.Font.SourceSans))
        family = Font.new("rbxasset://fonts/families/ComicNeueAngular.json").Family
        local label = Instance.new("TextLabel")
        label.FontFace = Font.new(Enum.Font.SourceSansBold)
        fontFaceType = typeof(label.FontFace)
    )");
    const rbxch::Result r = rbxch::engine::execute(*e, bc.data(), bc.size());
    INFO("error: " << r.error);
    CHECK(r.status == rbxch::Status::Ok);
    lua_State* L = rbxch::engine::state(*e);
    lua_getglobal(L, "fontType");
    CHECK(std::string(lua_tostring(L, -1) ? lua_tostring(L, -1) : "") == "Font");
    lua_pop(L, 1);
    lua_getglobal(L, "family");
    CHECK(std::string(lua_tostring(L, -1) ? lua_tostring(L, -1) : "") ==
          "rbxasset://fonts/families/ComicNeueAngular.json");
    lua_pop(L, 1);
    lua_getglobal(L, "fontFaceType");
    CHECK(std::string(lua_tostring(L, -1) ? lua_tostring(L, -1) : "") == "Font");
    lua_pop(L, 1);
    rbxch::engine::destroy(e);
}

TEST_CASE("engine attach: game is the DataModel instance") {
    // The live client must expose the native DataModel shape: game has
    // ClassName/IsA/children, and game:GetService returns the same Workspace
    // instance as the global. A plain Lua table cannot answer any of these.
    rbxch::Options o;
    o.profile = rbxch::Profile::Engine;
    rbxch::engine::Environment* e = rbxch::engine::create(o);
    REQUIRE(e != nullptr);
    rbx::Ref<rbx::Instance> root =
        rbx::instantiate(rbx::find_class("DataModel"));
    rbx::Ref<rbx::Instance> ws = rbx::instantiate(rbx::find_class("Workspace"));
    REQUIRE(root);
    REQUIRE(ws);
    ws->set_name("Workspace");
    std::string err;
    REQUIRE(ws->reparent(root.get(), &err));
    rbxch::engine::attach(*e, root.get());
    const std::vector<char> bc = compile(R"(
        gameOk = game.ClassName == "DataModel" and game:IsA("DataModel")
        svcOk = game:GetService("Workspace") == workspace
        -- Native DataModel holds every service as a child (the client-only
        -- ones are adopted from the engine registry), so the replicated
        -- Workspace is one child among them and dot access resolves both.
        kidsOk = game:FindFirstChild("Workspace") == workspace
                 and game.Workspace == workspace
                 and #game:GetChildren() > 1
        jobOk = type(game.JobId) == "string"
    )");
    const rbxch::Result r = rbxch::engine::execute(*e, bc.data(), bc.size());
    INFO("error: " << r.error);
    CHECK(r.status == rbxch::Status::Ok);
    lua_State* L = rbxch::engine::state(*e);
    CHECK(gbool(L, "gameOk"));
    CHECK(gbool(L, "svcOk"));
    CHECK(gbool(L, "kidsOk"));
    CHECK(gbool(L, "jobOk"));
    ws.reset();
    root.reset();
    rbxch::engine::destroy(e);
}

TEST_CASE("engine attach: services are DataModel children (game.Players)") {
    // Native DataModel holds every service as a child, so dot indexing works
    // for services the server never replicates (RunService, UserInputService).
    rbxch::Options o;
    o.profile = rbxch::Profile::Engine;
    rbxch::engine::Environment* e = rbxch::engine::create(o);
    REQUIRE(e != nullptr);
    rbx::Ref<rbx::Instance> root =
        rbx::instantiate(rbx::find_class("DataModel"));
    rbx::Ref<rbx::Instance> players =
        rbx::instantiate(rbx::find_class("Players"));
    REQUIRE(root);
    REQUIRE(players);
    players->set_name("Players");
    std::string err;
    REQUIRE(players->reparent(root.get(), &err));
    rbxch::engine::attach(*e, root.get());
    const std::vector<char> bc = compile(R"(
        playersDot = game.Players ~= nil and game.Players.ClassName == "Players"
        playersSame = game:GetService("Players") == game.Players
        runDot = game.RunService ~= nil and game.RunService.ClassName == "RunService"
        runSame = game:GetService("RunService") == game.RunService
        runChild = game:FindFirstChild("RunService") == game.RunService
    )");
    const rbxch::Result r = rbxch::engine::execute(*e, bc.data(), bc.size());
    INFO("error: " << r.error);
    CHECK(r.status == rbxch::Status::Ok);
    lua_State* L = rbxch::engine::state(*e);
    CHECK(gbool(L, "playersDot"));
    CHECK(gbool(L, "playersSame"));
    CHECK(gbool(L, "runDot"));
    CHECK(gbool(L, "runSame"));
    CHECK(gbool(L, "runChild"));
    players.reset();
    root.reset();
    rbxch::engine::destroy(e);
}

TEST_CASE("joindata: script Source hash becomes the content key") {
    // Every replicated script carries Source as a 16-byte content hash
    // (tag 47), not a string. Dropping those bytes leaves the client unable
    // to attribute the 0x78 script-data bulk that follows, which is how the
    // native client gets LocalScript bytecode. The apply pass records the
    // hash (both halves) without inventing a script-visible property.
    rbxclient::JoinSchema schema;
    schema.classes.resize(1);
    schema.classes[0].name = "LocalScript";
    rbx::register_builtin_classes();
    const uint8_t hash[16] = {0x1e, 0xb4, 0xf7, 0xb9, 0x75, 0x03, 0x5d, 0xce,
                              0x9a, 0x0c, 0x42, 0x8c, 0xac, 0xce, 0xcb, 0xb6};
    rbxclient::JoinRecord rec;
    rec.lo = 1;
    rec.id = 44215;
    rec.cls = 0;
    rec.plo = 1;
    rec.parent = 13; // implicit DataModel root
    rbxclient::JoinProp src;
    src.name = "Source";
    src.tag = 0x2f; // content hash
    src.value.assign(hash, hash + sizeof hash);
    rec.props.push_back(src);
    std::string err;
    rbxclient::ApplyReport rep =
        rbxclient::applyJoinSnapshot(schema, {rec}, &err);
    REQUIRE(rep.root);
    CHECK(err.empty());
    CHECK(rep.stats.source_hashes == 1);
    CHECK(rep.stats.props_skipped_undeclared == 0);
    CHECK(rep.source_hash.size() == 1);
    const std::string full(reinterpret_cast<const char*>(hash), 16);
    REQUIRE(rep.source_hash.count(full) == 1);
    CHECK(rep.source_hash[full] == 44215);
    CHECK(rep.source_half.count(full.substr(0, 8)) == 1);
    CHECK(rep.source_half.count(full.substr(8)) == 1);
}

TEST_CASE("engine: require runs a ModuleScript once with script bound") {
    // The native client never auto-runs a ModuleScript: it keeps the chunk and
    // executes it on the first require, binding `script` to the module and
    // caching the return value per ModuleScript. Adopt Me's loader requires
    // its sibling AwaitPersistentModels module at line 16, so this is a real
    // experience path, not a synthetic one.
    Env env;
    rbx::Ref<rbx::Instance> root =
        rbx::instantiate(rbx::find_class("DataModel"));
    REQUIRE(root);
    rbxch::engine::attach(*env.e, root.get());

    env.exec(R"(
        local holder = Instance.new("Folder")
        holder.Name = "Holder"
        holder.Parent = game
        local mod = Instance.new("ModuleScript")
        mod.Name = "M"
        mod.Parent = holder
    )");
    rbx::Instance* holder = root->find_first_child_of_class("Folder");
    REQUIRE(holder != nullptr);
    rbx::Instance* mod = holder->find_first_child_of_class("ModuleScript");
    REQUIRE(mod != nullptr);

    const std::vector<char> modbc = compile(R"(
        runs = (runs or 0) + 1
        return { value = 41 + 1, name = script.Name, runs = runs }
    )");
    rbxch::engine::set_module_source(
        *env.e, mod, std::vector<uint8_t>(modbc.begin(), modbc.end()));

    env.exec(R"(
        local m1 = require(game:FindFirstChild("Holder").M)
        local m2 = require(game:FindFirstChild("Holder").M)
        same = (m1 == m2)
        value = m1.value
        bound = m1.name
        runs = m1.runs
    )");
    CHECK(gbool(env.L(), "same"));
    CHECK(gnum(env.L(), "value") == 42.0);
    CHECK(gstr(env.L(), "bound") == "M");
    CHECK(gnum(env.L(), "runs") == 1.0); // cached: the chunk ran exactly once
}

TEST_CASE("engine: ControlState typed fields and player accessor") {
    // ControlState is the schema/value API the native PlayerModule uses for
    // the 141-byte 0xA6 input frame. Values must be typed, snapshot-able and
    // reachable from a Player-owned PlayerControlState.
    Env env;
    rbx::Ref<rbx::Instance> player =
        rbx::instantiate(rbx::find_class("Player"));
    REQUIRE(player);
    rbx::Instance::push(env.L(), player.get());
    lua_setglobal(env.L(), "player");

    env.exec(R"(
        local cs = Instance.new("ControlState")
        cs:AddBoolField("jump", false)
        cs:AddUnitVector3Field("move", Vector3.new(0, 0, 0))
        cs:AddIntField("mode", 1, 0, 3)
        cs:AddVector3Field("velocity", Vector3.new(0, 0, 0), 10)
        hasMove = cs:HasField("move")
        hasNope = cs:HasField("nope")
        initialJump = cs:GetState().jump
        events = 0
        cs.OnStateChanged:Connect(function() events = events + 1 end)
        cs:SetField("jump", true)
        cs:UpdateFields({move = Vector3.new(0, 0, -1), mode = 2})
        local changed = cs:GetChangedState()
        changedMoveZ = changed.move.Z
        changedMode = changed.mode
        changedJump = changed.jump == nil
        afterJump = cs:GetState().jump
        weight = cs:GetReplicationWeight()
        local pcs = player:GetControlState()
        pcs:AddBoolField("jump", false)
        pcs:SetField("jump", true)
        pcsJump = pcs:GetState().jump
        samePcs = player:GetControlState() == pcs
    )");
    CHECK(gbool(env.L(), "hasMove"));
    CHECK_FALSE(gbool(env.L(), "hasNope"));
    CHECK_FALSE(gbool(env.L(), "initialJump"));
    CHECK(gnum(env.L(), "events") == 2.0);
    CHECK(gnum(env.L(), "changedMoveZ") == -1.0);
    CHECK(gnum(env.L(), "changedMode") == 2.0);
    CHECK(gbool(env.L(), "changedJump"));
    CHECK(gbool(env.L(), "afterJump"));
    CHECK(gnum(env.L(), "weight") == 1.0);
    CHECK(gbool(env.L(), "pcsJump"));
    CHECK(gbool(env.L(), "samePcs"));
}

TEST_CASE("FaceControls: FACS writes clamp to 0..1") {
    Env env;
    env.exec(R"(
        local face = Instance.new("FaceControls")
        face.JawDrop = 1.5
        high = face.JawDrop
        face.JawDrop = -0.25
        low = face.JawDrop
        face.JawDrop = 0.4
        mid = face.JawDrop
        local head = Instance.new("Part")
        face.Parent = head
        parented = face.Parent == head
        notifications = 0
        face.Changed:Connect(function() notifications = notifications + 1 end)
        face.JawDrop = 2
        changed = notifications
    )");
    lua_State* L = env.L();
    CHECK(gnum(L, "high") == doctest::Approx(1.0));
    CHECK(gnum(L, "low") == doctest::Approx(0.0));
    CHECK(gnum(L, "mid") == doctest::Approx(0.4));
    CHECK(gbool(L, "parented"));
    CHECK(gnum(L, "changed") == 1.0);
}

TEST_CASE("FaceControls: every documented FACS property is clamped") {
    const rbx::ClassInfo* cls = rbx::find_class("FaceControls");
    REQUIRE(cls != nullptr);
    rbx::Ref<rbx::Instance> face = rbx::instantiate(cls);
    REQUIRE(face);

    int numeric = 0;
    for (const rbx::PropInfo& p : rbx::effective_properties(cls)) {
        if (p.type != rbx::PropType::Double)
            continue;
        std::string err;
        REQUIRE(face->set_prop(p.name, rbx::Variant(2.5), &err));
        rbx::Variant out;
        REQUIRE(face->get_prop(p.name, out));
        const auto* value = std::get_if<double>(&out);
        REQUIRE(value != nullptr);
        CHECK(*value == doctest::Approx(1.0));

        REQUIRE(face->set_prop(p.name, rbx::Variant(-2.5), &err));
        REQUIRE(face->get_prop(p.name, out));
        value = std::get_if<double>(&out);
        REQUIRE(value != nullptr);
        CHECK(*value == doctest::Approx(0.0));
        ++numeric;
    }
    CHECK(numeric == 50);
}

TEST_CASE("live runtime: local Player reparent and delete preserve the A6 ref") {
    // The local Player is client-owned: a stream parent update may move it
    // out of Players, and a replicated delete must not remove it. Both paths
    // used to leave a second, wire-less Player under Players, which made
    // local_player_ref() return 0 and forced A6 onto the short frame.
    const fs::path root_dir(REPO_ROOT);
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        REQUIRE(f);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    };
    const auto inner = slurp(root_dir / "run" / "join97_1818.raw");
    rbxclient::JoinSchema schema;
    std::string err;
    REQUIRE(rbxclient::parseJoinSchema(inner.data(), inner.size(), schema,
                                       &err));

    size_t player_cls = schema.classes.size();
    for (size_t i = 0; i < schema.classes.size(); ++i)
        if (schema.classes[i].name == "Player") {
            player_cls = i;
            break;
        }
    REQUIRE(player_cls < schema.classes.size());
    size_t user_id_member = 255;
    for (size_t i = 0; i < schema.classes[player_cls].members.size(); ++i)
        if (schema.classes[player_cls].members[i].name == "UserId") {
            user_id_member = i;
            break;
        }
    REQUIRE(user_id_member < 255);
    REQUIRE(schema.classes[player_cls].members[user_id_member].tag == 44);

    std::vector<uint8_t> compressed(ZSTD_compressBound(inner.size()));
    const size_t csize = ZSTD_compress(compressed.data(), compressed.size(),
                                       inner.data(), inner.size(), 3);
    REQUIRE(!ZSTD_isError(csize));
    compressed.resize(csize);
    std::vector<uint8_t> join_body{0x97, 0xa3, 0xcf, 0x00, 0x00};
    for (int i = 0; i < 4; ++i)
        join_body.push_back(uint8_t((inner.size() >> (8 * i)) & 0xff));
    join_body.insert(join_body.end(), compressed.begin(), compressed.end());

    std::vector<uint8_t> stream;
    auto add = [&](const std::vector<uint8_t>& payload) {
        rbx::compactVarint(stream, payload.size());
        stream.insert(stream.end(), payload.begin(), payload.end());
    };
    add(join_body);
    for (const char* fr : {"frame012.bin", "frame030.bin"}) {
        const auto f = slurp(root_dir / "tests" / "vectors" / "repl" / fr);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    add({0x84, 0x01, 0x00, 0x00, 0x00});

    struct LocalUserReset {
        long long id;
        std::string name;
        ~LocalUserReset() {
            rbx::set_local_user_id(id);
            rbx::set_local_user_name(name);
        }
    } reset{rbx::local_user_id(), rbx::local_user_name()};

    const uint32_t local_ref = 424242;
    rbxclient::LiveRuntime live("unit-local-owner", [](const std::string&) {},
                                false, false);
    live.setLocalUser(42, "A6Tester", "A6Tester");
    live.feed(stream.data(), stream.size());
    live.pump(1.0 / 60.0);
    REQUIRE(live.has_world());
    REQUIRE(live.root() != nullptr);

    auto put32 = [](std::vector<uint8_t>& b, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b.push_back(uint8_t((v >> (8 * i)) & 0xff));
    };
    auto local_player_in_players = [&]() -> const rbx::Instance* {
        const rbx::Instance* players =
            live.root()->find_first_child_of_class("Players");
        if (!players) return nullptr;
        for (const auto& child : players->children()) {
            if (!child->is_a("Player")) continue;
            rbx::Variant v;
            if (!child->get_prop("UserId", v)) continue;
            const auto* id = std::get_if<int64_t>(&v);
            if (id && *id == 42) return child.get();
        }
        return nullptr;
    };

    // A Player record for us with a null parent is what a streamed parent
    // update does: applyStreamRecord roots it, then syncPlayers must find the
    // wire-backed instance and put it back under Players, not spawn a twin.
    std::vector<uint8_t> spawn{0x83, 0x02, 0x01}; // chain, spawn, ref lo=1
    put32(spawn, local_ref);
    spawn.push_back(uint8_t(player_cls & 0xff));
    spawn.push_back(uint8_t(player_cls >> 8));
    spawn.push_back(uint8_t(user_id_member));
    spawn.push_back(0x54); // UserId 42 as zigzag varint
    spawn.push_back(0xff); // loop A end
    spawn.push_back(0xff); // loop B end
    spawn.push_back(0x00); // null parent -> DataModel root
    spawn.push_back(0x00); // chain end
    live.feedMessage(spawn.data(), spawn.size());
    live.pump(1.0 / 60.0);

    CHECK(live.hasInstance(local_ref));
    CHECK(live.local_player_ref() == local_ref);
    REQUIRE(local_player_in_players() != nullptr);

    // The same ref is the local Player's wire identity. A replicated delete
    // must be ignored so A6 keeps addressing this instance.
    std::vector<uint8_t> del{0x83, 0x01, 0x01};
    put32(del, local_ref);
    del.push_back(0x00); // chain end
    live.feedMessage(del.data(), del.size());
    live.pump(1.0 / 60.0);
    CHECK(live.hasInstance(local_ref));
    CHECK(live.local_player_ref() == local_ref);
    CHECK(local_player_in_players() != nullptr);
    CHECK_FALSE(live.failed());
}

TEST_CASE("Instance tags, Model transforms and BasePart/Camera queries") {
    Env env;
    env.exec(R"(
        part = Instance.new("Part")
        part:AddTag("Red")
        tagAdded = part:HasTag("Red")
        tagCount = #part:GetTags()
        part:RemoveTag("Red")
        tagRemoved = not part:HasTag("Red")

        model = Instance.new("Model")
        model.Parent = workspace
        p = Instance.new("Part")
        p.CFrame = CFrame.new(1, 2, 3)
        p.Parent = model
        model.PrimaryPart = p
        model:PivotTo(CFrame.new(10, 20, 30))
        pivotX = p.Position.X
        model:TranslateBy(Vector3.new(1, 2, 3))
        translateZ = p.Position.Z
        model:ScaleTo(2)
        scaledX = p.Size.X
        scaledScale = model:GetScale()
        hasPrimary = model:GetPrimaryPart() == p

        a = Instance.new("Part")
        a.Size = Vector3.new(4, 1, 2)
        a.CanCollide = true
        b = Instance.new("Part")
        b.Size = Vector3.new(2, 2, 2)
        b.CFrame = CFrame.new(1, 0, 0)
        b.CanCollide = true
        a.Parent = model
        b.Parent = model
        massOk = a:GetMass() > 0
        touchCount = #a:GetTouchingParts()
        closestX = a:GetClosestPointOnSurface(Vector3.new(10, 0, 0)).X
        canCollide = a:CanCollideWith(b)
        rootOk = a:GetRootPart() == a

        -- Overlap without a joint must not enter the assembly graph.
        d = Instance.new("Part")
        d.Size = Vector3.new(2, 2, 2)
        d.CFrame = CFrame.new(0.2, 0, 0)
        d.Parent = model

        w = Instance.new("Weld")
        w.Part0 = a
        w.Part1 = b
        w.Parent = model
        c = Instance.new("Part")
        c.Size = Vector3.new(2, 2, 2)
        c.CFrame = CFrame.new(0, 0, 10)
        c.Parent = model
        wc = Instance.new("WeldConstraint")
        wc.Part0 = b
        wc.Part1 = c
        wc.Parent = model
        disabled = Instance.new("Weld")
        disabled.Name = "JointProbeDisabled"
        disabled.Part0 = a
        disabled.Part1 = d
        disabled.Enabled = false
        disabled.Parent = model

        connected = #a:GetConnectedParts()
        connectedRecursive = #a:GetConnectedParts(true)
        joints = #a:GetJoints()
        bJoints = #b:GetJoints()
        weldFirst = a:GetJoints()[1] == w
        enabledDefaults = w.Enabled and wc.Enabled and not disabled.Enabled

        -- Joints outside a Workspace/WorldModel are attached but inactive.
        loose = Instance.new("Model")
        la = Instance.new("Part")
        la.Size = Vector3.new(2, 2, 2)
        la.Parent = loose
        lb = Instance.new("Part")
        lb.Size = Vector3.new(2, 2, 2)
        lb.Parent = loose
        lw = Instance.new("Weld")
        lw.Part0 = la
        lw.Part1 = lb
        lw.Parent = loose
        looseConnected = #la:GetConnectedParts()
        looseJoints = #la:GetJoints()
        grounded = a:IsGrounded()

        cam = Instance.new("Camera")
        cam.CFrame = CFrame.new(0, 0, 0)
        cam.FieldOfView = 90
        vp = cam:WorldToViewportPoint(Vector3.new(0, 0, -10))
        sp, onScreen = cam:WorldToScreenPoint(Vector3.new(0, 0, -10))
        ray = cam:ViewportPointToRay(640, 360)

        ts = game:GetService("TeleportService")
        guiOk, gui = pcall(function() return ts:GetArrivingTeleportGui() end)
        guiType = typeof(gui)
        guiIsNil = gui == nil
        noArrivingGui = guiOk and guiIsNil
        noTeleportData = ts:GetLocalPlayerTeleportData() == nil
        ts:SetTeleportSetting("X", 7)
        setting = ts:GetTeleportSetting("X")
        teleportFailed = ts:Teleport(1) == Enum.TeleportResult.Failure
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "tagAdded"));
    CHECK(gnum(L, "tagCount") == 1);
    CHECK(gbool(L, "tagRemoved"));
    CHECK(gnum(L, "pivotX") == 10);
    CHECK(gnum(L, "translateZ") == 33);
    CHECK(gnum(L, "scaledX") == 8);
    CHECK(gnum(L, "scaledScale") == 2);
    CHECK(gbool(L, "hasPrimary"));
    CHECK(gbool(L, "massOk"));
    CHECK(gnum(L, "touchCount") == 1);
    CHECK(gnum(L, "connected") == 2);
    CHECK(gnum(L, "connectedRecursive") == 3);
    CHECK(gnum(L, "closestX") == 2);
    CHECK(gbool(L, "canCollide"));
    CHECK(gbool(L, "rootOk"));
    CHECK(gnum(L, "joints") == 2);
    CHECK(gnum(L, "bJoints") == 2);
    CHECK(gbool(L, "weldFirst"));
    CHECK(gbool(L, "enabledDefaults"));
    CHECK(gnum(L, "looseConnected") == 1);
    CHECK(gnum(L, "looseJoints") == 1);
    CHECK_FALSE(gbool(L, "grounded"));
    CHECK(gbool(L, "guiOk"));
    CHECK(gstr(L, "guiType") == "nil");
    CHECK(gbool(L, "guiIsNil"));
    CHECK(gbool(L, "noArrivingGui"));
    CHECK(gbool(L, "noTeleportData"));
    CHECK(gnum(L, "setting") == 7);
    CHECK(gbool(L, "teleportFailed"));
    // `vp`/`sp`/`ray` are intentionally exercised for no-error execution;
    // their numeric projection is covered by the camera math below.
    env.exec(R"(
        cameraOk = vp.Z == 10 and sp.X == 640 and sp.Y == 360 and onScreen
        rayOk = math.abs(ray.Direction.Z + 1) < 0.0001
        noObscurers = #cam:GetPartsObscuringTarget({}, {}) == 0
    )");
    CHECK(gbool(L, "cameraOk"));
    CHECK(gbool(L, "rayOk"));
    CHECK(gbool(L, "noObscurers"));
}

TEST_CASE("BasePart assembly root follows native precedence") {
    Env env;
    env.exec(R"(
        local ws = workspace

        local function makePart(name, size, anchored, massless, parent)
            local p = Instance.new("Part")
            p.Name = name
            p.Size = size
            p.Anchored = anchored or false
            p.Massless = massless or false
            p.CanCollide = false
            p.Parent = parent or ws
            return p
        end

        local function weld(a, b)
            local j = Instance.new("Weld")
            j.Part0 = a
            j.Part1 = b
            j.Parent = a.Parent
            return j
        end

        local isolated = makePart("RootIsolated", Vector3.new(2, 2, 2))
        isolatedRoot = isolated:GetRootPart() == isolated
        isolatedAssembly = isolated.AssemblyRootPart == isolated
        isolatedConnected = #isolated:GetConnectedParts() == 1 and
                            isolated:GetConnectedParts()[1] == isolated

        local anchor = makePart("RootChainAnchor", Vector3.new(2, 2, 2), true)
        local mid = makePart("RootChainMid", Vector3.new(2, 2, 2))
        local tail = makePart("RootChainTail", Vector3.new(2, 2, 2))
        weld(anchor, mid)
        weld(mid, tail)
        chainRoot = mid:GetRootPart() == anchor and
                    tail:GetRootPart() == anchor

        local large = makePart("RootLargest", Vector3.new(12, 12, 12))
        local small = makePart("RootSmall", Vector3.new(2, 2, 2))
        weld(large, small)
        largestRoot = small:GetRootPart() == large

        local priority = makePart("RootPriority", Vector3.new(1, 1, 1))
        priority.RootPriority = 100
        local priorityPeer = makePart("RootPriorityPeer", Vector3.new(20, 20, 20))
        weld(priority, priorityPeer)
        priorityRoot = priorityPeer:GetRootPart() == priority

        local massless = makePart("RootMassless", Vector3.new(20, 20, 20),
                                  false, true)
        local solid = makePart("RootSolid", Vector3.new(2, 2, 2))
        weld(massless, solid)
        masslessRoot = massless:GetRootPart() == solid

        local humanoidModel = Instance.new("Model")
        humanoidModel.Name = "RootHumanoidModel"
        humanoidModel.Parent = ws
        local hrp = makePart("HumanoidRootPart", Vector3.new(2, 2, 1),
                             false, false, humanoidModel)
        local torso = makePart("Torso", Vector3.new(2, 2, 1),
                               false, false, humanoidModel)
        weld(hrp, torso)
        local humanoid = Instance.new("Humanoid")
        humanoid.Parent = humanoidModel
        humanoidRoot = hrp:GetRootPart() == hrp and
                       torso:GetRootPart() == hrp
        humanoidProperty = humanoid.RootPart == hrp

        local mutualOne = makePart("RootMutualOne", Vector3.new(2, 2, 2))
        local mutualTwo = makePart("RootMutualTwo", Vector3.new(2, 2, 2))
        weld(mutualOne, mutualTwo)
        weld(mutualTwo, mutualOne)
        mutualRoot = mutualOne:GetRootPart() == mutualTwo and
                     mutualTwo:GetRootPart() == mutualTwo

        local primaryModel = Instance.new("Model")
        primaryModel.Name = "RootPrimaryModel"
        primaryModel.Parent = ws
        local primary = makePart("RootPrimary", Vector3.new(2, 2, 2),
                                 false, false, primaryModel)
        local primaryLarge = makePart("RootPrimaryLarge",
                                      Vector3.new(12, 12, 12),
                                      false, false, primaryModel)
        weld(primary, primaryLarge)
        primaryModel.PrimaryPart = primary
        primaryIgnored = primary:GetRootPart() == primaryLarge and
                         primaryLarge:GetRootPart() == primaryLarge

        local anchoredOne = makePart("RootAnchoredOne",
                                     Vector3.new(1, 1, 1), true)
        local anchoredTwo = makePart("RootAnchoredTwo",
                                     Vector3.new(12, 12, 12), true)
        weld(anchoredOne, anchoredTwo)
        anchoredSplit = anchoredOne:GetRootPart() == anchoredOne and
                        anchoredTwo:GetRootPart() == anchoredTwo

        -- Equal anchors: the newer active weld owns the unanchored middle.
        -- Native root_anchor_ties T2: B-middle weld older, A-middle newer,
        -- activeA=true and middle root=A.
        local tieAnchorA = makePart("RootTieAnchorA", Vector3.new(2, 2, 2),
                                    true)
        local tieMiddle = makePart("RootTieMiddle", Vector3.new(2, 2, 2))
        local tieAnchorB = makePart("RootTieAnchorB", Vector3.new(2, 2, 2),
                                    true)
        weld(tieAnchorB, tieMiddle)
        weld(tieAnchorA, tieMiddle)
        anchorTieRoot = tieMiddle:GetRootPart() == tieAnchorA

        -- Equal anchors with the opposite weld order: newer B link wins.
        local tieAnchorC = makePart("RootTieAnchorC", Vector3.new(2, 2, 2),
                                    true)
        local tieMiddleTwo = makePart("RootTieMiddleTwo",
                                      Vector3.new(2, 2, 2))
        local tieAnchorD = makePart("RootTieAnchorD", Vector3.new(2, 2, 2),
                                    true)
        weld(tieAnchorC, tieMiddleTwo)
        weld(tieAnchorD, tieMiddleTwo)
        anchorTieNewerRoot = tieMiddleTwo:GetRootPart() == tieAnchorD

        -- Root rank still beats the active link: the larger anchor owns the
        -- middle even when the newer weld points at the small anchor.
        local sizeAnchorSmall = makePart("RootSizeAnchorSmall",
                                         Vector3.new(1, 1, 1), true)
        local sizeMiddle = makePart("RootSizeMiddle", Vector3.new(2, 2, 2))
        local sizeAnchorLarge = makePart("RootSizeAnchorLarge",
                                         Vector3.new(12, 12, 12), true)
        weld(sizeMiddle, sizeAnchorLarge)
        weld(sizeAnchorSmall, sizeMiddle)
        anchorSizeRoot = sizeMiddle:GetRootPart() == sizeAnchorLarge

        -- RootPriority beats size for anchored candidates too.
        local priorityAnchorSmall = makePart("RootPriorityAnchorSmall",
                                             Vector3.new(1, 1, 1), true)
        priorityAnchorSmall.RootPriority = 100
        local priorityAnchorMiddle = makePart("RootPriorityAnchorMiddle",
                                              Vector3.new(2, 2, 2))
        local priorityAnchorLarge = makePart("RootPriorityAnchorLarge",
                                             Vector3.new(12, 12, 12), true)
        weld(priorityAnchorMiddle, priorityAnchorLarge)
        weld(priorityAnchorSmall, priorityAnchorMiddle)
        anchorPriorityRoot =
            priorityAnchorMiddle:GetRootPart() == priorityAnchorSmall

        -- Equal anchors several joints apart: the newest anchor-incident
        -- link owns the unanchored chain (native long-chain G2).
        local chainAnchorA = makePart("RootChainAnchorA",
                                      Vector3.new(2, 2, 2), true)
        local chainOne = makePart("RootChainOne", Vector3.new(2, 2, 2))
        local chainTwo = makePart("RootChainTwo", Vector3.new(2, 2, 2))
        local chainThree = makePart("RootChainThree", Vector3.new(2, 2, 2))
        local chainAnchorB = makePart("RootChainAnchorB",
                                      Vector3.new(2, 2, 2), true)
        weld(chainThree, chainAnchorB)
        weld(chainTwo, chainThree)
        weld(chainOne, chainTwo)
        weld(chainAnchorA, chainOne)
        anchorLongChainRoot = chainTwo:GetRootPart() == chainAnchorA

        local orphan = Instance.new("Part")
        orphan.Name = "RootOrphan"
        orphanRoot = orphan:GetRootPart() == nil
        orphanAssembly = orphan.AssemblyRootPart == nil

        local outside = Instance.new("Model")
        local outsidePart = makePart("RootOutside", Vector3.new(2, 2, 2),
                                    false, false, outside)
        outsideRoot = outsidePart:GetRootPart() == nil
        outsideAssembly = outsidePart.AssemblyRootPart == nil

        local normal = makePart("RootNormalPeer", Vector3.new(2, 2, 1))
        local seat = Instance.new("Seat")
        seat.Name = "RootSeat"
        seat.Size = Vector3.new(2, 2, 1)
        seat.CanCollide = false
        seat.Parent = ws
        weld(seat, normal)
        seatRoot = normal:GetRootPart() == seat

        local torsoNamed = makePart("Torso", Vector3.new(2, 2, 1))
        local neutral = makePart("RootNeutral", Vector3.new(2, 2, 1))
        weld(torsoNamed, neutral)
        torsoNameRoot = neutral:GetRootPart() == torsoNamed
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "isolatedRoot"));
    CHECK(gbool(L, "isolatedAssembly"));
    CHECK(gbool(L, "isolatedConnected"));
    CHECK(gbool(L, "chainRoot"));
    CHECK(gbool(L, "largestRoot"));
    CHECK(gbool(L, "priorityRoot"));
    CHECK(gbool(L, "masslessRoot"));
    CHECK(gbool(L, "humanoidRoot"));
    CHECK(gbool(L, "humanoidProperty"));
    CHECK(gbool(L, "mutualRoot"));
    CHECK(gbool(L, "primaryIgnored"));
    CHECK(gbool(L, "anchoredSplit"));
    CHECK(gbool(L, "anchorTieRoot"));
    CHECK(gbool(L, "anchorTieNewerRoot"));
    CHECK(gbool(L, "anchorSizeRoot"));
    CHECK(gbool(L, "anchorPriorityRoot"));
    CHECK(gbool(L, "anchorLongChainRoot"));
    CHECK(gbool(L, "orphanRoot"));
    CHECK(gbool(L, "orphanAssembly"));
    CHECK(gbool(L, "outsideRoot"));
    CHECK(gbool(L, "outsideAssembly"));
    CHECK(gbool(L, "seatRoot"));
    CHECK(gbool(L, "torsoNameRoot"));
}

TEST_CASE("JointInstance.Active follows the native assembly forest") {
    Env env;
    env.exec(R"(
        local ws = workspace

        local function makePart(name, size, anchored, massless, parent)
            local p = Instance.new("Part")
            p.Name = name
            p.Size = size
            p.Anchored = anchored or false
            p.Massless = massless or false
            p.CanCollide = false
            p.Parent = parent or ws
            return p
        end

        local function weld(name, a, b)
            local j = Instance.new("Weld")
            j.Name = name
            j.Part0 = a
            j.Part1 = b
            j.Parent = a.Parent
            return j
        end

        local splitA = makePart("ActiveSplitA", Vector3.new(2, 2, 2), true)
        local splitM = makePart("ActiveSplitM", Vector3.new(2, 2, 2))
        local splitB = makePart("ActiveSplitB", Vector3.new(2, 2, 2), true)
        local splitWB = weld("ActiveSplitWB", splitM, splitB)
        local splitWA = weld("ActiveSplitWA", splitA, splitM)
        splitRoot = splitM:GetRootPart() == splitA
        splitActive = splitWA.Active and not splitWB.Active

        local model = Instance.new("Model")
        model.Name = "ActiveHumanoidModel"
        model.Parent = ws
        local hrp = makePart("HumanoidRootPart", Vector3.new(2, 2, 1),
                             false, false, model)
        local torso = makePart("Torso", Vector3.new(2, 2, 1),
                               false, false, model)
        local humanoidWeld = weld("ActiveHumanoidWeld", hrp, torso)
        local humanoid = Instance.new("Humanoid")
        humanoid.Parent = model
        humanoidActive = humanoidWeld.Active and
                         hrp:GetRootPart() == hrp

        local one = makePart("ActiveMutualOne", Vector3.new(2, 2, 2))
        local two = makePart("ActiveMutualTwo", Vector3.new(2, 2, 2))
        local w12 = weld("ActiveMutual12", one, two)
        local w21 = weld("ActiveMutual21", two, one)
        mutualActive = not w12.Active and w21.Active
        mutualRoot = one:GetRootPart() == two

        local massless = makePart("ActiveMassless", Vector3.new(20, 20, 20),
                                  false, true)
        local solid = makePart("ActiveSolid", Vector3.new(2, 2, 2))
        local masslessWeld = weld("ActiveMasslessWeld", massless, solid)
        masslessActive = masslessWeld.Active and solid:GetRootPart() == solid

        -- Native long-chain G1: one unanchored component between two equal
        -- anchors.  Only the newest anchor link is active; the other anchor
        -- link and all cycle-closing links are inactive.
        local chainA = makePart("ChainA", Vector3.new(2, 2, 2), true)
        local chainM1 = makePart("ChainM1", Vector3.new(2, 2, 2))
        local chainM2 = makePart("ChainM2", Vector3.new(2, 2, 2))
        local chainB = makePart("ChainB", Vector3.new(2, 2, 2), true)
        local chainJ1 = weld("ChainJ1", chainA, chainM1)
        local chainJ2 = weld("ChainJ2", chainM1, chainM2)
        local chainJ3 = weld("ChainJ3", chainM2, chainB)
        chainRoot = chainM1:GetRootPart() == chainB
        chainActive = not chainJ1.Active and chainJ2.Active and chainJ3.Active

        -- Native long-chain G2: reverse the anchor-link creation order.  The
        -- winning side and the one active anchor link reverse too.
        local chain2A = makePart("Chain2A", Vector3.new(2, 2, 2), true)
        local chain2M1 = makePart("Chain2M1", Vector3.new(2, 2, 2))
        local chain2M2 = makePart("Chain2M2", Vector3.new(2, 2, 2))
        local chain2B = makePart("Chain2B", Vector3.new(2, 2, 2), true)
        local chain2J4 = weld("Chain2J4", chain2M2, chain2B)
        local chain2J3 = weld("Chain2J3", chain2M1, chain2M2)
        local chain2J1 = weld("Chain2J1", chain2A, chain2M1)
        chain2Root = chain2M1:GetRootPart() == chain2A
        chain2Active = chain2J1.Active and chain2J3.Active and
                       not chain2J4.Active

        local anchoredOne = makePart("ActiveAnchoredOne",
                                     Vector3.new(2, 2, 2), true)
        local anchoredTwo = makePart("ActiveAnchoredTwo",
                                     Vector3.new(2, 2, 2), true)
        local anchoredWeld = weld("ActiveAnchoredWeld", anchoredOne,
                                  anchoredTwo)
        fullyAnchoredInactive = not anchoredWeld.Active and
                                anchoredOne:GetRootPart() == anchoredOne and
                                anchoredTwo:GetRootPart() == anchoredTwo

        local disabledA = makePart("ActiveDisabledA", Vector3.new(2, 2, 2))
        local disabledB = makePart("ActiveDisabledB", Vector3.new(2, 2, 2))
        local disabledWeld = weld("ActiveDisabledWeld", disabledA, disabledB)
        disabledWeld.Enabled = false
        disabledInactive = not disabledWeld.Active

        local wcA = makePart("ActiveWCA", Vector3.new(2, 2, 2))
        local wcB = makePart("ActiveWCB", Vector3.new(2, 2, 2))
        local wc = Instance.new("WeldConstraint")
        wc.Part0 = wcA
        wc.Part1 = wcB
        wc.Parent = ws
        constraintActive = wc.Active and wcA:GetRootPart() == wcB

        local outside = Instance.new("Model")
        local outsideA = makePart("ActiveOutsideA", Vector3.new(2, 2, 2),
                                  false, false, outside)
        local outsideB = makePart("ActiveOutsideB", Vector3.new(2, 2, 2),
                                  false, false, outside)
        local outsideWeld = weld("ActiveOutsideWeld", outsideA, outsideB)
        outsideInactive = not outsideWeld.Active

        local writeOk, writeErr = pcall(function() splitWA.Active = false end)
        writeRefused = not writeOk and splitWA.Active and
                       string.find(tostring(writeErr),
                                   "Unable to assign property Active. Property is read only",
                                   1, true) ~= nil

        splitWA.Enabled = false
        disabledSplitFlips = not splitWA.Active and splitWB.Active and
                             splitM:GetRootPart() == splitB
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "splitRoot"));
    CHECK(gbool(L, "splitActive"));
    CHECK(gbool(L, "humanoidActive"));
    CHECK(gbool(L, "mutualActive"));
    CHECK(gbool(L, "mutualRoot"));
    CHECK(gbool(L, "masslessActive"));
    CHECK(gbool(L, "chainRoot"));
    CHECK(gbool(L, "chainActive"));
    CHECK(gbool(L, "chain2Root"));
    CHECK(gbool(L, "chain2Active"));
    CHECK(gbool(L, "fullyAnchoredInactive"));
    CHECK(gbool(L, "disabledInactive"));
    CHECK(gbool(L, "constraintActive"));
    CHECK(gbool(L, "outsideInactive"));
    CHECK(gbool(L, "writeRefused"));
    CHECK(gbool(L, "disabledSplitFlips"));
}

TEST_CASE("Legacy surface and non-rigid joints match native Active") {
    Env env;
    env.exec(R"(
        local ws = workspace

        local function makePart(name, size, anchored, parent)
            local p = Instance.new("Part")
            p.Name = name
            p.Size = size or Vector3.new(2, 2, 2)
            p.Anchored = anchored or false
            p.CanCollide = false
            p.Parent = parent or ws
            return p
        end

        local function joint(cls, name, a, b)
            local j = Instance.new(cls)
            j.Name = name
            j.Part0 = a
            j.Part1 = b
            j.Parent = ws
            return j
        end

        -- Glue and ManualGlue are the legacy surface joints.  Native 1818
        -- roots their pair at the newer part, connects both parts and reports
        -- the newest edge active.
        local glueA = makePart("GlueA")
        local glueB = makePart("GlueB")
        local glue = joint("Glue", "GlueJ", glueA, glueB)
        glueActive = glue.Active
        glueRoot = glueA:GetRootPart() == glueB
        glueConnected = #glueA:GetConnectedParts(true) == 2

        local manualA = makePart("ManualGlueA")
        local manualB = makePart("ManualGlueB")
        local manualGlue = joint("ManualGlue", "ManualGlueJ", manualA,
                                 manualB)
        manualGlueActive = manualGlue.Active
        manualGlueRoot = manualA:GetRootPart() == manualB
        manualGlueConnected = #manualA:GetConnectedParts(true) == 2

        -- Two anchored sides are separate assemblies even through a Glue.
        local anchoredA = makePart("GlueAnchorA", nil, true)
        local anchoredB = makePart("GlueAnchorB", nil, true)
        local anchoredGlue = joint("Glue", "GlueAnchorJ", anchoredA,
                                   anchoredB)
        anchoredGlueInactive = not anchoredGlue.Active and
                               anchoredA:GetRootPart() == anchoredA and
                               anchoredB:GetRootPart() == anchoredB

        -- Rotate and VelocityMotor are the non-rigid JointInstance classes:
        -- native never connects their parts and their Active is always false.
        local rotateA = makePart("RotateA")
        local rotateB = makePart("RotateB")
        local rotate = joint("Rotate", "RotateJ", rotateA, rotateB)
        rotateInactive = not rotate.Active
        rotateSeparate = rotateA:GetRootPart() == rotateA and
                         rotateB:GetRootPart() == rotateB and
                         #rotateA:GetConnectedParts(true) == 1

        local velocityA = makePart("VelocityA")
        local velocityB = makePart("VelocityB")
        local velocity = joint("VelocityMotor", "VelocityJ", velocityA,
                               velocityB)
        velocityInactive = not velocity.Active
        velocitySeparate = velocityA:GetRootPart() == velocityA and
                           velocityB:GetRootPart() == velocityB and
                           #velocityA:GetConnectedParts(true) == 1

        -- Same-pair duplicates: the newest edge is the active spanning-tree
        -- edge, exactly as for Weld duplicates.
        local dupA = makePart("GlueDupA")
        local dupB = makePart("GlueDupB")
        local dupFirst = joint("Glue", "GlueDupFirst", dupA, dupB)
        local dupSecond = joint("Glue", "GlueDupSecond", dupA, dupB)
        duplicateGlue = not dupFirst.Active and dupSecond.Active

        -- Mixed Weld/Glue edges compete in one creation-order forest.
        local mixA = makePart("GlueMixA")
        local mixB = makePart("GlueMixB")
        local mixWeld = joint("Weld", "GlueMixWeld", mixA, mixB)
        local mixGlue = joint("Glue", "GlueMixGlue", mixA, mixB)
        mixedPair = not mixWeld.Active and mixGlue.Active

        -- A three-part cycle with two newer edges: the oldest link closes the
        -- cycle and is inactive.
        local chainA = makePart("GlueChainA")
        local chainB = makePart("GlueChainB")
        local chainC = makePart("GlueChainC")
        local chainW1 = joint("Weld", "GlueChainW1", chainA, chainB)
        local chainGlue = joint("Glue", "GlueChainJ", chainB, chainC)
        local chainW3 = joint("Weld", "GlueChainW3", chainC, chainA)
        glueCycle = not chainW1.Active and chainGlue.Active and
                    chainW3.Active

        -- Native detach/reattach probe: changing Part1 splits the assembly,
        -- then the reattached edge becomes active again (and disabled splits
        -- it once more).
        local detachA = makePart("DetachA")
        local detachB = makePart("DetachB")
        local detachC = makePart("DetachC")
        local detachGlue = joint("Glue", "DetachJ", detachA, detachB)
        detachInitial = detachGlue.Active and
                        detachA:GetRootPart() == detachB
        detachGlue.Part1 = nil
        detachSplit = not detachGlue.Active and
                      detachA:GetRootPart() == detachA and
                      #detachA:GetConnectedParts(true) == 1
        detachGlue.Part1 = detachC
        detachReattached = detachGlue.Active and
                           detachA:GetRootPart() == detachC and
                           #detachA:GetConnectedParts(true) == 2
        detachGlue.Enabled = false
        detachDisabled = not detachGlue.Active and
                         detachA:GetRootPart() == detachA and
                         #detachA:GetConnectedParts(true) == 1
        detachGlue.Enabled = true
        detachReenabled = detachGlue.Active and
                          detachA:GetRootPart() == detachC and
                          #detachA:GetConnectedParts(true) == 2
    )");
    lua_State* L = env.L();
    CHECK(gbool(L, "glueActive"));
    CHECK(gbool(L, "glueRoot"));
    CHECK(gbool(L, "glueConnected"));
    CHECK(gbool(L, "manualGlueActive"));
    CHECK(gbool(L, "manualGlueRoot"));
    CHECK(gbool(L, "manualGlueConnected"));
    CHECK(gbool(L, "anchoredGlueInactive"));
    CHECK(gbool(L, "rotateInactive"));
    CHECK(gbool(L, "rotateSeparate"));
    CHECK(gbool(L, "velocityInactive"));
    CHECK(gbool(L, "velocitySeparate"));
    CHECK(gbool(L, "duplicateGlue"));
    CHECK(gbool(L, "mixedPair"));
    CHECK(gbool(L, "glueCycle"));
    CHECK(gbool(L, "detachInitial"));
    CHECK(gbool(L, "detachSplit"));
    CHECK(gbool(L, "detachReattached"));
    CHECK(gbool(L, "detachDisabled"));
    CHECK(gbool(L, "detachReenabled"));
}

TEST_CASE("SurfaceType.Hinge MakeJoints builds native Rotate edges") {
    Env env;
    env.exec(R"(
        local ws = workspace
        ws.Gravity = 0

        local function part(name, x, y, z)
            local p = Instance.new("Part")
            p.Name = name
            p.Size = Vector3.new(4, 1, 4)
            p.CFrame = CFrame.new(x, y, z)
            p.Anchored = false
            p.CanCollide = true
            p.Parent = ws
            return p
        end

        local function componentsEqual(cf, expected)
            local got = {cf:GetComponents()}
            for i = 1, 12 do
                if math.abs(got[i] - expected[i]) > 1e-9 then return false end
            end
            return true
        end

        local a = part("HingeSurfaceA", 0, 500, 600)
        local b = part("HingeSurfaceB", 0, 501, 600)
        a.TopSurface = Enum.SurfaceType.Hinge
        b.BottomSurface = Enum.SurfaceType.Smooth
        a:MakeJoints()
        local j = a:GetJoints()[1]
        hingeClass = j.ClassName
        hingeName = j.Name
        hingeParent = j.Parent == a
        hingeParts = j.Part0 == a and j.Part1 == b
        hingeEnabled = j.Enabled
        hingeInactive = not j.Active
        hingeSeparate = a:GetRootPart() == a and b:GetRootPart() == b and
                        #a:GetConnectedParts(true) == 1
        hingeC0 = componentsEqual(j.C0, {0, 0.5, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0})
        hingeC1 = componentsEqual(j.C1, {0, -0.5, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0})

        b:MakeJoints()
        hingeNoDuplicate = #a:GetJoints() == 1
        local same = j
        a:MakeJoints()
        hingeIdempotent = a:GetJoints()[1] == same
        a:BreakJoints()
        hingeBreakKeepsRotate = #a:GetJoints() == 1 and a:GetJoints()[1] == same
        j:Destroy()
        hingeDestroyed = #a:GetJoints() == 0
        b.CFrame = CFrame.new(10, 501, 600)
        a:MakeJoints()
        hingeDetached = #a:GetJoints() == 0
        b.CFrame = CFrame.new(0, 501, 600)
        a:MakeJoints()
        hingeReattached = #a:GetJoints() == 1 and a:GetJoints()[1] ~= same

        -- Bottom-face hinge: native processes the contact pair, so a
        -- Smooth caller creates the Rotate owned by the Hinge part, not by
        -- the caller.  Calling the Hinge part afterwards is idempotent.
        local c = part("HingeSurfaceC", 20, 500, 600)
        local d = part("HingeSurfaceD", 20, 501, 600)
        c.TopSurface = Enum.SurfaceType.Smooth
        d.BottomSurface = Enum.SurfaceType.Hinge
        c:MakeJoints()
        local jd = d:GetJoints()[1]
        hingeOtherBottomOwned = #c:GetJoints() == 1 and #d:GetJoints() == 1 and
            jd.Parent == d and jd.Part0 == d and jd.Part1 == c
        d:MakeJoints()
        hingeBottomParent = #d:GetJoints() == 1 and jd.Parent == d and
            jd.Part0 == d and jd.Part1 == c
        hingeBottomC0 = componentsEqual(jd.C0,
            {0, -0.5, 0, 1, 0, 0, 0, 0, -1, 0, 1, 0})
        hingeBottomC1 = componentsEqual(jd.C1,
            {0, 0.5, 0, 1, 0, 0, 0, 0, -1, 0, 1, 0})

        -- A non-Hinge top surface does not suppress the other part's Bottom
        -- Hinge (native Weld+Hinge and Glue+Hinge both produce Rotate).
        local cw = part("HingeWeldC", 30, 500, 600)
        local dw = part("HingeWeldD", 30, 501, 600)
        cw.TopSurface = Enum.SurfaceType.Weld
        dw.BottomSurface = Enum.SurfaceType.Hinge
        cw:MakeJoints()
        local jw = dw:GetJoints()[1]
        hingeWeldOtherBottom = jw and jw.ClassName == "Rotate" and
            jw.Parent == dw and jw.Part0 == dw and jw.Part1 == cw

        -- Top Motor/SteppingMotor win over the other part's Bottom Hinge;
        -- native uses RotateV/RotateP on the top or bottom face equally.
        local cm = part("HingeMotorC", 50, 500, 600)
        local dm = part("HingeMotorD", 50, 501, 600)
        cm.TopSurface = Enum.SurfaceType.Motor
        dm.BottomSurface = Enum.SurfaceType.Hinge
        cm:MakeJoints()
        local jm = cm:GetJoints()[1]
        hingeMotorOtherBottom = jm and jm.ClassName == "RotateV" and
            jm.Parent == cm and jm.Part0 == cm and jm.Part1 == dm and
            componentsEqual(jm.C0,
                {0, 0.5, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0}) and
            componentsEqual(jm.C1,
                {0, -0.5, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0})

        local cs = part("HingeStepC", 70, 500, 600)
        local ds = part("HingeStepD", 70, 501, 600)
        cs.TopSurface = Enum.SurfaceType.SteppingMotor
        ds.BottomSurface = Enum.SurfaceType.Hinge
        cs:MakeJoints()
        local js = cs:GetJoints()[1]
        hingeStepOtherBottom = js and js.ClassName == "RotateP" and
            js.Parent == cs and js.Part0 == cs and js.Part1 == ds

        -- Native anchors the frame at the caller's face centre, not the
        -- centre of the overlap rectangle.
        local e = part("HingeOffsetE", 40, 500, 600)
        local f = part("HingeOffsetF", 41, 501, 602)
        e.TopSurface = Enum.SurfaceType.Hinge
        f.BottomSurface = Enum.SurfaceType.Smooth
        e:MakeJoints()
        local je = e:GetJoints()[1]
        hingeOffsetC0 = componentsEqual(je.C0,
            {0, 0.5, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0})
        hingeOffsetC1 = componentsEqual(je.C1,
            {-1, -0.5, -2, -1, 0, 0, 0, 0, 1, 0, 1, 0})

        -- Native still creates the edge when the caller's TopSurface normal
        -- is not world-up (Z90, X90 and Z180 in the native rotation probe).
        local rz = part("HingeRotZ", 80, 500, 600)
        local rzb = part("HingeRotZB", 78.5, 500, 600)
        rz.Size = Vector3.new(2, 1, 2)
        rzb.Size = Vector3.new(2, 2, 2)
        rz.CFrame = CFrame.new(80, 500, 600) *
            CFrame.fromAxisAngle(Vector3.new(0, 0, 1), math.rad(90))
        rzb.CFrame = CFrame.new(78.5, 500, 600)
        rz.CanCollide = true
        rzb.CanCollide = true
        rz.TopSurface = Enum.SurfaceType.Hinge
        rz:MakeJoints()
        local jrz = rz:GetJoints()[1]
        hingeRotZ90 = jrz and jrz.ClassName == "Rotate" and
            componentsEqual(jrz.C1,
                {1, 0, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0})

        local rx = part("HingeRotX", 90, 500, 600)
        local rxb = part("HingeRotXB", 90, 500, 601.5)
        rx.Size = Vector3.new(2, 1, 2)
        rxb.Size = Vector3.new(2, 2, 2)
        rx.CFrame = CFrame.new(90, 500, 600) *
            CFrame.fromAxisAngle(Vector3.new(1, 0, 0), math.rad(90))
        rxb.CFrame = CFrame.new(90, 500, 601.5)
        rx.CanCollide = true
        rxb.CanCollide = true
        rx.TopSurface = Enum.SurfaceType.Hinge
        rx:MakeJoints()
        local jrx = rx:GetJoints()[1]
        hingeRotX90 = jrx and jrx.ClassName == "Rotate" and
            componentsEqual(jrx.C1,
                {0, 0, -1, -1, 0, 0, 0, 0, 1, 0, 1, 0})

        local r180 = part("HingeRot180", 100, 500, 600)
        local r180b = part("HingeRot180B", 100, 498.5, 600)
        r180.Size = Vector3.new(2, 1, 2)
        r180b.Size = Vector3.new(2, 2, 2)
        r180.CFrame = CFrame.new(100, 500, 600) *
            CFrame.fromAxisAngle(Vector3.new(0, 0, 1), math.rad(180))
        r180b.CFrame = CFrame.new(100, 498.5, 600)
        r180.CanCollide = true
        r180b.CanCollide = true
        r180.TopSurface = Enum.SurfaceType.Hinge
        r180:MakeJoints()
        local j180 = r180:GetJoints()[1]
        hingeRotZ180 = j180 and j180.ClassName == "Rotate" and
            componentsEqual(j180.C1,
                {0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 1, 0})

        -- Native does not joint two interpenetrating boxes (probe overlap
        -- case): the caller's top face is inside the other part.
        local oa = part("HingeOverlapA", 120, 500, 600)
        local ob = part("HingeOverlapB", 120, 501, 600)
        oa.Size = Vector3.new(2, 1, 2)
        ob.Size = Vector3.new(2, 2, 2)
        oa.CanCollide = true
        ob.CanCollide = true
        oa.TopSurface = Enum.SurfaceType.Hinge
        oa:MakeJoints()
        hingeOverlapEmpty = #oa:GetJoints() == 0

        -- A visible gap is not a contact either (native gap case).
        local ga = part("HingeGapA", 220, 500, 600)
        local gb = part("HingeGapB", 220, 501.55, 600)
        ga.Size = Vector3.new(2, 1, 2)
        gb.Size = Vector3.new(2, 2, 2)
        ga.CanCollide = true
        gb.CanCollide = true
        ga.TopSurface = Enum.SurfaceType.Hinge
        ga:MakeJoints()
        hingeGapEmpty = #ga:GetJoints() == 0

        -- Lower-case aliases are real native methods, not generated stubs.
        local la = part("HingeAliasA", 140, 500, 600)
        local lb = part("HingeAliasB", 140, 501, 600)
        la.Size = Vector3.new(2, 1, 2)
        lb.Size = Vector3.new(2, 1, 2)
        la.CanCollide = true
        lb.CanCollide = true
        la.TopSurface = Enum.SurfaceType.Hinge
        la:makeJoints()
        hingeAlias = #la:GetJoints() == 1 and
            la:GetJoints()[1].ClassName == "Rotate"

        -- Model:MakeJoints runs the BasePart surface pass for its subtree;
        -- Model:BreakJoints removes every legacy joint, Rotate included.
        local modelMake = Instance.new("Model")
        modelMake.Name = "HingeModelMake"
        modelMake.Parent = ws
        local ma = part("HingeModelMakeA", 160, 500, 600)
        local mb = part("HingeModelMakeB", 160, 501, 600)
        ma.Size = Vector3.new(2, 1, 2)
        mb.Size = Vector3.new(2, 1, 2)
        ma.Parent = modelMake
        mb.Parent = modelMake
        ma.CanCollide = true
        mb.CanCollide = true
        ma.TopSurface = Enum.SurfaceType.Hinge
        modelMake:MakeJoints()
        local modelJoint = ma:GetJoints()[1]
        hingeModelMake = modelJoint and modelJoint.ClassName == "Rotate" and
            modelJoint.Parent == ma and modelJoint.Part0 == ma and
            modelJoint.Part1 == mb

        local modelBreak = Instance.new("Model")
        modelBreak.Name = "HingeModelBreak"
        modelBreak.Parent = ws
        local ba = part("HingeModelBreakA", 180, 500, 600)
        local bb = part("HingeModelBreakB", 180, 501, 600)
        ba.Size = Vector3.new(2, 1, 2)
        bb.Size = Vector3.new(2, 1, 2)
        ba.Parent = modelBreak
        bb.Parent = modelBreak
        ba.CanCollide = true
        bb.CanCollide = true
        ba.TopSurface = Enum.SurfaceType.Hinge
        ba:MakeJoints()
        local weld = Instance.new("Weld")
        weld.Part0 = ba
        weld.Part1 = bb
        weld.Parent = ba
        local wc = Instance.new("WeldConstraint")
        wc.Part0 = ba
        wc.Part1 = bb
        wc.Parent = ba
        hingeModelBefore = #ba:GetJoints()
        modelBreak:BreakJoints()
        hingeModelBreak = hingeModelBefore == 3 and #ba:GetJoints() == 0

        -- Workspace entry points take a table of BaseParts and use the same
        -- BasePart passes; BreakJoints still leaves the Hinge Rotate.
        local wa = part("HingeWorkspaceA", 240, 500, 600)
        local wb = part("HingeWorkspaceB", 240, 501, 600)
        wa.Size = Vector3.new(2, 1, 2)
        wb.Size = Vector3.new(2, 1, 2)
        wa.CanCollide = true
        wb.CanCollide = true
        wa.TopSurface = Enum.SurfaceType.Hinge
        local wsNil, wsNilErr = pcall(function() return ws:MakeJoints(nil) end)
        local wsInst, wsInstErr = pcall(function() return ws:MakeJoints(wa) end)
        hingeWorkspaceErrors = not wsNil and not wsInst and
            tostring(wsNilErr):find("Expected table argument, got nil", 1, true) ~=
                nil and
            tostring(wsInstErr):find("Expected table argument, got userdata",
                1, true) ~= nil
        ws:MakeJoints({ wa })
        hingeWorkspaceMake = #wa:GetJoints() == 1 and
            wa:GetJoints()[1].ClassName == "Rotate"
        ws:BreakJoints({ wa })
        hingeWorkspaceBreak = #wa:GetJoints() == 1
        local wsBreakNil, wsBreakErr = pcall(function()
            return ws:BreakJoints(nil)
        end)
        hingeWorkspaceBreakError = not wsBreakNil and
            tostring(wsBreakErr):find("Expected table argument, got nil", 1,
                true) ~= nil

        -- Coplanar top faces that meet only along an edge have zero contact
        -- area and create no native surface joint.
        local edgeA = part("HingeEdgeA", 300, 500, 600)
        local edgeB = part("HingeEdgeB", 304, 500, 600)
        edgeA.TopSurface = Enum.SurfaceType.Hinge
        edgeB.TopSurface = Enum.SurfaceType.Smooth
        edgeA:MakeJoints()
        hingeEdgeEmpty = #edgeA:GetJoints() == 0

        -- Hinge on a side face participates in the native six-face pass too.
        local g = part("HingeSideG", 60, 500, 600)
        local h = part("HingeSideH", 64, 500, 600)
        g.RightSurface = Enum.SurfaceType.Hinge
        h.LeftSurface = Enum.SurfaceType.Smooth
        g:MakeJoints()
        local sideJoint = g:GetJoints()[1]
        hingeSideJoint = sideJoint and sideJoint.ClassName == "Rotate" and
            sideJoint.Parent == g and sideJoint.Part0 == g and
            sideJoint.Part1 == h

        local ok, err = pcall(function() return Instance.new("Hinge") end)
        hingeClassRefused = not ok and string.find(tostring(err),
            'Unable to create an Instance of type "Hinge"', 1, true) ~= nil
    )");
    lua_State* L = env.L();
    CHECK(gstr(L, "hingeClass") == "Rotate");
    CHECK(gstr(L, "hingeName") == "Rotate");
    CHECK(gbool(L, "hingeParent"));
    CHECK(gbool(L, "hingeParts"));
    CHECK(gbool(L, "hingeEnabled"));
    CHECK(gbool(L, "hingeInactive"));
    CHECK(gbool(L, "hingeSeparate"));
    CHECK(gbool(L, "hingeC0"));
    CHECK(gbool(L, "hingeC1"));
    CHECK(gbool(L, "hingeNoDuplicate"));
    CHECK(gbool(L, "hingeIdempotent"));
    CHECK(gbool(L, "hingeBreakKeepsRotate"));
    CHECK(gbool(L, "hingeDestroyed"));
    CHECK(gbool(L, "hingeDetached"));
    CHECK(gbool(L, "hingeReattached"));
    CHECK(gbool(L, "hingeOtherBottomOwned"));
    CHECK(gbool(L, "hingeWeldOtherBottom"));
    CHECK(gbool(L, "hingeMotorOtherBottom"));
    CHECK(gbool(L, "hingeStepOtherBottom"));
    CHECK(gbool(L, "hingeBottomParent"));
    CHECK(gbool(L, "hingeBottomC0"));
    CHECK(gbool(L, "hingeBottomC1"));
    CHECK(gbool(L, "hingeOffsetC0"));
    CHECK(gbool(L, "hingeOffsetC1"));
    CHECK(gbool(L, "hingeRotZ90"));
    CHECK(gbool(L, "hingeRotX90"));
    CHECK(gbool(L, "hingeRotZ180"));
    CHECK(gbool(L, "hingeOverlapEmpty"));
    CHECK(gbool(L, "hingeGapEmpty"));
    CHECK(gbool(L, "hingeEdgeEmpty"));
    CHECK(gbool(L, "hingeAlias"));
    CHECK(gbool(L, "hingeModelMake"));
    CHECK(gbool(L, "hingeModelBreak"));
    CHECK(gbool(L, "hingeWorkspaceErrors"));
    CHECK(gbool(L, "hingeWorkspaceMake"));
    CHECK(gbool(L, "hingeWorkspaceBreak"));
    CHECK(gbool(L, "hingeWorkspaceBreakError"));
    CHECK(gbool(L, "hingeSideJoint"));
    CHECK(gbool(L, "hingeClassRefused"));
}

TEST_CASE("SurfaceType.MakeJoints and BreakJoints cover every joint family") {
    Env env;
    env.exec(R"LUA(
        local ws = workspace
        ws.Gravity = 0
        local function part(name, x, y, z)
            local p = Instance.new('Part')
            p.Name = name
            p.Size = Vector3.new(2, 1, 2)
            p.CFrame = CFrame.new(x, y, z)
            p.Anchored = false
            p.CanCollide = true
            p.Parent = ws
            return p
        end
        local surfaceName = {
            [0] = 'Smooth', [1] = 'Glue', [2] = 'Weld', [3] = 'Studs',
            [4] = 'Inlet', [5] = 'Universal', [6] = 'Hinge', [7] = 'Motor',
            [8] = 'SteppingMotor', [10] = 'SmoothNoOutlines',
        }
        local function pair(top, bottom, x, z)
            local a = part('A' .. x .. top, x, 500, z)
            local b = part('B' .. x .. bottom, x, 501, z)
            a.TopSurface = Enum.SurfaceType[surfaceName[top]]
            b.BottomSurface = Enum.SurfaceType[surfaceName[bottom]]
            return a, b
        end
        local function joint(a, b)
            for _, j in ipairs(a:GetJoints()) do
                if (j.Part0 == a and j.Part1 == b) or
                   (j.Part0 == b and j.Part1 == a) then
                    return j
                end
            end
        end
        local cases = {
            { 2, 0, 'Weld', true }, { 1, 0, 'Glue', true },
            { 3, 4, 'Snap', true }, { 6, 0, 'Rotate', false },
            { 7, 0, 'RotateV', false }, { 8, 0, 'RotateP', false },
        }
        for i, c in ipairs(cases) do
            local a, b = pair(c[1], c[2], i * 8, 600)
            a:MakeJoints()
            local j = joint(a, b)
            if not j or j.ClassName ~= c[3] or j.Name ~= c[3] or
               j.Parent ~= a or j.Part0 ~= a or j.Part1 ~= b then
                jointFamilyWrong = true
            end
            a:BreakJoints()
            local after = joint(a, b)
            if c[4] then
                if after then breakFamilyWrong = true end
            elseif not after then
                breakFamilyWrong = true
            end
            a:Destroy()
            b:Destroy()
        end
        -- BreakJoints from the opposite endpoint removes the same rigid edge.
        local oa, ob = pair(2, 0, 80, 600)
        ob:MakeJoints()
        if not joint(oa, ob) then breakOtherWrong = true end
        oa:BreakJoints()
        if joint(oa, ob) then breakOtherWrong = true end
        oa:Destroy()
        ob:Destroy()

        -- Side faces participate in the same six-face pass.
        local sa = part('SideA', 0, 500, 650)
        local sb = part('SideB', 2, 500, 650)
        sa.RightSurface = Enum.SurfaceType.Weld
        sb.LeftSurface = Enum.SurfaceType.Smooth
        sa:MakeJoints()
        local sj = joint(sa, sb)
        sideFaceWrong = not sj or sj.ClassName ~= 'Weld' or sj.Parent ~= sa
        sa:Destroy()
        sb:Destroy()

        -- Model:MakeJoints runs the BasePart pass; Model:BreakJoints removes
        -- every joint in the subtree, including Rotate.
        local model = Instance.new('Model')
        model.Parent = ws
        local ma, mb = pair(2, 0, 120, 620)
        ma.Parent = model
        mb.Parent = model
        model:MakeJoints()
        local mj = joint(ma, mb)
        if not mj or mj.ClassName ~= 'Weld' or mj.Parent ~= ma then
            modelMakeWrong = true
        end
        local hm = Instance.new('Model')
        hm.Parent = ws
        local ha, hb = pair(6, 0, 140, 620)
        ha.Parent = hm
        hb.Parent = hm
        hm:MakeJoints()
        if not joint(ha, hb) then modelHingeWrong = true end
        model:BreakJoints()
        hm:BreakJoints()
        if joint(ma, mb) or joint(ha, hb) then modelBreakWrong = true end

        -- Workspace table entry points use the same BasePart passes.
        local wa, wb = pair(2, 0, 160, 600)
        ws:MakeJoints({ wa })
        if not joint(wa, wb) then workspaceMakeWrong = true end
        ws:BreakJoints({ wa })
        if joint(wa, wb) then workspaceBreakWrong = true end
        local xa, xb = pair(6, 0, 180, 600)
        ws:MakeJoints({ xa })
        ws:BreakJoints({ xa })
        if not joint(xa, xb) then workspaceDynamicWrong = true end

        -- Lower-case aliases exist on BasePart, Model and Workspace.
        local aliasOk = pcall(function() wa:makeJoints() end) and
            pcall(function() wa:breakJoints() end) and
            pcall(function() model:makeJoints() end) and
            pcall(function() model:breakJoints() end) and
            pcall(function() ws:makeJoints({}) end) and
            pcall(function() ws:breakJoints({}) end)
        aliasesWrong = not aliasOk
    )LUA");
    lua_State* L = env.L();
    CHECK(!gbool(L, "jointFamilyWrong"));
    CHECK(!gbool(L, "breakFamilyWrong"));
    CHECK(!gbool(L, "breakOtherWrong"));
    CHECK(!gbool(L, "sideFaceWrong"));
    CHECK(!gbool(L, "modelMakeWrong"));
    CHECK(!gbool(L, "modelHingeWrong"));
    CHECK(!gbool(L, "modelBreakWrong"));
    CHECK(!gbool(L, "workspaceMakeWrong"));
    CHECK(!gbool(L, "workspaceBreakWrong"));
    CHECK(!gbool(L, "workspaceDynamicWrong"));
    CHECK(!gbool(L, "aliasesWrong"));
}

TEST_CASE("legacy Hinge records are dropped like native InsertService") {
    rbx::register_builtin_classes();
    CHECK(rbx::find_class("Hinge") == nullptr);

    rbxclient::JoinSchema schema;
    rbxclient::SchemaClass model;
    model.name = "Model";
    rbxclient::SchemaClass hinge;
    hinge.name = "Hinge";
    rbxclient::SchemaClass part;
    part.name = "Part";
    schema.classes = {model, hinge, part};

    rbxclient::JoinRecord model_rec;
    model_rec.lo = 1;
    model_rec.id = 1;
    model_rec.cls = 0;
    model_rec.plo = 0;
    model_rec.parent = -1;

    rbxclient::JoinRecord hinge_rec;
    hinge_rec.lo = 1;
    hinge_rec.id = 2;
    hinge_rec.cls = 1;
    hinge_rec.plo = 1;
    hinge_rec.parent = 1;

    rbxclient::JoinRecord part_rec;
    part_rec.lo = 1;
    part_rec.id = 3;
    part_rec.cls = 2;
    part_rec.plo = 1;
    part_rec.parent = 1;

    std::string err;
    rbxclient::ApplyReport rep = rbxclient::applyJoinSnapshot(
        schema, {model_rec, hinge_rec, part_rec}, &err);
    REQUIRE(rep.root);
    CHECK(err.empty());
    CHECK(rep.stats.skipped_records == 1);
    CHECK(rep.stats.skip_names.count("Hinge") == 1);
    CHECK(rep.by_id.at(2) == -1);

    rbx::Instance* loaded = rep.root->find_first_child_of_class("Model");
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->children().size() == 1);
    CHECK(loaded->children()[0]->class_name() == "Part");
}

TEST_CASE("Assembly tie uses creation order across replicated and local parts") {
    Env env;
    env.exec(R"(
        local ws = workspace
        local function makePart(name)
            local p = Instance.new("Part")
            p.Name = name
            p.Size = Vector3.new(2, 2, 2)
            p.Anchored = false
            p.Massless = false
            p.CanCollide = false
            p.Parent = ws
            return p
        end
        local function weld(a, b)
            local j = Instance.new("Weld")
            j.Part0 = a
            j.Part1 = b
            j.Parent = ws
            return j
        end

        -- R1/R2 stand in for replicated instances materialized before the
        -- script ran; L1/L2 are created later and therefore have higher
        -- native creation order.
        mixedR1 = makePart("MixedNetR1")
        mixedR2 = makePart("MixedNetR2")
        mixedL1 = makePart("MixedNetL1")
        mixedL2 = makePart("MixedNetL2")
        weld(mixedR1, mixedL1)
        weld(mixedL1, mixedL2)
        weld(mixedL2, mixedR2)
    )");

    lua_State* L = env.L();
    auto get = [&](const char* name) -> rbx::Instance* {
        lua_getglobal(L, name);
        rbx::Instance* inst = rbx::Instance::check(L, -1);
        lua_pop(L, 1);
        return inst;
    };
    rbx::Instance* r1 = get("mixedR1");
    rbx::Instance* r2 = get("mixedR2");
    REQUIRE(r1 != nullptr);
    REQUIRE(r2 != nullptr);
    // Deliberately huge wire ids must not outrank the later local creation
    // order; native mixed-tie probe selected the later local part.
    r1->set_network_identity(1, 0x40000000u);
    r2->set_network_identity(1, 0x40000001u);

    env.exec(R"(
        mixedRootOk = mixedL1:GetRootPart() == mixedL2 and
                      mixedR1:GetRootPart() == mixedL2 and
                      mixedR2:GetRootPart() == mixedL2
    )");
    CHECK(gbool(L, "mixedRootOk"));
}

TEST_CASE("Player mouse, Humanoid state and headless input defaults") {
    Env env;
    lua_State* L = env.L();
    rbx::Ref<rbx::Instance> bob = rbx::instantiate(rbx::find_class("Player"));
    REQUIRE(bob);
    rbx::Instance::push(L, bob.get());
    lua_setglobal(L, "bob");
    env.exec(R"(
        players = game:GetService("Players")
        bob.Parent = players
        player = players.LocalPlayer
        m1 = player:GetMouse()
        m2 = player:GetMouse()
        mouseStable = m1 == m2
        mouseClass = m1.ClassName
        mouseSize = m1.ViewSizeX
        mouseX = m1.X
        mouseY = m1.Y

        hum = Instance.new("Humanoid")
        hum:ChangeState(Enum.HumanoidStateType.Jumping)
        state = hum:GetState()
        stateIsJumping = state == Enum.HumanoidStateType.Jumping
        hum:Move(Vector3.new(1, 0, 0))
        hum.WalkSpeed = 16
        moveVelocityX = hum:GetMoveVelocity().X
        hum:SetStateEnabled(Enum.HumanoidStateType.Climbing, false)
        climbEnabled = hum:GetStateEnabled(Enum.HumanoidStateType.Climbing)
        accessories = #hum:GetAccessories()

        uis = game:GetService("UserInputService")
        noKey = not uis:IsKeyDown(Enum.KeyCode.W)
        noMouse = not uis:IsMouseButtonPressed(Enum.UserInputType.MouseButton1)
        emptyKeys = #uis:GetKeysPressed() == 0
        mouseAtZero = uis:GetMouseLocation() == Vector2.new(0, 0)
        desktop = uis:GetDeviceType() == Enum.DeviceType.Desktop
        linux = uis:GetPlatform() == Enum.Platform.Linux

        run = game:GetService("RunService")
        isClient = run:IsClient() and not run:IsServer() and run:IsRunning()
        pgs = workspace:PGSIsEnabled()
    )");
    CHECK(gbool(L, "mouseStable"));
    CHECK(gstr(L, "mouseClass") == "Mouse");
    CHECK(gnum(L, "mouseSize") == 0);
    CHECK(gnum(L, "mouseX") == 0);
    CHECK(gnum(L, "mouseY") == 0);
    CHECK(gbool(L, "stateIsJumping"));
    CHECK(gnum(L, "moveVelocityX") == 16);
    CHECK(gbool(L, "climbEnabled") == false);
    CHECK(gnum(L, "accessories") == 0);
    CHECK(gbool(L, "noKey"));
    CHECK(gbool(L, "noMouse"));
    CHECK(gbool(L, "emptyKeys"));
    CHECK(gbool(L, "mouseAtZero"));
    CHECK(gbool(L, "desktop"));
    CHECK(gbool(L, "linux"));
    CHECK(gbool(L, "isClient"));
    CHECK(gbool(L, "pgs"));
}

TEST_CASE("host input drives Mouse and UserInputService") {
    Env env;
    lua_State* L = env.L();
    rbx::input::reset();
    env.exec(R"(
        players = game:GetService("Players")
        bob = Instance.new("Player")
        bob.Parent = players
        player = players.LocalPlayer
        mouse = player:GetMouse()
        uis = game:GetService("UserInputService")
        moves = 0
        downs = 0
        ups = 0
        began = 0
        ended = 0
        changed = 0
        lastBegan = Enum.UserInputType.None
        mouse.Move:Connect(function() moves += 1 end)
        mouse.Button1Down:Connect(function() downs += 1 end)
        mouse.Button1Up:Connect(function() ups += 1 end)
        uis.InputBegan:Connect(function(input)
            began += 1
            lastBegan = input.UserInputType
        end)
        uis.InputChanged:Connect(function() changed += 1 end)
        uis.InputEnded:Connect(function() ended += 1 end)
    )");

    rbx::input::publish_mouse(100, 200, false, false, false, 800, 600);
    env.step(1.0 / 60.0);
    env.exec(R"(
        movedX = mouse.X
        movedY = mouse.Y
        movedW = mouse.ViewSizeX
        movedH = mouse.ViewSizeY
        moveCount = moves
        inputChanged = changed
    )");
    CHECK(gnum(L, "movedX") == 100);
    CHECK(gnum(L, "movedY") == 200);
    CHECK(gnum(L, "movedW") == 800);
    CHECK(gnum(L, "movedH") == 600);
    CHECK(gnum(L, "moveCount") == 1);
    CHECK(gnum(L, "inputChanged") == 1);

    rbx::input::publish_mouse(150, 250, true, false, false, 800, 600);
    rbx::input::publish_key(119, true); // Enum.KeyCode.W
    env.step(1.0 / 60.0);
    env.exec(R"(
        downCount = downs
        beganCount = began
        mouseDown = uis:IsMouseButtonPressed(Enum.UserInputType.MouseButton1)
        keyDown = uis:IsKeyDown(Enum.KeyCode.W)
        keyCount = #uis:GetKeysPressed()
        pressedType = lastBegan
    )");
    CHECK(gnum(L, "downCount") == 1);
    CHECK(gnum(L, "beganCount") == 2); // mouse button + key
    CHECK(gbool(L, "mouseDown"));
    CHECK(gbool(L, "keyDown"));
    CHECK(gnum(L, "keyCount") == 1);
    env.exec("pressedMouse = pressedType == Enum.UserInputType.Keyboard");
    CHECK(gbool(L, "pressedMouse"));

    rbx::input::publish_mouse(150, 250, false, false, false, 800, 600);
    rbx::input::publish_key(119, false);
    env.step(1.0 / 60.0);
    env.exec(R"(
        upCount = ups
        endedCount = ended
        mouseUp = not uis:IsMouseButtonPressed(Enum.UserInputType.MouseButton1)
        keyUp = not uis:IsKeyDown(Enum.KeyCode.W)
        keysGone = #uis:GetKeysPressed() == 0
    )");
    CHECK(gnum(L, "upCount") == 1);
    CHECK(gnum(L, "endedCount") == 2);
    CHECK(gbool(L, "mouseUp"));
    CHECK(gbool(L, "keyUp"));
    CHECK(gbool(L, "keysGone"));
    rbx::input::reset();
}

TEST_CASE("legacy GUI tweens animate over frames and respect override") {
    Env env;
    lua_State* L = env.L();
    env.exec(R"(
        frame = Instance.new("Frame")
        tweened = frame:TweenPosition(
            UDim2.new(0.25, 0, 0.5, 0), Enum.EasingDirection.Out,
            Enum.EasingStyle.Linear, 0.5)
        tweenStartX = frame.Position.X.Scale
        conflict = frame:TweenPosition(UDim2.new(0.5, 0, 0.5, 0),
            Enum.EasingDirection.Out, Enum.EasingStyle.Linear, 0.5)

        frame2 = Instance.new("Frame")
        tweenedSize = frame2:TweenSizeAndPosition(
            UDim2.new(0, 100, 0, 50), UDim2.new(0, 10, 0, 20),
            Enum.EasingDirection.Out, Enum.EasingStyle.Linear, 0.5)
        sizeStart = frame2.Size.X.Offset

        frame3 = Instance.new("Frame")
        first = frame3:TweenPosition(UDim2.new(0.1, 0, 0, 0),
            Enum.EasingDirection.Out, Enum.EasingStyle.Linear, 1)
        overrideOk = frame3:TweenPosition(UDim2.new(0.4, 0, 0, 0),
            Enum.EasingDirection.Out, Enum.EasingStyle.Linear, 0.5, true)
    )");
    CHECK(gbool(L, "tweened"));
    CHECK(gnum(L, "tweenStartX") == 0);
    CHECK_FALSE(gbool(L, "conflict"));
    CHECK(gbool(L, "tweenedSize"));
    CHECK(gnum(L, "sizeStart") == 0);
    CHECK(gbool(L, "overrideOk"));

    env.step(0.25);
    env.exec(R"(
        midX = frame.Position.X.Scale
        midWidth = frame2.Size.X.Offset
    )");
    CHECK(gnum(L, "midX") == doctest::Approx(0.125));
    CHECK(gnum(L, "midWidth") == doctest::Approx(50));

    env.step(0.30);
    env.exec(R"(
        finalX = frame.Position.X.Scale
        finalWidth = frame2.Size.X.Offset
        overriddenX = frame3.Position.X.Scale
    )");
    CHECK(gnum(L, "finalX") == doctest::Approx(0.25));
    CHECK(gnum(L, "finalWidth") == 100);
    CHECK(gnum(L, "overriddenX") == doctest::Approx(0.4));
}

TEST_CASE("Sound playback follows the host audio backend") {
    FakeAudio fake;
    rbx::audio::set_backend(&fake);
    {
        Env env;
        lua_State* L = env.L();
        env.exec(R"(
            snd = Instance.new("Sound")
            snd.SoundId = "rbxasset://sounds/electronicpingshort.wav"
            snd.Parent = workspace
            snd:Play()
            acceptedPlaying = snd.IsPlaying
            acceptedState = snd:GetPlaybackState() == Enum.PlaybackState.Playing
            snd:Pause()
            acceptedPaused = snd.IsPaused
            snd:Resume()
            acceptedResumed = snd.IsPlaying and not snd.IsPaused
            snd:Stop()
            acceptedStopped = not snd.IsPlaying and not snd.IsPaused
        )");
        CHECK(gbool(L, "acceptedPlaying"));
        CHECK(gbool(L, "acceptedState"));
        CHECK(gbool(L, "acceptedPaused"));
        CHECK(gbool(L, "acceptedResumed"));
        CHECK(gbool(L, "acceptedStopped"));
        CHECK(fake.plays == 1);
        CHECK(fake.uri == "rbxasset://sounds/electronicpingshort.wav");

        fake.accept = false;
        env.exec(R"(
            snd:Play()
            rejectedPlaying = snd.IsPlaying
            rejectedState = snd:GetPlaybackState() == Enum.PlaybackState.Playing
        )");
        CHECK_FALSE(gbool(L, "rejectedPlaying"));
        CHECK_FALSE(gbool(L, "rejectedState"));
        CHECK(fake.plays == 2);
    }
    rbx::audio::set_backend(nullptr);
}

TEST_CASE("Sound listener configuration and world positions") {
    Env env;
    lua_State* L = env.L();
    env.exec(R"(
        local defaultSound = Instance.new("Sound")
        defaultVolume = defaultSound.Volume
        defaultSpeed = defaultSound.PlaybackSpeed
        defaultMin = defaultSound.RollOffMinDistance
        defaultEmitter = defaultSound.EmitterSize
        defaultMax = defaultSound.RollOffMaxDistance
        defaultLooped = defaultSound.Looped
        defaultMode = defaultSound.RollOffMode.Value
        local defaultGroup = Instance.new("SoundGroup")
        defaultGroupVolume = defaultGroup.Volume

        local service = game:GetService("SoundService")
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.new(1, 2, 3)
        workspace.CurrentCamera = camera

        local part = Instance.new("Part")
        part.CFrame = CFrame.new(10, 20, 30)
        part.Parent = workspace
        local sound = Instance.new("Sound")
        sound.Parent = part

        cameraCFX = workspace.CurrentCamera.CFrame.Position.X
        listenerType = service.ListenerType.Value
        soundWorldX = sound:GetWorldCFrame().Position.X

        service.ListenerType = Enum.ListenerType.CFrame
        service.ListenerCFrame = CFrame.new(100, 200, 300)
        cframeListenerX = service:GetListenerWorldCFrame().Position.X

        service.ListenerType = Enum.ListenerType.ObjectCFrame
        service.ListenerObject = camera
        objectListenerX = service:GetListenerWorldCFrame().Position.X
    )");
    CHECK(gnum(L, "cameraCFX") == doctest::Approx(1.0));
    CHECK(gnum(L, "listenerType") == 0);
    CHECK(gnum(L, "defaultVolume") == doctest::Approx(0.5));
    CHECK(gnum(L, "defaultSpeed") == doctest::Approx(1.0));
    CHECK(gnum(L, "defaultMin") == doctest::Approx(10.0));
    CHECK(gnum(L, "defaultEmitter") == doctest::Approx(10.0));
    CHECK(gnum(L, "defaultMax") == doctest::Approx(10000.0));
    CHECK_FALSE(gbool(L, "defaultLooped"));
    CHECK(gnum(L, "defaultMode") == 0);
    CHECK(gnum(L, "defaultGroupVolume") == doctest::Approx(1.0));
    CHECK(gnum(L, "soundWorldX") == doctest::Approx(10.0));
    CHECK(gnum(L, "cframeListenerX") == doctest::Approx(100.0));
    CHECK(gnum(L, "objectListenerX") == doctest::Approx(1.0));
}

TEST_CASE("strict API mode rejects compatibility fallbacks") {
    setenv("RBX_STRICT_API", "1", 1);
    Env env;
    lua_State* L = env.L();
    env.exec(R"(
        p = Instance.new("Player")
        strictOk = pcall(function() return p:IsInGroup(1) end) == false
        local ok, err = pcall(function() return p:IsInGroup(1) end)
        strictMessage = tostring(err)
        stubStrict = pcall(function() return p:GetUnder13() end) == false
        snd = Instance.new("Sound")
        soundStrict = pcall(function() snd:Play() end) == false
    )");
    CHECK(gbool(L, "strictOk"));
    CHECK(gbool(L, "stubStrict"));
    CHECK(gbool(L, "soundStrict"));
    CHECK(gstr(L, "strictMessage").find("not implemented") !=
          std::string::npos);
    unsetenv("RBX_STRICT_API");
}

TEST_CASE("Workspace starts with the native gravity and authority defaults") {
    Env env;
    lua_State* L = env.L();
    env.exec(R"(
        wsGravity = workspace.Gravity
        wsAuthority = tostring(workspace.AuthorityMode)
    )");
    CHECK(gnum(L, "wsGravity") == doctest::Approx(196.2));
    CHECK(gstr(L, "wsAuthority") == "Enum.AuthorityMode.Automatic");
}

TEST_CASE("contact backend drives queries and dispatches touch events") {
    struct FakeContact final : rbx::contact::Backend {
        rbx::Instance* a = nullptr;
        rbx::Instance* b = nullptr;
        std::vector<rbx::contact::Pair> begun;
        std::vector<rbx::contact::Pair> ended;

        void touching_parts(rbx::Instance* part,
                            std::vector<rbx::Instance*>* out) override {
            if (out && part == a)
                out->push_back(b);
        }
        bool grounded(rbx::Instance* part) override { return part == a; }
        std::vector<rbx::contact::Pair> take_begun() override {
            std::vector<rbx::contact::Pair> out = std::move(begun);
            begun.clear();
            return out;
        }
        std::vector<rbx::contact::Pair> take_ended() override {
            std::vector<rbx::contact::Pair> out = std::move(ended);
            ended.clear();
            return out;
        }
    } fake;

    Env env;
    lua_State* L = env.L();
    env.exec(R"(
        a = Instance.new("Part")
        a.Name = "ContactA"
        b = Instance.new("Part")
        b.Name = "ContactB"
        a.Parent = workspace
        b.Parent = workspace
        touchedA, touchedB, endedA, endedB = 0, 0, 0, 0
        touchName = ""
        a.Touched:Connect(function(hit)
            touchedA = touchedA + 1
            touchName = hit.Name
        end)
        b.Touched:Connect(function(hit) touchedB = touchedB + 1 end)
        a.TouchEnded:Connect(function(hit) endedA = endedA + 1 end)
        b.TouchEnded:Connect(function(hit) endedB = endedB + 1 end)
    )");

    auto global_inst = [&](const char* name) {
        lua_getglobal(L, name);
        rbx::Instance* inst = rbx::Instance::check(L, -1);
        lua_pop(L, 1);
        return inst;
    };
    fake.a = global_inst("a");
    fake.b = global_inst("b");
    REQUIRE(fake.a != nullptr);
    REQUIRE(fake.b != nullptr);

    rbx::contact::set_backend(&fake);
    rbx::contact::Pair pair{fake.a, fake.b};
    fake.begun.push_back(pair);
    rbx::contact::dispatch(L);
    CHECK(gnum(L, "touchedA") == 1);
    CHECK(gnum(L, "touchedB") == 1);
    CHECK(gstr(L, "touchName") == "ContactB");

    env.exec(R"(
        touchCount = #a:GetTouchingParts()
        grounded = a:IsGrounded()
    )");
    CHECK(gnum(L, "touchCount") == 1);
    CHECK(gbool(L, "grounded"));

    fake.ended.push_back(pair);
    rbx::contact::dispatch(L);
    CHECK(gnum(L, "endedA") == 1);
    CHECK(gnum(L, "endedB") == 1);
    rbx::contact::set_backend(nullptr);
}
