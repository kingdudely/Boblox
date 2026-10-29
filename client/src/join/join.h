// join.h — HTTPS game join (gamejoin.roblox.com), port of
// py/rbx_client.py::join_game with the same v2-SSE → v1 fallback order.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace rbx {

struct JoinResult {
  nlohmann::json join_script; // joinScript (PlaceId, GameId, udmux, ticket...)
  nlohmann::json reply;       // full reply incl. joinTicket (needed by 0x90)
};

// Cookie may be a raw .ROBLOSECURITY value or a full header. job_id,
// follow_user and universe_id optional (""/0 = unset; universe_id is the
// place's universeId = the join body's gameId — REQUIRED in practice:
// without it v1/join-game answers status 12 "Unable to join Game <n>").
// Returns false + *err on failure.
bool joinGame(long long place, const std::string& cookie, const std::string& job_id,
              int follow_user, long universe_id, JoinResult& out,
              std::string* err);

// placeId -> universeId (apis.roblox.com/universes/v1/places/<id>/universe);
// the value v1/join-game wants as gameId (1818 -> 13058).
bool resolveUniverse(long long place, long* universe, std::string* err);

// Player hydration (apis.roblox.com/player-hydration-service/v1/players/signed
// ?userId=<id>, the same call the native client makes). Returns the fields the
// join document (0x90) carries but the join script does not, already named
// like the document's keys: AgeBracket, Gender, Platform, Os, IsOriginalUser
// and OriginalAccountCreationTimestampMs. Without it those keys keep whatever
// the *template* had, i.e. another account's values.
bool fetchPlayerHydration(long long user_id, const std::string& cookie,
                          nlohmann::json* fields, std::string* err);

// Save {"joinScript":…, "reply":…} (superset of dump_join.py format).
bool saveJoin(const std::string& path, const JoinResult& j, std::string* err);

// Load either {"joinScript":…, "reply":…} or a bare joinScript / dump_join
// file (reply may then be empty — 0x90 rebuild will fail without it).
bool loadJoin(const std::string& path, JoinResult& out, std::string* err);

} // namespace rbx
