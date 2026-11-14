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

struct FaceData {
    const uint16_t (*advance)[kCharCount];
    const KernPair* pairs;
    int pair_count;
    const int8_t (*space_adjustment)[3];
    int line_numerator;
    int line_denominator;
    double pixel_scale;
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
    const double effective_size = std::max(1.0, size);
    int size_index = int(std::lround(effective_size)) - kFirstSize;
    size_index = std::clamp(size_index, 0, kSizeCount - 1);
    // TextSize is an integer; TextScaled's fitted size and UIScale can be
    // fractional.  Native's fitted glyphs land on the nearest captured size
    // (e.g. 100x20 "Hello" fits at 13.33 and reports the 13 px width), while
    // the reported height still uses the fractional size.
    double scale = 1.0;
    if (effective_size > double(kLastSize))
        scale = effective_size / double(kLastSize);

    double width = 0.0;
    double max_width = 0.0;
    int lines = 1;
    bool has_previous = false;
    bool leading_whitespace = true;
    unsigned char previous = 0;

    for (unsigned char c : text) {
        if (c == '\n') {
            max_width = std::max(max_width, width);
            width = 0.0;
            ++lines;
            has_previous = false;
            leading_whitespace = true;
            continue;
        }
        if (c >= kFirstChar && c <= kLastChar) {
            const int index = c - kFirstChar;
            width += face.advance[size_index][index] * scale;
            if (has_previous && !leading_whitespace) {
                const int space_index = previous == ' '
                                            ? spaceAdjustmentIndex(c)
                                            : -1;
                const int adjustment =
                    space_index >= 0
                        ? face.space_adjustment[size_index][space_index]
                        : pairAdjustment(face, previous, c, size_index);
                width += adjustment * scale;
            }
            previous = c;
            has_previous = true;
            if (c != ' ')
                leading_whitespace = false;
        } else {
            // The native Legacy fonts also cover non-ASCII text through
            // fallback faces.  Keep those readable without pretending the
            // captured Latin tables describe them.
            width += 0.56 * effective_size;
            has_previous = false;
            leading_whitespace = false;
        }
    }
    max_width = std::max(max_width, width);

    const int line_height =
        std::max(1, int(std::floor(effective_size *
                                   double(face.line_numerator) /
                                   double(face.line_denominator))));
    return rbx::Vector2{std::round(max_width),
                        double(lines * line_height)};
}

double lineHeight(double size, int font) {
    const FaceData& face = kFaces[normalizedFace(font)];
    const double effective_size = std::max(1.0, size);
    return double(std::max(
        1, int(std::floor(effective_size * double(face.line_numerator) /
                          double(face.line_denominator)))));
}

double pixelScale(int font) {
    return kFaces[normalizedFace(font)].pixel_scale;
}

} // namespace rbxclient::font_metrics
