#include "join/join.h"

#include "common/util.h"

#include <curl/curl.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace rbx {

namespace {

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud) {
  static_cast<std::string*>(ud)->append(ptr, size * nmemb);
  return size * nmemb;
}

bool httpPost(const std::string& url, const std::string& body,
              const std::string& cookie, const std::string& accept,
              long& status, std::string& resp, std::string* err) {
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl_easy_init failed";
    return false;
  }
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "User-Agent: Roblox/WinInet");
  if (!cookie.empty()) hdrs = curl_slist_append(hdrs, ("Cookie: " + cookie).c_str());
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json; charset=utf-8");
  if (!accept.empty()) hdrs = curl_slist_append(hdrs, ("Accept: " + accept).c_str());
  resp.clear();
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(body.size()));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK) {
    if (err) *err = std::string("curl: ") + curl_easy_strerror(rc);
    return false;
  }
  return true;
}

bool httpGet(const std::string& url, const std::string& cookie, long& status,
             std::string& resp, std::string* err) {
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl_easy_init failed";
    return false;
  }
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "User-Agent: Roblox/WinInet");
  if (!cookie.empty()) hdrs = curl_slist_append(hdrs, ("Cookie: " + cookie).c_str());
  resp.clear();
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK) {
    if (err) *err = std::string("curl: ") + curl_easy_strerror(rc);
    return false;
  }
  return true;
}

std::string cookieHeader(const std::string& cookie) {
  if (cookie.empty()) return "";
  // already a header if first "name=value" contains '='
  size_t semi = cookie.find(';');
  std::string first = semi == std::string::npos ? cookie : cookie.substr(0, semi);
  if (first.find('=') != std::string::npos) return cookie;
  return ".ROBLOSECURITY=" + cookie;
}

} // namespace

bool joinGame(long long place, const std::string& cookie_raw, const std::string& job_id,
              int follow_user, long universe_id, JoinResult& out,
              std::string* err) {
  static bool inited = (curl_global_init(CURL_GLOBAL_DEFAULT) == 0);
  if (!inited) {
    if (err) *err = "curl_global_init failed";
    return false;
  }
  std::string cookie = cookieHeader(cookie_raw);

  nlohmann::ordered_json body;
  body["placeId"] = place;
  body["isTeleport"] = false;
  body["gameJoinAttemptId"] = uuid4();
  body["browserTrackerId"] = 0;
  body["playSessionId"] = uuid4();
  body["eventId"] = uuid4();
  body["launchData"] = "";
  body["joinAttemptOrigin"] = "PlayButton";
  if (!job_id.empty()) body["jobId"] = job_id;
  if (follow_user > 0) body["followUserId"] = follow_user;
  if (universe_id > 0) body["gameId"] = universe_id;
  std::string payload = body.dump();

  // v2/join-game was retired server-side: it answers HTTP 404
  // ({"errors":[{"code":0,"message":""}]}) even with a valid
  // authenticated cookie (verified live 2026-10-26/27). v1 is the only path.
  long st = 0;
  std::string resp;
  if (!httpPost("https://gamejoin.roblox.com/v1/join-game", payload, cookie, "",
                st, resp, err))
    return false;
  nlohmann::json reply = nlohmann::json::parse(resp, nullptr, false);
  if (reply.is_discarded()) {
    if (err) *err = "v1: bad json";
    return false;
  }
  if (reply.value("status", -1) == 2 && reply.contains("joinScript") &&
      reply["joinScript"].is_object()) {
    out.reply = reply;
    out.join_script = reply["joinScript"];
    return true;
  }
  if (reply.value("status", -1) == 2 && reply.contains("joinScriptUrl")) {
    long st2 = 0;
    std::string resp2;
    if (!httpGet(reply["joinScriptUrl"].get<std::string>(), cookie, st2, resp2, err))
      return false;
    nlohmann::json js = nlohmann::json::parse(resp2, nullptr, false);
    if (js.is_discarded()) {
      if (err) *err = "joinScriptUrl: bad json";
      return false;
    }
    out.reply = reply;
    if (js.contains("joinScript")) {
      out.join_script = js["joinScript"];
    } else {
      out.join_script = js;
    }
    if (!out.join_script.is_object()) {
      if (err)
        *err = "joinScriptUrl: script is not an object";
      return false;
    }
    return true;
  }
  if (err) {
    std::string st_s = reply.contains("status") ? reply["status"].dump() : "?";
    std::string msg = reply.contains("message") ? reply["message"].dump() : "?";
    *err = "join failed: status=" + st_s + " message=" + msg;
  }
  return false;
}

bool resolveUniverse(long long place, long* universe, std::string* err) {
  static bool inited = (curl_global_init(CURL_GLOBAL_DEFAULT) == 0);
  if (!inited) {
    if (err) *err = "curl_global_init failed";
    return false;
  }
  long status = 0;
  std::string resp;
  const std::string url =
      "https://apis.roblox.com/universes/v1/places/" + std::to_string(place) +
      "/universe";
  if (!httpGet(url, "", status, resp, err)) return false;
  nlohmann::json j = nlohmann::json::parse(resp, nullptr, false);
  if (j.is_discarded() || !j.contains("universeId")) {
    if (err) *err = "universe lookup: HTTP " + std::to_string(status) + " " + resp;
    return false;
  }
  *universe = j["universeId"].get<long>();
  return true;
}

bool saveJoin(const std::string& path, const JoinResult& j, std::string* err) {
  nlohmann::ordered_json o;
  o["joinScript"] = j.join_script;
  o["reply"] = j.reply;
  std::ofstream f(path);
  if (!f) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  f << o.dump(1);
  return true;
}

bool loadJoin(const std::string& path, JoinResult& out, std::string* err) {
  std::ifstream f(path);
  if (!f) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
  if (j.is_discarded()) {
    if (err) *err = "bad json: " + path;
    return false;
  }
  if (j.contains("joinScript")) {
    out.join_script = j["joinScript"];
    out.reply = j.value("reply", nlohmann::json());
  } else {
    out.join_script = j;
    out.reply = nlohmann::json();
  }
  return true;
}

} // namespace rbx
