// player_web.h — authenticated Player social data from the Roblox web APIs.
//
// The native client answers Player:IsInGroup / GetRankInGroup /
// GetRoleInGroup from the same group-role documents and Player:GetFriendsOnline
// from the online-friends endpoint.  This module keeps that HTTP/JSON policy
// out of the Lua binding layer so it can be unit-tested with a fake transport.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rbx::playerweb {

struct GroupRole {
    bool member = false;
    int rank = 0;
    std::string role = "Guest";
};

struct FriendPresence {
    std::int64_t visitor_id = 0;
    std::string user_name;
    std::string display_name;
    bool is_online = true;
    std::int64_t place_id = 0;
    std::string game_id;
    std::string last_online;
    std::string last_location;
    int location_type = 0;
};

// Looks up the account's role in one group.  Returns false only for a
// transport/protocol failure; a non-member is a successful default result.
bool group_role(std::int64_t user_id, std::int64_t group_id,
                GroupRole* out, std::string* error);

// Returns the online friends in the server's order.  The native client
// ignores the caller's maxFriends argument; this follows that observable
// behavior and keeps only the endpoint's 200-entry bound.
bool online_friends(std::int64_t user_id, int max_friends,
                    std::vector<FriendPresence>* out, std::string* error);

} // namespace rbx::playerweb