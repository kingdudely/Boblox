// repl_pv.cpp — BasePart.ReplicationPV (schema tag 57) wire codec.
//
// The wire is bit-packed but every field group is a multiple of eight bits,
// so the native writer and reader stay byte-aligned between value members.
// Bit order is LSB-first, matching sub_391897E.  See repl_pv.h for the
// source functions and field mapping.
#include "solve/repl_pv.h"

#include <cmath>

namespace rbxclient {
namespace {

class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

    bool bits(unsigned count, uint64_t& out) {
        if (count > 52)
            return false;
        if (pos_ + count > n_ * 8)
            return false;
        uint64_t v = 0;
        for (unsigned i = 0; i < count; ++i) {
            v |= uint64_t((d_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
            ++pos_;
        }
        out = v;
        return true;
    }

    bool signed_bits(unsigned count, int64_t& out) {
        uint64_t v = 0;
        if (!bits(count, v))
            return false;
        if (count == 0) {
            out = 0;
            return true;
        }
        const uint64_t sign = uint64_t(1) << (count - 1);
        out = int64_t((v ^ sign) - sign);
        return true;
    }

    size_t bit_pos() const { return pos_; }

private:
    const uint8_t* d_;
    size_t n_;
    size_t pos_ = 0;
};

// sub_6427536 / sub_5E7C2DE width selection from the 5-bit position exponent
// (0..31) or a 4-bit velocity exponent (0..15).  `ceil(code + 6.643856...)`
// is the native _mm_round_sd(..., 10) (round toward +infinity) expression.
unsigned block_bits(unsigned code) {
    const double rounded = std::ceil(double(code) + 6.643856189774724);
    if (rounded >= 44.0)
        return 52;
    if (rounded >= 36.0)
        return 44;
    if (rounded >= 28.0)
        return 36;
    if (rounded >= 20.0)
        return 28;
    if (rounded >= 12.0)
        return 20;
    return 12;
}

// sub_5E7D9AE: value = raw * 2^(exponent - bits + 1).
double dequantize(int64_t raw, unsigned exponent, unsigned bits) {
    return double(raw) * std::ldexp(1.0, int(exponent) - int(bits) + 1);
}

bool read_block(BitReader& br, unsigned exponent, ReplicationPVBlock& out,
                const char* what, std::string* err) {
    out = ReplicationPVBlock{};
    out.present = true;
    out.exponent = uint8_t(exponent);
    out.bits = uint8_t(block_bits(exponent));
    for (int i = 0; i < 3; ++i) {
        if (!br.signed_bits(out.bits, out.raw[i])) {
            if (err)
                *err = std::string("ReplicationPV: truncated ") + what +
                       " component " + std::to_string(i);
            return false;
        }
        out.value[i] =
            dequantize(out.raw[i], out.exponent, out.bits);
    }
    return true;
}

} // namespace

bool decodeReplicationPV(const uint8_t* d, size_t n, ReplicationPV& out,
                         size_t* used, std::string* err) {
    if (used)
        *used = 0;
    if (!d) {
        if (err)
            *err = "ReplicationPV: null payload";
        return false;
    }
    BitReader br(d, n);
    out = ReplicationPV{};
    uint64_t flags = 0;
    if (!br.bits(8, flags)) {
        if (err)
            *err = "ReplicationPV: missing flags byte";
        return false;
    }
    out.flags = uint8_t(flags);

    // Bit 0 = position absent (sub_6427536 zeroes the block without reading).
    if (!(out.flags & 0x01)) {
        uint64_t low = 0;
        if (!br.bits(4, low)) {
            if (err)
                *err = "ReplicationPV: truncated position exponent";
            return false;
        }
        const unsigned exponent = unsigned(((out.flags & 0x06) << 3) | low);
        if (!read_block(br, exponent, out.position, "position", err))
            return false;
    }

    // Bit 5 = first vector absent; bit 6 = second vector absent.  Each present
    // block starts with its own 4-bit exponent.
    if (!(out.flags & 0x20)) {
        uint64_t low = 0;
        if (!br.bits(4, low)) {
            if (err)
                *err = "ReplicationPV: truncated linear-velocity exponent";
            return false;
        }
        if (!read_block(br, unsigned(low), out.linear_velocity,
                        "linear-velocity", err))
            return false;
    }
    if (!(out.flags & 0x40)) {
        uint64_t low = 0;
        if (!br.bits(4, low)) {
            if (err)
                *err = "ReplicationPV: truncated angular-velocity exponent";
            return false;
        }
        if (!read_block(br, unsigned(low), out.angular_velocity,
                        "angular-velocity", err))
            return false;
    }

    // The orientation trailer is written with byte reads in sub_6427317, so
    // the preceding blocks must have ended on a byte boundary.
    if ((br.bit_pos() & 7) != 0) {
        if (err)
            *err = "ReplicationPV: bit-packed vectors do not end on a byte";
        return false;
    }
    const size_t trailer = br.bit_pos() / 8;
    const unsigned kind = (out.flags >> 3) & 3;
    out.orientation_kind = uint8_t(kind);
    size_t width = 0;
    switch (kind) {
    case 0:
        width = 0;
        break;
    case 1:
        width = 4;
        break;
    case 2:
        width = 5;
        break;
    case 3:
        width = 6;
        break;
    default:
        if (err)
            *err = "ReplicationPV: invalid orientation selector";
        return false;
    }
    if (trailer + width > n) {
        if (err)
            *err = "ReplicationPV: truncated orientation code";
        return false;
    }
    uint64_t code = 0;
    if (width != 0) {
        for (size_t i = 0; i < 4; ++i)
            code |= uint64_t(d[trailer + i]) << (8 * i);
        for (size_t i = 4; i < width; ++i)
            code |= uint64_t(d[trailer + i]) << (8 * i);
    }
    out.orientation_code = code;
    if (used)
        *used = trailer + width;
    return true;
}

} // namespace rbxclient
