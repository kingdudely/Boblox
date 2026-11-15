// client_events.cpp — wire-id -> argument-type dictionary for 0x1c events.
//
// Evidence (2026-11-15, libroblox.so via IDA + a live native 1818 session):
//
//  * sub_65588CE passes the Event object (instance[11]) and its 16-byte
//    argument descriptors (instance[12]) to sub_6558160. The writer takes the
//    u16 wire id from Event+0 and each value's schema type from descriptor+1;
//    sub_64F0256 switches on that byte and writes only the value payload (no
//    tag byte). RemoteEvent/RemoteFunction arguments are a top-level Tuple
//    (type 0x1D): varint count followed by self-tagged elements.
//  * gdb on sub_6558160 read Event+32 -> EventDescriptor; descriptor+24 holds
//    the global descriptor id in its high dword and descriptor+8 is the Name.
//    The id -> descriptor-name mapping below was read from those live events
//    and cross-checked against the 7,674-line registration log
//    (/tmp/event_reg3.log): e.g. wire 0x1047 -> descriptor 4027
//    PlayerChatTranslationSettingsLocaleSetFromLua, wire 0x11f3 -> 5472
//    OnRemoteServerEvent, wire 0x1200 -> 5479 RemoteOnInvokeServer.
//  * A descriptor can own multiple wire ids (one per event object). Ids that
//    have no live descriptor trace are kept only when their payload parses
//    byte-for-byte as the same proven signature; their names say "alias".
//
// The table is deliberately a dictionary, not a schema: an id absent here is
// unknown and the caller preserves the inner body and reports the id.
#include "solve/client_events.h"

#include "solve/repl_internal.h"

#include <string>
#include <vector>

