// gui_font_metrics.cpp — see gui_font_metrics.h.
#include "gui/gui_font_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rbxclient::font_metrics {

namespace {

constexpr int kFaceCount = 5;
constexpr int kSizeCount = kLastSize - kFirstSize + 1;
constexpr int kFirstChar = 32;
constexpr int kLastChar = 126;
constexpr int kCharCount = kLastChar - kFirstChar + 1;

struct KernPair {
    uint16_t code;
    int8_t adjustment[kSizeCount];
};

struct UnicodeAdvance {
    uint32_t codepoint;
    uint16_t advance[kSizeCount];
};

struct UnicodeFaceData {
    const UnicodeAdvance* entries;
    int count;
};

struct UnicodeKernPair {
    uint32_t code;
    int8_t adjustment[kSizeCount];
};

struct FaceData {
    const uint16_t (*advance)[kCharCount];
    const KernPair* pairs;
    int pair_count;
    const int8_t (*space_adjustment)[3];
    int line_numerator;
    int line_denominator;
    double pixel_scale;
    const UnicodeAdvance* unicode;
    int unicode_count;
    const UnicodeKernPair* unicode_pairs;
    int unicode_pair_count;
};

#include "gui_font_metrics_data.inc"

bool bold_face(int font) {
    switch (font) {
    case 2:
    case 5:
    case 7:
    case 10:
    case 13:
    case 16:
    case 18:
    case 20:
    case 21:
        return true;
    default:
        return false;
    }
}

int unicodePairAdjustment(const FaceData& face, uint32_t first,
                          uint32_t second, int size_index) {
    const uint32_t code = (first << 16) | second;
    const UnicodeKernPair* begin = face.unicode_pairs;
    const UnicodeKernPair* end = begin + face.unicode_pair_count;
    const UnicodeKernPair* it =
        std::lower_bound(begin, end, code,
                         [](const UnicodeKernPair& pair, uint32_t key) {
                             return pair.code < key;
                         });
    return it != end && it->code == code ? it->adjustment[size_index] : 0;
}

int pairAdjustment(const FaceData& face, unsigned char first,
                   unsigned char second, int size_index) {
    const uint16_t code = uint16_t((unsigned(first) << 8) | second);
    const KernPair* begin = face.pairs;
    const KernPair* end = face.pairs + face.pair_count;
    const KernPair* it =
        std::lower_bound(begin, end, code,
                         [](const KernPair& pair, uint16_t key) {
                             return pair.code < key;
                         });
    return it != end && it->code == code ? it->adjustment[size_index] : 0;
}

const UnicodeAdvance* unicodeAdvance(const FaceData& face,
                                     uint32_t codepoint) {
    const UnicodeAdvance* begin = face.unicode;
    const UnicodeAdvance* end = begin + face.unicode_count;
    const UnicodeAdvance* it =
        std::lower_bound(begin, end, codepoint,
                         [](const UnicodeAdvance& entry, uint32_t key) {
                             return entry.codepoint < key;
                         });
    return it != end && it->codepoint == codepoint ? it : nullptr;
}

int unicodeWidth(const FaceData& face, uint32_t codepoint, int size_index) {
    if (const UnicodeAdvance* entry = unicodeAdvance(face, codepoint))
        return entry->advance[size_index];
    // The native engine draws its replacement glyph for a codepoint outside
    // the captured set.  U+FFFD is captured per face/size so even that path
    // has a measured advance instead of a constant heuristic.
    if (const UnicodeAdvance* replacement = unicodeAdvance(face, 0xFFFD))
        return replacement->advance[size_index];
    return 0;
}

std::size_t decodeUtf8(const std::string& text, std::size_t pos,
                       uint32_t* codepoint) {
    const unsigned char first = static_cast<unsigned char>(text[pos]);
    if (first < 0x80) {
        *codepoint = first;
        return 1;
    }
    int length = 0;
    uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07;
    } else {
        *codepoint = 0xFFFD;
        return 1;
    }
    if (pos + std::size_t(length) > text.size()) {
        *codepoint = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < length; ++i) {
        const unsigned char byte =
            static_cast<unsigned char>(text[pos + std::size_t(i)]);
        if ((byte & 0xC0) != 0x80) {
            *codepoint = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (byte & 0x3F);
    }
    static constexpr uint32_t kMinimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < kMinimum[length] || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        *codepoint = 0xFFFD;
        return 1;
    }
    *codepoint = value;
    return std::size_t(length);
}

