// unit_runtime.cpp — doctest suite for the rbx_runtime scripting environment.
//
// Exercises rbxch::run end-to-end (compile Luau source -> sandboxed execution
// -> u32 answer), pinning the individual sandbox shims granularly. The 5
// captured native datasets (solver-regress suites) remain the byte-exact
// oracle for the full challenge programs; this suite catches which SHIM broke
// when they drift.
// doctest provides main via DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN (note: not
// Catch2's "..._IMPLEMENTATION_MAIN" spelling).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "runner.h"

#include "luacode.h" // luau_compile + lua_CompileOptions (Luau.Compiler)

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

lua_CompileOptions default_compile_options() {
    lua_CompileOptions o;
    std::memset(&o, 0, sizeof(o)); // same as Luau's REPL
    return o;
}

std::vector<char> compile(const std::string& src) {
    lua_CompileOptions co = default_compile_options();
    size_t outSize = 0;
    char* out = luau_compile(src.c_str(), src.size(), &co, &outSize);
    REQUIRE(out != nullptr);
    std::vector<char> bc(out, out + outSize);
    std::free(out); // luau_compile allocates with malloc
    return bc;
}

uint32_t run_src(const std::string& src, const rbxch::Options& opts,
                 rbxch::Status* status_out = nullptr) {
    std::vector<char> bc = compile(src);
    rbxch::Result r = rbxch::run(bc.data(), bc.size(), opts);
    if (status_out)
        *status_out = r.status;
    return r.answer;
}

} // namespace

TEST_CASE("basic execution") {
    CHECK(run_src("return 40 + 2", {}) == 42);
}

TEST_CASE("arguments are pushed as (u1, u2)") {
    rbxch::Options o;
    o.u1 = 7;
    o.u2 = 3;
    CHECK(run_src("local a, b = ...; return a * 100 + b", o) == 703);
}

TEST_CASE("load failure on garbage bytecode") {
    rbxch::Status st = rbxch::Status::Ok;
    const char garbage[] = "\x00\x01\x02\x03not bytecode";
    rbxch::Result r = rbxch::run(garbage, sizeof(garbage) - 1, {});
    CHECK(r.status == rbxch::Status::LoadFailed);
}

TEST_CASE("JobId") {
    rbxch::Options o;
    o.job = "job-1234";
    CHECK(run_src("return #game.JobId", o) == 8);
}

TEST_CASE("RunService:IsStudio follows the profile") {
    rbxch::Options o;
    rbxch::Status st = rbxch::Status::Ok;

    o.studio = "false";
    CHECK(run_src("return game:GetService(\"RunService\"):IsStudio() and 1 or 0", o, &st) == 0);
    CHECK(st == rbxch::Status::Ok);

    o.studio = "true";
    CHECK(run_src("return game:GetService(\"RunService\"):IsStudio() and 1 or 0", o, &st) == 1);

    o.studio = "error"; // native behavior: calling it raises
    run_src("return game:GetService(\"RunService\"):IsStudio() and 1 or 0", o, &st);
    CHECK(st == rbxch::Status::RuntimeError);
}

TEST_CASE("GetService identity contract") {
    // same service table on every call; unknown service = fresh table per call
    rbxch::Options o;
    CHECK(run_src(
              "local rs = game:GetService(\"RunService\")\n"
              "return rs == game:GetService(\"RunService\") and 1 or 0",
              o) == 1);
    CHECK(run_src(
              "return game:GetService(\"NoSuchService\") == "
              "game:GetService(\"NoSuchService\") and 1 or 0",
              o) == 0);
}

TEST_CASE("UserSettings profile") {
    rbxch::Options o;
    rbxch::Status st = rbxch::Status::Ok;

    // default profile: UserSettings is nil (native: calling nil raises)
    CHECK(run_src("return UserSettings == nil and 1 or 0", o) == 1);

    // ok profile: callable, tostring() yields the calibration string
    o.usersettings = "ok";
    o.us_string = std::string(20, 'a'); // 20 chars, like "a"*19 + "Q"
    CHECK(run_src("return #tostring(UserSettings())", o) == 20);
}

TEST_CASE("os.exit profile") {
    rbxch::Options o;
    rbxch::Status st = rbxch::Status::Ok;

    o.os_exit = "missing"; // native: os.exit does not exist / raises
    run_src("os.exit()", o, &st);
    CHECK(st == rbxch::Status::RuntimeError);

    o.os_exit = "ok"; // installed but a no-op — must NOT terminate the process
    run_src("os.exit()", o, &st);
    CHECK(st == rbxch::Status::Ok);
}

TEST_CASE("newproxy profile") {
    rbxch::Options o;
    rbxch::Status st = rbxch::Status::Ok;

    o.newproxy = "ok";
    CHECK(run_src("local p = newproxy(true) return 1", o, &st) == 1);
    CHECK(st == rbxch::Status::Ok);

    o.newproxy = "error";
    run_src("local p = newproxy(true) return 1", o, &st);
    CHECK(st == rbxch::Status::RuntimeError);
}

TEST_CASE("Roblox Random (PCG) known-answer") {
    // Both ports (C++ rbx_runtime/src/api/random.cpp and the independent
    // RE-derived py/roblox_random.py, from libroblox sub_27B30AE/sub_2307554)
    // agree on this value; the captured datasets also lock the behavior
    // end-to-end against the native client.
    rbxch::Options o;
    CHECK(run_src("local r = Random.new(12345) return r:NextInteger(0, 1000000)", o) == 903147u);
}
