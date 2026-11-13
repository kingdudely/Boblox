// unit_features.cpp — the local feature-set resolver used by the client.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "common/avatar_watch.h"
#include "common/features.h"
#include "common/session_config.h"

TEST_CASE("features: presets select the local cost profile") {
    rbx::features::Set s;
    std::string err;
    REQUIRE(rbx::features::preset(&s, "full", &err));
    CHECK(s.luau);
    CHECK(s.render);
    CHECK(s.gui);
    CHECK(s.audio);
    CHECK(s.physics == rbx::features::PhysicsMode::Client);

    REQUIRE(rbx::features::preset(&s, "minimal", &err));
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);
    CHECK_FALSE(s.audio);

    REQUIRE(rbx::features::preset(&s, "remote", &err));
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);
    CHECK(s.world == rbx::features::WorldMode::Full);

    // Join-only is the low-memory preset: no world is built or parsed.
    REQUIRE(rbx::features::preset(&s, "join", &err));
    CHECK(s.world == rbx::features::WorldMode::None);
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);
    CHECK_FALSE(s.audio);

    CHECK_FALSE(rbx::features::preset(&s, "turbo", &err));
    CHECK(err.find("full, minimal, remote or join") != std::string::npos);
}

TEST_CASE("features: session modes are complete bases") {
    rbx::features::Set s;
    std::string err;

    REQUIRE(rbx::features::applyMode(&s, "join", &err));
    CHECK(s.world == rbx::features::WorldMode::None);
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);

    REQUIRE(rbx::features::applyMode(&s, "world", &err));
    CHECK(s.world == rbx::features::WorldMode::Full);
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);

    REQUIRE(rbx::features::applyMode(&s, "script", &err));
    CHECK(s.world == rbx::features::WorldMode::Full);
    CHECK(s.luau);
    CHECK_FALSE(s.render);

    REQUIRE(rbx::features::applyMode(&s, "full", &err));
    CHECK(s.world == rbx::features::WorldMode::Full);
    CHECK(s.luau);
    CHECK(s.render);
    CHECK(s.gui);

    CHECK_FALSE(rbx::features::applyMode(&s, "turbo", &err));
    CHECK(err.find("join, world, script or full") != std::string::npos);
}

TEST_CASE("features: world is a separate dimension") {
    rbx::features::Set s;
    std::string err;

    REQUIRE(rbx::features::applyWorld(&s, "none", &err));
    CHECK(s.world == rbx::features::WorldMode::None);
    REQUIRE(rbx::features::applyWorld(&s, "full", &err));
    CHECK(s.world == rbx::features::WorldMode::Full);
    CHECK_FALSE(rbx::features::applyWorld(&s, "half", &err));
    CHECK(err.find("--world must be none or full") != std::string::npos);

    // An explicit feature list leaves the world choice alone.
    REQUIRE(rbx::features::applyWorld(&s, "none", &err));
    REQUIRE(rbx::features::applyList(&s, "luau", &err));
    CHECK(s.world == rbx::features::WorldMode::None);
    CHECK(s.luau);
    CHECK_FALSE(rbx::features::validate(s, &err));
    CHECK(err.find("cannot run Luau") != std::string::npos);

    REQUIRE(rbx::features::applyList(&s, "world=full", &err));
    CHECK(s.world == rbx::features::WorldMode::Full);
    CHECK(rbx::features::validate(s, &err));
}

TEST_CASE("features: join-only rejects features that need a world") {
    rbx::features::Set s;
    std::string err;
    REQUIRE(rbx::features::preset(&s, "join", &err));
    CHECK(rbx::features::validate(s, &err));

    s.render = true;
    CHECK_FALSE(rbx::features::validate(s, &err));
    CHECK(err.find("render the world") != std::string::npos);

    s.render = false;
    s.gui = true;
    CHECK_FALSE(rbx::features::validate(s, &err));
    CHECK(err.find("GUI tree") != std::string::npos);

    s.gui = false;
    s.luau = true;
    CHECK_FALSE(rbx::features::validate(s, &err));
    CHECK(err.find("run Luau") != std::string::npos);
}

