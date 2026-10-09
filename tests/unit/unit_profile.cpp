// unit_profile.cpp — the two-profile sandbox split (rbxch::Profile).
//
// The Challenge profile is FROZEN: it must expose exactly the historical six
// modules, in order, byte-exact against the native client — the 5 captured
// datasets depend on this global surface. This test pins the list, so any
// change to the challenge surface (even an accidental kBoth tag on a new
// engine module) fails here and must be a conscious decision.
//
// The Engine profile must be a superset of Challenge (it is the future full
// scripting environment; see the Phase J instance work).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "runner.h"
#include "api/api.h"

#include <string>
#include <vector>

namespace {

std::vector<std::string> enabled_names(rbxch::Profile p) {
    std::vector<std::string> out;
    for (size_t i = 0; i < rbxch::api::kModuleCount; i++) {
        const rbxch::api::Module& m = rbxch::api::kModules[i];
        if (m.profiles & rbxch::api::profile_bit(p))
            out.push_back(m.name);
    }
    return out;
}

} // namespace

TEST_CASE("Challenge profile is frozen to the historical six modules") {
    // Frozen on purpose — do NOT extend. New engine surface goes into the
    // Engine profile only (tag kEngineOnly in api/registry.cpp).
    const std::vector<std::string> frozen = {"Random",       "game",   "RunService",
                                             "UserSettings", "os",     "newproxy"};
    CHECK(enabled_names(rbxch::Profile::Challenge) == frozen);
}

TEST_CASE("Engine profile is a superset of Challenge") {
    const auto challenge = enabled_names(rbxch::Profile::Challenge);
    const auto engine = enabled_names(rbxch::Profile::Engine);
    REQUIRE_FALSE(challenge.empty());
    for (const auto& name : challenge) {
        bool present = false;
        for (const auto& e : engine)
            present = present || (e == name);
        CHECK(present); // every challenge module also exists in Engine
    }
}

TEST_CASE("every challenge module is tagged for both profiles") {
    // The mechanism: kBoth modules appear on the frozen path; kEngineOnly
    // never may. Any kBoth is allowed, any missing bit on a challenge entry
    // would mean the solve path lost a global.
    for (size_t i = 0; i < rbxch::api::kModuleCount; i++) {
        const rbxch::api::Module& m = rbxch::api::kModules[i];
        INFO("module: " << m.name);
        CHECK((m.profiles & rbxch::api::kChallenge) != 0);
        CHECK(m.register_globals != nullptr);
    }
}