int spaceAdjustmentIndex(unsigned char c) {
    switch (c) {
    case 'A':
        return 0;
    case 'T':
        return 1;
    case 'Y':
        return 2;
    default:
        return -1;
    }
}

} // namespace

int normalizedFace(int font) {
    if (font >= 0 && font < kExactFaceCount)
        return font;
    return bold_face(font) ? kSourceSansBold : kSourceSans;
}

bool isExactFace(int font) {
    return font >= 0 && font < kExactFaceCount;
}

rbx::Vector2 measure(const std::string& text, double size, int font) {
    if (text.empty())
        return rbx::Vector2{0.0, 0.0};

    const FaceData& face = kFaces[normalizedFace(font)];
    // Native TextBounds clamps TextSize to 1..100 and floors fractional
    // values before shaping; 13.99 is exactly the 13 px metric and 1000 is
    // exactly the 100 px metric (font_metrics_extended_probe.lua).
    const int effective_size =
        std::clamp(int(std::floor(size)), kFirstSize, kLastSize);
    const int size_index = effective_size - kFirstSize;

    double width = 0.0;
    double max_width = 0.0;
    int lines = 1;
    bool has_previous = false;
    bool leading_whitespace = true;
    uint32_t previous = 0;

    for (std::size_t pos = 0; pos < text.size();) {
        uint32_t c = 0;
        pos += decodeUtf8(text, pos, &c);
        if (c == '\n') {
            max_width = std::max(max_width, width);
            width = 0.0;
            ++lines;
            has_previous = false;
            leading_whitespace = true;
            continue;
        }
        if (c == '\r')
            continue;
        const bool ascii = c >= uint32_t(kFirstChar) && c <= uint32_t(kLastChar);
        width += ascii ? face.advance[size_index][c - kFirstChar]
                       : unicodeWidth(face, c, size_index);
        // Native applies the captured GPOS adjustment for adjacent pairs.
        // Pairs containing a non-ASCII codepoint live in the Unicode pair
        // table; ASCII pairs keep their original compact table (including
        // the special space adjustments).
        if (has_previous && previous <= 0xFFFF && c <= 0xFFFF &&
            !leading_whitespace) {
            if (ascii && previous <= uint32_t(kLastChar)) {
                const unsigned char first = static_cast<unsigned char>(previous);
                const unsigned char second = static_cast<unsigned char>(c);
                const int space_index = first == ' '
                                            ? spaceAdjustmentIndex(second)
                                            : -1;
                width += space_index >= 0
                             ? face.space_adjustment[size_index][space_index]
                             : pairAdjustment(face, first, second, size_index);
            } else {
                width += unicodePairAdjustment(face, previous, c, size_index);
            }
        }
        if (c == uint32_t(' '))
            leading_whitespace = true;
        else if (!(c >= 0x0300 && c <= 0x036F))
            leading_whitespace = false;
        previous = c;
        has_previous = true;
    }
    max_width = std::max(max_width, width);

    return rbx::Vector2{std::round(max_width),
                        double(lines) * lineHeight(size, font)};
}

double lineHeight(double size, int font) {
    const FaceData& face = kFaces[normalizedFace(font)];
    const int effective_size =
        std::clamp(int(std::floor(size)), kFirstSize, kLastSize);
    return double(std::max(
        1, int(std::floor(double(effective_size) *
                          double(face.line_numerator) /
                          double(face.line_denominator)))));
}

double pixelScale(int font) {
    return kFaces[normalizedFace(font)].pixel_scale;
}

std::size_t codepointLength(const std::string& text, std::size_t pos) {
    uint32_t codepoint = 0;
    return decodeUtf8(text, pos, &codepoint);
}

std::size_t codepointOffset(const std::string& text, std::size_t count) {
    std::size_t pos = 0;
    uint32_t codepoint = 0;
    while (pos < text.size() && count > 0) {
        pos += decodeUtf8(text, pos, &codepoint);
        --count;
    }
    return pos;
}

} // namespace rbxclient::font_metrics