TEST_CASE("features: an explicit list is exact") {
    rbx::features::Set s;
    std::string err;
    REQUIRE(rbx::features::applyList(&s, "luau", &err));
    CHECK(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);
    CHECK_FALSE(s.audio);

    REQUIRE(rbx::features::applyList(&s, "luau,gui", &err));
    CHECK(s.luau);
    CHECK_FALSE(s.render);
    CHECK(s.gui);

    REQUIRE(rbx::features::applyList(&s, "all", &err));
    CHECK(s.luau);
    CHECK(s.render);
    CHECK(s.gui);
    CHECK(s.audio);

    REQUIRE(rbx::features::applyList(&s, "none", &err));
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);
    CHECK_FALSE(s.audio);

    REQUIRE(rbx::features::applyList(&s, "audio", &err));
    CHECK(s.audio);
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);
}

TEST_CASE("features: physics modes are explicit") {
    rbx::features::Set s;
    std::string err;
    REQUIRE(rbx::features::applyList(&s, "luau,physics=off", &err));
    CHECK(s.physics == rbx::features::PhysicsMode::Off);
    REQUIRE(rbx::features::applyList(&s, "luau,physics=server", &err));
    CHECK(s.physics == rbx::features::PhysicsMode::Server);
    REQUIRE(rbx::features::applyList(&s, "luau,physics=client", &err));
    CHECK(s.physics == rbx::features::PhysicsMode::Client);
}

TEST_CASE("features: unavailable names fail loudly") {
    rbx::features::Set s;
    std::string err;
    for (const char* name : {"mic"}) {
        err.clear();
        CHECK_FALSE(rbx::features::applyList(&s, name, &err));
        CHECK(err.find("not implemented") != std::string::npos);
    }
    for (const char* name : {"camera", "input", "controller"}) {
        err.clear();
        CHECK(rbx::features::applyList(&s, name, &err));
    }
    err.clear();
    CHECK(rbx::features::applyList(&s, "audio", &err));
    CHECK(s.audio);
    CHECK_FALSE(rbx::features::applyList(&s, "nope", &err));
    CHECK(err.find("unknown feature") != std::string::npos);
}

TEST_CASE("config: JSON session config resolves like the CLI") {
    rbx::features::SessionConfig c;
    std::string err;
    REQUIRE(rbx::features::parseSessionConfig(
        R"({"profile":"full","mode":"join","pump_ms":4,"seconds":90,"spawn_retry":2,"spawn_retry_after":45})",
        &c, &err));
    CHECK(c.mode == "join");
    CHECK(c.pump_ms == 4);
    CHECK(c.seconds == 90);
    CHECK(c.spawn_retry == 2);
    CHECK(c.spawn_retry_after == 45);

    rbx::features::SessionConfig defaults;
    REQUIRE(rbx::features::parseSessionConfig("{}", &defaults, &err));
    CHECK(defaults.spawn_retry == -1);
    CHECK(defaults.spawn_retry_after == 30);

    rbx::features::Set s;
    REQUIRE(rbx::features::applySessionConfig(c, &s, &err));
    CHECK(s.world == rbx::features::WorldMode::None);
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);

    // A features array is joined into the same comma list the CLI accepts;
    // mode is the base and the explicit list refines it.
    REQUIRE(rbx::features::parseSessionConfig(
        R"({"mode":"join","features":["luau","world=full"]})", &c, &err));
    REQUIRE(rbx::features::applySessionConfig(c, &s, &err));
    CHECK(s.world == rbx::features::WorldMode::Full);
    CHECK(s.luau);
    CHECK_FALSE(s.render);

    REQUIRE(rbx::features::parseSessionConfig(R"({"world":"none"})", &c, &err));
    REQUIRE(rbx::features::applySessionConfig(c, &s, &err));
    CHECK(s.world == rbx::features::WorldMode::None);
    CHECK_FALSE(s.luau);
    CHECK_FALSE(s.render);
    CHECK_FALSE(s.gui);
}

