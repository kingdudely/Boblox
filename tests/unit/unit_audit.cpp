// unit_audit.cpp — the opt-in RBX_API_AUDIT inventory contract.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "instance/api_audit.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

TEST_CASE("api audit: counts and escaped names reach one JSON object") {
    const std::string path = "/tmp/rbx_unit_audit.json";
    std::remove(path.c_str());
    REQUIRE(setenv("RBX_API_AUDIT", path.c_str(), 1) == 0);

    rbx::audit::note_global_missing("TweenInfo");
    rbx::audit::note_global_missing("TweenInfo");
    rbx::audit::note_member_missing("Part", "NotARealMember");
    rbx::audit::note_stub_call("TeleportService", "GetArrivingTeleportGui");
    rbx::audit::note_fallback_call("Player", "IsInGroup");
    rbx::audit::note_event_connect("Workspace.DescendantAdded");
    rbx::audit::note_event_connect("Workspace.DescendantAdded");
    rbx::audit::note_event_fire("Workspace.DescendantAdded");
    rbx::audit::note_event_connect("Player.AttrChanged:file\nload");
    rbx::audit::dump();

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const nlohmann::json doc = nlohmann::json::parse(in);

    CHECK(doc["missing_globals"]["TweenInfo"] == 2);
    CHECK(doc["missing_members"]["Part.NotARealMember"] == 1);
    CHECK(doc["stub_calls"]["TeleportService.GetArrivingTeleportGui"] == 1);
    CHECK(doc["fallback_calls"]["Player.IsInGroup"] == 1);
    CHECK(doc["event_connects"]["Workspace.DescendantAdded"] == 2);
    CHECK(doc["event_fires"]["Workspace.DescendantAdded"] == 1);
    CHECK(doc["event_connects"]["Player.AttrChanged:file\nload"] == 1);
    CHECK(std::remove(path.c_str()) == 0);
    unsetenv("RBX_API_AUDIT");
}

TEST_CASE("api audit: disabled means no file and no output") {
    const std::string path = "/tmp/rbx_unit_audit_disabled.json";
    std::remove(path.c_str());
    unsetenv("RBX_API_AUDIT");
    rbx::audit::note_global_missing("TweenInfo");
    rbx::audit::dump();
    std::ifstream in(path, std::ios::binary);
    CHECK_FALSE(in.good());
}
