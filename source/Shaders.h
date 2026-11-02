#pragma once

#include <initializer_list>
#include <string>

/**
    The GLSL, as text.

    `kCommon` is a LIBRARY, not a shader: no #version, no main. Every pass is
    assembled as kVersion + kCommon + its pieces, so the harness's checks run
    the text the plugin runs. No piece may pass 15 000 bytes (MSVC's C2026
    limit on one string literal).

    Two passes:

      glyph      one instanced quad per glyph, covering its ink plus the
                 reach of the edge, drawn into the mask (one float channel)
                 with MAX blending. Per pixel: the winding number of the
                 glyph's pieces (a ray to +x), the distance to the nearest
                 boundary piece, and from the two the mask -- fill or
                 outline stroke, antialiased over one pixel, or feathered.
      composite  the clip and the mask: Inside keeps the clip where the
                 mask is, Outside where it is not, and what is taken away
                 becomes the Background. Mix brings the clip back.

    The mask is not colour, so nothing here converts between sRGB and
    linear: the clip is scaled by a coverage, which is what an alpha is.
*/
namespace knockout::shaders
{
extern const char* const kVersion;
extern const char* const kCommon;

extern const char* const kGlyphVertex;
extern const char* const kGlyphFragment;
extern const char* const kQuadVertex;
extern const char* const kCompositeFragment;

/// kVersion + kCommon + the pieces, in order.
std::string Assemble( std::initializer_list< const char* > pieces );

/// The glyph shader's negative controls, for the harness (TestFlags).
enum TestFlag : int
{
	kEvenOdd       = 1,///< inside by the parity of the crossings, not their sum
	kAllPieces     = 2,///< distance to internal pieces too: overlaps seam
};

} // namespace knockout::shaders
