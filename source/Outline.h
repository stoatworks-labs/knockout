#pragma once

#include "Fonts.h"

#include <vector>

/**
    A glyph's outline, in the form the shader reads it.

    ## Why outlines and not a texture

    The mask is cut from the font's own curves, per pixel, every frame: no
    bitmap, no distance-field atlas. A letter is seen at every size here --
    a word across the frame, then Zoom diving a thousand times into one
    stroke -- and anything sampled at a fixed resolution goes soft or
    faceted somewhere in that range. The curves do not.

    Per pixel the shader needs two things, both from the same list of
    pieces: whether the pixel is inside (the winding number of a ray to +x,
    TrueType's and CFF's non-zero rule) and how far it is from the edge (the
    distance to the nearest piece of outline). The first decides the sign,
    the second the antialiasing, the feather, the outline stroke and Grow.

    ## What BuildOutline does to make that robust

    - **Every piece is a quadratic Bezier, monotonic in y.** TrueType's
      curves already are quadratics; CFF's cubics are approximated by
      quadratics to within `kCubicTolerance` of an em, measured, not
      estimated. Each quadratic is split at its y-extremum (the split's
      control points snapped to the extremum's y, so each half is monotonic
      in floats too). A ray then crosses a piece at most once, and the
      half-open rule -- a piece counts for y in [low, high) -- counts a
      crossing through a shared vertex once and a tangent touch zero times.
    - **Lines are flagged** (the control point at the chord's midpoint) and
      measured as segments: a nearly straight quadratic is the one shape the
      closed-form distance gets wrong.
    - **Overlaps are resolved for the distance, not for the fill.** A
      variable font's default instance draws an A as two overlapping
      strokes and a crossbar. The fill is right regardless (non-zero), but
      the distance to "the nearest piece" would be the distance to an edge
      that is buried inside the letter, and the antialiasing would draw it:
      a one-pixel seam through every overlap. So the pieces are split where
      they cross one another, and each piece is classified by the winding
      just either side of its midpoint. A piece with ink on both sides is
      internal: it still counts for the winding, but not for the distance.
*/
namespace knockout
{
struct Vec2
{
	double x = 0.0, y = 0.0;
};

struct Piece
{
	Vec2 p0, p1, p2;
	bool line     = false;///< straight: p1 is the midpoint of p0 and p2
	bool boundary = true; ///< on the edge of the filled region
};

struct GlyphOutline
{
	std::vector< Piece > pieces;
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;///< ink bounds, font units
	int crossings = 0;///< where overlap removal split pieces
	int internal  = 0;///< pieces it took out of the distance
	bool HasInk() const
	{
		return !pieces.empty();
	}
};

/// A cubic is close enough when no point of it is further than this from
/// the quadratics standing in for it, in ems: 2e-5 em is under a pixel
/// until an em spans 50 000 pixels.
constexpr double kCubicTolerance = 2.0e-5;

struct OutlineOptions
{
	/// The negative control: off, internal pieces stay in the distance, and
	/// an overlap shows as a seam.
	bool resolveOverlaps = true;
};

GlyphOutline BuildOutline( const std::vector< Font::Vertex >& shape, int unitsPerEm, const OutlineOptions& options = {} );

/// The CPU twins of the shader's loops, in double: the winding number at q
/// (a ray to +x, half-open in y) and the distance from q to the nearest
/// boundary piece. The harness measures the picture against these and
/// checks these against brute force.
int WindingAt( const std::vector< Piece >& pieces, Vec2 q, bool evenOdd = false );
double BoundaryDistance( const std::vector< Piece >& pieces, Vec2 q );

Vec2 Evaluate( const Piece& piece, double t );
/// Exact: the roots of the distance's derivative on [0, 1], in double.
double DistanceToPiece( const Piece& piece, Vec2 q );
/// Quadratics standing in for a cubic, each within `tolerance` of it.
std::vector< Piece > CubicToQuads( Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, double tolerance );
/// The worst distance from a cubic's points to a run of pieces, sampled
/// densely: what CubicToQuads promises, measured independently.
double CubicDeviation( Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, const std::vector< Piece >& pieces );

} // namespace knockout
