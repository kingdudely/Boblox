// repl_pv.h — BasePart.ReplicationPV (schema tag 57) wire codec.
//
// Provenance: IDA on libroblox 2.743 (libroblox.so), sub_6432F94 case 0x39
// (InstanceStateSerialization value reader), sub_6427536 (the live codec),
// sub_5E7C3C8 / sub_5E7C2DE / sub_5E7D9AE (decode), sub_6427317 +
// sub_5E7C482 / sub_5E7D78E (encode), and the 1818/189707 0xA1 captures.
//
// The live build has byte_7710990 != 0, so the alternate (bit-packed) branch
// is the only one on the wire; the older sub_6661A42/sub_6661F98/sub_666213A
// byte-aligned branches are never reached.  The value is not scriptable:
// BasePart.ReplicationPV is Hidden, NotScriptable and RobloxSecurity in the
// API dump, and the native Lua bridge rejects it ("Cannot use ReplicationPV
// from Lua").  Boblox therefore decodes it into this internal value and does
// not add a Luau-visible property; parity requires that it stay invisible.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rbxclient {

// One 3-component compressed block.  The native stores signed fixed-point
// integers quantized to `bits` (12/20/28/36/44/52) with scale
// 2^(exponent - bits + 1); `value` is that dequantized float (studs for the
// position block, studs/tick for the two velocity blocks).
struct ReplicationPVBlock {
    bool present = false;
    uint8_t exponent = 0;
    uint8_t bits = 0;
    int64_t raw[3] = {0, 0, 0};
    double value[3] = {0.0, 0.0, 0.0};
};

// Decoded BasePart.ReplicationPV.  Native source layout (sub_643B154):
//   position        -> source +0..8
//   orientation     -> source +12..36 (a 7-float rotation, sent as a table
//                      index + packed angle/axis code, not as floats)
//   linear velocity -> source +40..48
//   angular velocity-> source +52..60
// The wire carries the first, third and fourth fields plus the orientation
// table selector.  `orientation_kind` is the native switch at struct +32:
// 0 = identity (no trailer), 1 = u32 code, 2 = u32 + u8 code, 3 = u32 + u16
// code.  `orientation_code` is the native packed angle/axis id passed to
// sub_5E7D6F0; the table itself is built deterministically by sub_5E7C570
// (entries 11/21, 14/26 and 16/32 bits).  The codec here preserves that
// selector exactly; orientation reconstruction is an engine lookup, not
// additional wire data.
struct ReplicationPV {
    uint8_t flags = 0;
    ReplicationPVBlock position;
    ReplicationPVBlock linear_velocity;
    ReplicationPVBlock angular_velocity;
    uint8_t orientation_kind = 0;
    uint64_t orientation_code = 0;

    bool has_orientation() const { return orientation_kind != 0; }
};

// Decode exactly one ReplicationPV value from [d, d+n).  Returns false when
// the value is truncated or its vectors do not end on a byte boundary.  On
// success *used is the exact value extent; any following member or attribute
// bytes are left to the record decoder.
bool decodeReplicationPV(const uint8_t* d, size_t n, ReplicationPV& out,
                         size_t* used, std::string* err);

} // namespace rbxclient