TEST_CASE("config: a bad config fails loudly") {
    rbx::features::SessionConfig c;
    std::string err;
    CHECK_FALSE(rbx::features::parseSessionConfig("not json", &c, &err));
    CHECK(err.find("not valid JSON") != std::string::npos);

    CHECK_FALSE(rbx::features::parseSessionConfig(R"({"nope":1})", &c, &err));
    CHECK(err.find("unknown key 'nope'") != std::string::npos);

    CHECK_FALSE(rbx::features::parseSessionConfig(R"({"pump_ms":0})", &c, &err));
    CHECK(err.find("pump_ms") != std::string::npos);

    CHECK_FALSE(rbx::features::parseSessionConfig(
        R"({"spawn_retry_after":0})", &c, &err));
    CHECK(err.find("spawn_retry_after") != std::string::npos);

    CHECK_FALSE(
        rbx::features::parseSessionConfig(R"({"seconds":"soon"})", &c, &err));
    CHECK(err.find("must be an integer") != std::string::npos);

    CHECK_FALSE(
        rbx::features::parseSessionConfig(R"({"features":[1]})", &c, &err));
    CHECK(err.find("array of strings") != std::string::npos);

    REQUIRE(rbx::features::parseSessionConfig(R"({"mode":"join"})", &c, &err));
    c.features = "render";
    rbx::features::Set s;
    CHECK_FALSE(rbx::features::applySessionConfig(c, &s, &err));
    CHECK(err.find("cannot render") != std::string::npos);
}

TEST_CASE("avatar watch: retry is opt-in, late and limited") {
    rbx::client::AvatarWatch w;
    w.observe(false);

    // Opt-in: limit 0 never retries, even long after the deadline.
    CHECK_FALSE(w.retryDue(true, 300.0, 30.0, 0, 0, true));
    // A healthy but slow join is not torn down at 12 s, or before the gate.
    CHECK_FALSE(w.retryDue(true, 12.0, 30.0, 0, 1, true));
    CHECK_FALSE(w.retryDue(true, 29.9, 30.0, 0, 1, true));
    // Pinned credentials or no world cannot rejoin.
    CHECK_FALSE(w.retryDue(true, 30.0, 30.0, 0, 1, false));
    CHECK_FALSE(w.retryDue(false, 30.0, 30.0, 0, 1, true));
    // At the deadline, with a retry left, a never-linked join may retry once.
    CHECK(w.retryDue(true, 30.0, 30.0, 0, 1, true));
    CHECK_FALSE(w.retryDue(true, 90.0, 30.0, 1, 1, true));
}

TEST_CASE("avatar watch: a linked Character latches the session") {
    rbx::client::AvatarWatch w;
    w.observe(false);
    w.observe(true); // the RCC linked our avatar
    CHECK(w.ever_replicated());
    CHECK(w.replicated_now());

    // A respawn/model swap briefly clears the link; it is not a failed join.
    w.observe(false);
    CHECK(w.ever_replicated());
    CHECK_FALSE(w.replicated_now());
    CHECK_FALSE(w.retryDue(true, 600.0, 30.0, 0, 10, true));
    CHECK_FALSE(w.noteDue(true, 600.0, 30.0));
}

TEST_CASE("avatar watch: the missing-avatar note fires once") {
    rbx::client::AvatarWatch w;
    w.observe(false);
    CHECK_FALSE(w.noteDue(false, 60.0, 30.0)); // no world yet
    CHECK_FALSE(w.noteDue(true, 29.9, 30.0));
    CHECK(w.noteDue(true, 30.0, 30.0));
    CHECK_FALSE(w.noteDue(true, 31.0, 30.0));
}
