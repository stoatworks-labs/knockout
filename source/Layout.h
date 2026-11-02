#pragma once

#include "Fonts.h"
#include "Outline.h"

#include <map>
#include <string>
#include <vector>

/**
    Setting the text: which glyphs, and where their origins go.

    Plain horizontal setting, left to right: the face's advances, its
    kerning (the `kern` table or GPOS pair adjustments, as stb_truetype
    reads them), Tracking added after every glyph but a line's last, lines
    a Line Spacing multiple of hhea's ascent - descent + lineGap apart.
    Lines are aligned by their advance widths, the typographer's measure;
    the block as a whole is centred and fitted by its INK, the measure a
    mask is seen by.

    What it does not do, and says so in the README: shaping. No ligatures,
    no contextual forms, no right-to-left, no combining marks placed by
    GPOS -- one codepoint, one glyph. A Latin, Greek or Cyrillic word set
    in a display face is what a mask is for, and that is exact; Arabic or
    Devanagari would need HarfBuzz.

    Units are the face's: font units, y up, the first line's baseline at 0.
*/
namespace knockout
{
/// The Text field's string as codepoints: UTF-8 decoded (a malformed byte
/// is U+FFFD), "\n" typed as two characters is a line break (Resolume's
/// text field is one line), "\\" a backslash, CR LF and CR are LF, and
/// other control characters are dropped. A tab is a space.
std::u32string DecodeText( const std::string& utf8 );

enum class Align
{
	Left = 0,
	Centre,
	Right,
	Count
};

struct SetOptions
{
	double trackingEm  = 0.0;///< added after each glyph, ems
	double lineSpacing = 1.0;///< multiples of the face's line height
	Align align        = Align::Centre;
	bool kerning       = true;///< off only for the negative control
};

struct PlacedGlyph
{
	int glyph = 0;           ///< into the Typesetter's outlines
	double x = 0.0, y = 0.0; ///< the glyph's origin, font units
	int order = 0;           ///< reading order among glyphs with ink
	int line  = 0;
};

struct Setting
{
	std::vector< PlacedGlyph > glyphs;///< inked glyphs only; spaces advance but are not drawn
	int lines = 0;
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;///< the ink's bounds
	bool HasInk() const
	{
		return !glyphs.empty();
	}
};

class Typesetter
{
public:
	/// Forget every outline; the next Set builds them from this face.
	void UseFont( const Font* font, const OutlineOptions& options = {} );
	Setting Set( const std::u32string& text, const SetOptions& options );

	/// Every outline built so far, in the order Set numbered them. Set only
	/// appends, so a table uploaded once stays valid until UseFont.
	const std::vector< GlyphOutline >& Outlines() const
	{
		return outlines;
	}
	const Font* CurrentFont() const
	{
		return font;
	}

private:
	int outlineFor( int glyphIndex );

	const Font* font = nullptr;
	OutlineOptions outlineOptions;
	std::map< int, int > byGlyph;
	std::vector< GlyphOutline > outlines;
};

} // namespace knockout
