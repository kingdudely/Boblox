// gui_font_metrics.h — native Legacy text metrics for the GUI.
//
// The native client shapes the five Legacy faces with FreeType hinted
// advances and HarfBuzz GPOS kerning at fractional pixel sizes.  The exact
// hinted results depend on the FreeType version in the client, so the public
// client's quantized results are recorded as tables under
// tests/live/output/font_metrics_native*.txt and compiled in here.  Every GUI
// consumer (TextBounds, wrapping, TextFits, AutomaticSize, input caret and the
// renderer's caret geometry) reads through this one source.
#pragma once

#include "instance/variant.h"

#include <string>

namespace rbxclient::font_metrics {

// Enum.Font values with native Legacy assets.  Other enum entries fall back
// to the regular or bold SourceSans face; see normalizedFace().
constexpr int kLegacy = 0;
constexpr int kArial = 1;
constexpr int kArialBold = 2;
constexpr int kSourceSans = 3;
constexpr int kSourceSansBold = 4;
constexpr int kExactFaceCount = 5;

// TextSize range covered by the authoritative tables.
constexpr int kFirstSize = 1;
constexpr int kLastSize = 100;

// Map an Enum.Font value to the exact table that should measure it.  Faces
// 0..4 have native assets; the remaining Legacy enum entries use the closest
// available SourceSans weight, mirroring the renderer's fallback.
int normalizedFace(int font);

bool isExactFace(int font);

// Width/height of `text` at `size` in screen pixels.  Integer sizes in
// 1..100 reproduce native TextBounds exactly; fractional sizes (UIScale) and
// sizes outside the table range scale the nearest captured size and are
// documented as approximations.
rbx::Vector2 measure(const std::string& text, double size, int font);

// Native line height for one line: Legacy is floor(1.5 * size); the Arial and
// SourceSans faces are floor(size).
double lineHeight(double size, int font);

// Native pixel em size as a multiple of TextSize.  The renderer uses this so
// drawn glyphs and layout advances describe the same font.
double pixelScale(int font);

} // namespace rbxclient::font_metrics
