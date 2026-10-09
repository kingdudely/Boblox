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

// Cookie may be a raw .ROBLOSECURITY value or a full header. job_id and
// follow_user optional (""/0 = unset). Returns false + *err on failure.
bool joinGame(int place, const std::string& cookie, const std::string& job_id,
              int follow_user, JoinResult& out, std::string* err);

// Read a cookie file (raw value or Cookie: header line) -> header value.
std::string loadCookieFile(const std::string& path, std::string* err);

// Save {"joinScript":…, "reply":…} (superset of dump_join.py format).
bool saveJoin(const std::string& path, const JoinResult& j, std::string* err);

// Load either {"joinScript":…, "reply":…} or a bare joinScript / dump_join
// file (reply may then be empty — 0x90 rebuild will fail without it).
bool loadJoin(const std::string& path, JoinResult& out, std::string* err);

} // namespace rbx
