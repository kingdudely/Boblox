#include "instance/player_web.h"

#include "instance/web_http.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <utility>

namespace rbx::playerweb {
namespace {

std::string decimal(std::int64_t value) {
    char buffer[32];
    auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

bool request_json(const std::string& method, const std::string& url,
                  const std::string& body, bool content_type,
                  nlohmann::json* out, std::string* error) {
    std::string headers = "Accept: application/json";
    if (content_type) headers += "\nContent-Type: application/json";
    const std::string cookie = web::session_cookie_header();
    if (!cookie.empty()) headers += "\nCookie: " + cookie;
    web::HttpResponse response;
    if (!web::http_request(method, url, body, headers, &response)) {
        *error = response.error.empty() ? "HTTP request failed"
                                        : response.error;
        return false;
    }
    if (response.status != 200) {
        *error = "HTTP " + std::to_string(response.status);
        return false;
    }
    *out = nlohmann::json::parse(response.body, nullptr, false);
    if (out->is_discarded()) {
        *error = "invalid JSON response";
        return false;
    }
    return true;
}

bool get_json(const std::string& url, nlohmann::json* out,
              std::string* error) {
    return request_json("GET", url, "", false, out, error);
}

bool post_json(const std::string& url, const nlohmann::json& body,
               nlohmann::json* out, std::string* error) {
    return request_json("POST", url, body.dump(), true, out, error);
}

const nlohmann::json* field(const nlohmann::json& object, const char* name) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(name);
    return it == object.end() ? nullptr : &*it;
}

std::string string_field(const nlohmann::json& object, const char* name) {
    const nlohmann::json* value = field(object, name);
    return value && value->is_string() ? value->get<std::string>() : std::string();
}

std::int64_t int_field(const nlohmann::json& object, const char* name,
                       std::int64_t fallback = 0) {
    const nlohmann::json* value = field(object, name);
    if (!value || !value->is_number_integer()) return fallback;
    return value->get<std::int64_t>();
}

int location_type(const nlohmann::json& presence) {
    const std::string type = string_field(presence, "UserLocationType");
    if (type == "CloudEdit" || type == "Studio") return 6;
    if (type == "Page" || type == "Website") return 2;
    if (type == "Game" || type == "InGame") return 4;
    const std::string presence_type = string_field(presence, "UserPresenceType");
    if (presence_type == "InStudio") return 6;
    if (presence_type == "InGame") return 4;
    if (presence_type == "Online") return 2;
    return 0;
}

std::string last_location(const nlohmann::json& presence) {
    return string_field(presence, "lastLocation");
}

bool fetch_names(const std::vector<std::int64_t>& ids,
                 std::vector<std::string>* names,
                 std::vector<std::string>* display_names, std::string* error) {
    nlohmann::json body;
    body["userIds"] = ids;
    body["excludeBannedUsers"] = true;
    nlohmann::json document;
    if (!post_json("https://users.roblox.com/v1/users", body, &document,
                   error))
        return false;
    const nlohmann::json* data = field(document, "data");
    if (!data || !data->is_array()) {
        *error = "users response has no data array";
        return false;
    }
    names->assign(ids.size(), std::string());
    display_names->assign(ids.size(), std::string());
    for (const auto& user : *data) {
        const std::int64_t id = int_field(user, "id");
        const auto found = std::find(ids.begin(), ids.end(), id);
        if (found == ids.end()) continue;
        const size_t index = size_t(found - ids.begin());
        (*names)[index] = string_field(user, "name");
        (*display_names)[index] = string_field(user, "displayName");
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        if ((*names)[i].empty()) {
            *error = "users response is missing a friend name";
            return false;
        }
    }
    return true;
}

} // namespace

bool group_role(std::int64_t user_id, std::int64_t group_id,
                GroupRole* out, std::string* error) {
    if (!out || !error) return false;
    *out = GroupRole{};
    if (web::session_cookie_header().empty()) {
        *error = "not signed in";
        return false;
    }
    if (user_id <= 0 || group_id <= 0) {
        *error = "invalid user or group id";
        return false;
    }
    const std::string url = "https://groups.roblox.com/v1/users/" +
                            decimal(user_id) + "/groups/roles";
    nlohmann::json document;
    if (!get_json(url, &document, error)) return false;
    const nlohmann::json* data = field(document, "data");
    if (!data || !data->is_array()) {
        *error = "groups response has no data array";
        return false;
    }
    for (const auto& entry : *data) {
        const nlohmann::json* group = field(entry, "group");
        if (!group || int_field(*group, "id") != group_id) continue;
        out->member = true;
        const nlohmann::json* role = field(entry, "role");
        if (!role) {
            out->rank = 0;
            out->role = "Guest";
            return true;
        }
        out->rank = int(int_field(*role, "rank"));
        out->role = string_field(*role, "name");
        if (out->role.empty()) out->role = "Guest";
        // The measured native client reports a base role as rank 0/Guest
        // while still answering IsInGroup=true.  Preserve that observable
        // split instead of silently correcting the native result.
        const nlohmann::json* is_base = field(*role, "isBase");
        if (is_base && is_base->is_boolean() && is_base->get<bool>()) {
            out->rank = 0;
            out->role = "Guest";
        }
        return true;
    }
    return true;
}

bool online_friends(std::int64_t user_id, int max_friends,
                    std::vector<FriendPresence>* out, std::string* error) {
    if (!out || !error) return false;
    out->clear();
    if (web::session_cookie_header().empty()) {
        *error = "not signed in";
        return false;
    }
    if (user_id <= 0) {
        *error = "invalid user id";
        return false;
    }
    // The measured native client ignores the caller's maxFriends argument:
    // GetFriendsOnline(1) still returned the complete online list (10 entries
    // in the 2026-11-13 23:53 run).  The endpoint itself is bounded at 200.
    (void)max_friends;
    constexpr int kServerFriendLimit = 200;
    const std::string url = "https://friends.roblox.com/v1/users/" +
                            decimal(user_id) + "/friends/online?origin=getFriendsOnline";
    nlohmann::json document;
    if (!get_json(url, &document, error)) return false;
    const nlohmann::json* data = field(document, "data");
    if (!data || !data->is_array()) {
        *error = "friends response has no data array";
        return false;
    }

    std::vector<std::int64_t> ids;
    for (const auto& entry : *data) {
        const std::int64_t id = int_field(entry, "id");
        if (id <= 0) continue;
        ids.push_back(id);
        if (int(ids.size()) >= kServerFriendLimit) break;
    }
    if (ids.empty()) return true;

    std::vector<std::string> names;
    std::vector<std::string> display_names;
    if (!fetch_names(ids, &names, &display_names, error)) return false;

    size_t index = 0;
    for (const auto& entry : *data) {
        const std::int64_t id = int_field(entry, "id");
        if (id <= 0) continue;
        if (index >= ids.size() || ids[index] != id) continue;
        const nlohmann::json* presence = field(entry, "userPresence");
        if (!presence || !presence->is_object()) {
            *error = "friend response is missing userPresence";
            return false;
        }
        FriendPresence friend_info;
        friend_info.visitor_id = id;
        friend_info.user_name = names[index];
        friend_info.display_name = display_names[index];
        friend_info.is_online = true;
        friend_info.last_online = string_field(*presence, "lastOnline");
        friend_info.last_location = last_location(*presence);
        friend_info.location_type = location_type(*presence);
        if (const nlohmann::json* place = field(*presence, "placeId");
            place && place->is_number_integer())
            friend_info.place_id = place->get<std::int64_t>();
        if (const nlohmann::json* game = field(*presence, "gameInstanceId");
            game && game->is_string())
            friend_info.game_id = game->get<std::string>();
        out->push_back(std::move(friend_info));
        ++index;
        if (int(out->size()) >= kServerFriendLimit) break;
    }
    return true;
}

} // namespace rbx::playerweb