namespace rbxclient {
namespace {

// Argument type bytes are EventDescriptor entry[0][1] values. They use the
// same numbering as the replication variant codec; see read_tagged_value.
constexpr uint8_t kString = 0x02;   // [varint len][bytes]
constexpr uint8_t kEnum = 0x07;     // varint, descriptor[4] names the enum table
constexpr uint8_t kBool = 0x09;     // one byte
constexpr uint8_t kZigZag = 0x0A;   // signed zigzag varint
constexpr uint8_t kF32 = 0x0B;      // four bytes
constexpr uint8_t kRef = 0x1C;      // [varint lo][u32 id] when lo != 0
constexpr uint8_t kTuple = 0x1D;    // [varint n][n x tagged value]
constexpr uint8_t kInt64 = 0x2C;    // varint (signed in the codec)
constexpr uint8_t kUInt = 0x22;     // unsigned varint

// Eighteen wire ids: eleven with a live EventDescriptor trace, five
// signature aliases, and two unidentified SDP/asset payloads. The comments
// name the evidence class, not a guess.
constexpr ClientEventDef kClientEvents[] = {
    // --- live descriptor trace (EventDescriptor name + id) ---
    {0x011B, "OnStreamingUpdated2", 5,
     {kF32, kF32, kEnum, kBool, kZigZag}},                     // 283
    {0x1047, "PlayerChatTranslationSettingsLocaleSetFromLua", 1,
     {kString}},                                               // 4027
    {0x1054, "SimulationRadiusChanged", 1, {kF32}},            // 4034
    {0x11F3, "OnRemoteServerEvent", 1, {kTuple}},              // 5472
    {0x1200, "RemoteOnInvokeServer", 3, {kZigZag, kRef, kTuple}}, // 5479
    {0x14E4, "PlayingUpdatedFromClient", 1, {kBool}},          // 2169
    {0x14EA, "playbackActionSync", 1, {kZigZag}},              // 2175
    {0x176C, "ClientToServerChatableUserCountRequestSignal", 1,
     {kString}},                                               // 2294
    {0x1AF3, "VoiceChatClientVoiceCapabilityWithConfig", 1,
     {kInt64}},                                                // 6302

    // --- signature aliases: the wire ids occur in the corpus but the live
    //     session that produced it never instantiated their event object.
    //     Each parses as the same proven signature; the name is the live
    //     descriptor they alias, not a new claim. ---
    {0x100F, "PlayerChatTranslationSettingsLocaleSetFromLua (alias)", 1,
     {kString}},
    {0x101C, "SimulationRadiusChanged (alias)", 1, {kF32}},
    {0x11BB, "OnRemoteServerEvent (alias)", 1, {kTuple}},
    {0x11C8, "RemoteOnInvokeServer (alias)", 3, {kZigZag, kRef, kTuple}},
    {0x1733, "ClientToServerChatableUserCountRequestSignal (alias)", 1,
     {kString}},

    // --- SDP/asset payloads. The descriptor itself was not instantiated in
    //     the live run, so the name is deliberately neutral; the wire shape is
    //     pinned by the native capture: a byte count plus a zstd frame. ---
    // 0x1AAD: [u32-ish varint size][string compressed SDP][bool][string id]
    {0x1AAD, "UnidentifiedClientPayloadA", 4,
     {kUInt, kString, kBool, kString}},
    // 0x1AA3: [string compressed SDP][bool][string id]
    {0x1AA3, "UnidentifiedClientPayloadB", 3,
     {kString, kBool, kString}},

    // --- later SDP handshake objects. The live descriptor trace read both
    //     names and argument counts directly: 0x1ADD -> descriptor 6288
    //     PublishingHandshakeInitiated (3 args), 0x1AE7 -> descriptor 6312
    //     SubscriptionHandshakeAcked (4 args). The capture pins the value
    //     extents: [string SDP][bool][string] and
    //     [uint size][string SDP][bool][string]. ---
    {0x1ADD, "PublishingHandshakeInitiated", 3,
     {kString, kBool, kString}},
    {0x1AE7, "SubscriptionHandshakeAcked", 4,
     {kUInt, kString, kBool, kString}},
};

constexpr size_t kClientEventCount =
    sizeof(kClientEvents) / sizeof(kClientEvents[0]);

} // namespace

const ClientEventDef* findClientEvent(uint16_t id) {
    for (size_t i = 0; i < kClientEventCount; ++i) {
        if (kClientEvents[i].id == id)
            return &kClientEvents[i];
    }
    return nullptr;
}

bool parseClientEventItem(const uint8_t* d, size_t n, EventItem& out,
                          size_t* consumed, bool* unknown_id,
                          std::string* err) {
    if (unknown_id)
        *unknown_id = false;
    out.args.clear();
    out.client_event = false;
    out.client_event_name = nullptr;

    detail::Cursor c(d, n);
    int64_t ent = -1;
    if (!detail::read_ref(c, out.lo, ent, err, "client event entity"))
        return false;
    out.entity = uint32_t(ent);
    const uint8_t* b = nullptr;
    if (!c.take(2, &b, err, "client event id"))
        return false;
    out.event = detail::u16le(b);
    const ClientEventDef* def = findClientEvent(out.event);
    if (!def) {
        if (unknown_id)
            *unknown_id = true;
        return detail::fail(err, "unknown client event id " +
                                    std::to_string(out.event));
    }

    out.client_event = true;
    out.client_event_name = def->name;
    for (uint8_t i = 0; i < def->nargs; ++i) {
        EventArg a;
        a.tag = def->args[i];
        const std::string ctx =
            std::string(def->name) + " arg " + std::to_string(i + 1);
        if (a.tag >= 29 && a.tag <= 32) {
            if (!detail::read_tuple_value(c, a.tag, true, 0, ctx, a, err))
                return false;
        } else if (!detail::read_tagged_value(c, a.tag, true, ctx, a.value,
                                              err)) {
            return false;
        }
        out.args.push_back(std::move(a));
    }
    if (consumed)
        *consumed = c.off();
    return true;
}

} // namespace rbxclient
