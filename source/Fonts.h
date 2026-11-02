#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
    Fonts: finding the installed faces, and opening one.

    The scan is polyhedral's (which was downpour's): walk the OS font
    directories, read only each face's `name` table, never the whole file.
    What is new is that it keeps every FACE, not one per family, with the two
    things a face is chosen by -- its weight class and whether it is italic --
    read from `OS/2` (and `head`'s macStyle where a face has no OS/2).

    A mask wants weight. The difference between Arial and Arial Black is the
    difference between a mask you can see the clip through and one you
    cannot, so the plugin chooses a face the way CSS does (Font, Weight,
    Italic -> the nearest face the family has) rather than listing nine
    hundred faces in one dropdown.

    Faces whose outlines this plugin cannot read are left out: bitmap-only
    faces, and faces whose glyphs are pictures (`sbix`, `CBDT`: Apple Color
    Emoji's `glyf` is placeholder boxes), and CFF2, the variable CFF that
    stb_truetype does not parse.
*/
namespace knockout
{
struct FontFace
{
	std::string family;///< typographic family (name ID 16), else legacy (1)
	std::string style; ///< typographic subfamily (17), else legacy (2)
	std::string path;
	int collectionIndex = 0;
	int weight          = 400;///< OS/2 usWeightClass, 1..1000
	bool italic         = false;
};

struct FontFamily
{
	std::string name;
	std::vector< int > faces;///< indices into InstalledFaces()
};

/// Every face in the OS font directories, sorted by family, then weight,
/// then upright before italic. Scanned once per process.
const std::vector< FontFace >& InstalledFaces();
/// The families those faces make, sorted case-insensitively.
const std::vector< FontFamily >& InstalledFamilies();
/// Index of the family whose name matches (case-insensitive), or -1.
int FindFamily( const std::string& name );
/// The face of `family` nearest `weight` and `italic`, by CSS Fonts 4's
/// matching rules (5.2, "font-weight"); -1 for no family.
int MatchFace( int family, int weight, bool italic );
/// The family a fresh instance starts on: Arial Black where it exists (on
/// every Mac; not on GitHub's windows-2022 runner, which has Arial), then
/// Arial, Helvetica, then the first family. -1 when there are no fonts.
int DefaultFamily();

/// What the Font list gives `family` as its element value, which is what a
/// host saves. A position in this machine's list means another family on the
/// next, so the value comes from the name: the same family gets the same
/// value whatever else is installed. 0 for no family.
float FamilyValue( int family );
/// The installed family whose FamilyValue is `value`, or -1: a composition
/// saved with a family this machine does not have.
int FamilyOfValue( float value );
/// FamilyValue's arithmetic for any list of names in list order, exposed for
/// the harness. FNV-1a over the lowered name folded to 24 bits, so a float
/// holds it exactly; a name whose value is taken moves up by one, so only two
/// colliding names depend on order (335 families, as on the Mac this was
/// built on, give a pair about one machine in 300; that Mac has none).
std::vector< uint32_t > FamilyValues( const std::vector< std::string >& names );

/// CSS's nearest-weight order, for the harness: which of `available` CSS
/// would pick for `wanted`. Exposed so it can be checked against the
/// specification's own examples.
int NearestWeight( int wanted, const std::vector< int >& available );

/// Read one face's names and classes from a file, for Font File: what the
/// scan would have recorded for it. False if it is not a face we can draw.
bool DescribeFace( const std::string& path, int collectionIndex, FontFace& into );

/**
    An open face: the file's bytes and stb_truetype's view of them, plus the
    numbers the layout needs. Kept behind a pointer so stb_truetype.h stays
    in one translation unit.
*/
class Font
{
public:
	Font();
	~Font();
	Font( const Font& )            = delete;
	Font& operator=( const Font& ) = delete;

	bool Load( const std::string& path, int collectionIndex );
	bool IsLoaded() const
	{
		return loaded;
	}
	const FontFace& Face() const
	{
		return face;
	}

	int UnitsPerEm() const
	{
		return unitsPerEm;
	}
	/// hhea's, in font units: ascent positive, descent negative.
	int Ascent() const
	{
		return ascent;
	}
	int Descent() const
	{
		return descent;
	}
	int LineGap() const
	{
		return lineGap;
	}

	/// 0 (.notdef) for a codepoint the face does not have.
	int GlyphFor( uint32_t codepoint ) const;
	int Advance( int glyph ) const;
	/// The kerning between two glyphs, `kern` table or GPOS pair adjustment,
	/// in font units (negative pulls them together).
	int Kern( int left, int right ) const;

	/// A glyph's outline as stb_truetype gives it, in font units.
	struct Vertex
	{
		enum Type : uint8_t
		{
			Move = 1,
			Line,
			Quad,
			Cubic
		};
		Type type;
		int x, y;  ///< the end point
		int cx, cy;///< the control point (Quad), the first one (Cubic)
		int dx, dy;///< the second control point (Cubic)
	};
	std::vector< Vertex > Shape( int glyph ) const;

private:
	struct Impl;
	std::unique_ptr< Impl > impl;
	FontFace face;
	bool loaded    = false;
	int unitsPerEm = 1000;
	int ascent = 800, descent = -200, lineGap = 0;
};

} // namespace knockout
