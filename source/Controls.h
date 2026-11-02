#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean in the mask's units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control. Option and boolean parameters hold the element value itself.

    Units: an EM is the face's em square (the size the font is designed at);
    sizes, tracking, Grow and Outline are in ems, so they scale with the
    type and hold through a zoom. Feather is in frame heights, because a
    soft edge is something seen on the screen, and should stay the softness
    asked for while the letters grow past it.
*/
namespace knockout
{
/// Host index == id: one plugin, one dense list, the About block last.
enum ParamId : unsigned int
{
	// -- Text ----------------------------------------------------------------
	PT_TEXT = 0,
	PT_FONT,
	PT_FONT_NAME,
	PT_FONT_FILE,
	PT_WEIGHT,
	PT_ITALIC,

	// -- Layout --------------------------------------------------------------
	PT_FIT,
	PT_SIZE,
	PT_MARGIN,
	PT_ALIGN,
	PT_TRACKING,
	PT_LINE_SPACING,

	// -- Transform -----------------------------------------------------------
	PT_POSITION_X,
	PT_POSITION_Y,
	PT_ROTATION,
	PT_ZOOM,
	PT_ZOOM_X,
	PT_ZOOM_Y,

	// -- Edge ----------------------------------------------------------------
	PT_GROW,
	PT_FEATHER,
	PT_OUTLINE,

	// -- Reveal --------------------------------------------------------------
	PT_REVEAL,
	PT_REVEAL_SPREAD,
	PT_REVEAL_ORDER,

	// -- Mask ----------------------------------------------------------------
	PT_MASK,
	PT_BACKGROUND,
	PT_COLOUR_R,
	PT_COLOUR_G,
	PT_COLOUR_B,
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_TEXT,
	PT_COUNT = PT_ABOUT_TEXT + 1 + stoatworks::about::kButtonCount
};

const char* GroupOf( unsigned int id );
const char* NameOf( unsigned int id );
bool IsTextParam( unsigned int id );

enum class Fit
{
	Frame = 0,///< the ink as large as fits inside the frame, less the margin
	Width,    ///< the ink across the frame's width, less the margin
	Height,   ///< the ink up the frame's height, less the margin
	Size,     ///< no fitting: Size sets the em
	Count
};
const char* FitName( Fit fit );

const char* AlignName( int align );

/// Weight: the nine CSS weights, 100 (Thin) to 900 (Black).
constexpr int kWeightCount = 9;
const char* WeightName( int index );
int WeightOf( int index );

enum class Order
{
	Forward = 0,///< reading order
	Backward,
	CentreOut,///< from the middle glyph outwards, both ways at once
	Random,   ///< a fixed shuffle of the glyphs (the same every time)
	Count
};
const char* OrderName( Order order );

enum class Mask
{
	Inside = 0,///< the clip shows through the letters
	Outside,   ///< the letters are cut out of the clip
	Count
};
const char* MaskName( Mask mask );

enum class Background
{
	Transparent = 0,///< what the mask takes away is gone: lower layers show
	Black,
	White,
	Colour,
	Count
};
const char* BackgroundName( Background background );

int OptionIndex( float value, int count );

//---------------------------------------------------------------------------
// The mappings.
//---------------------------------------------------------------------------

/// Size (Fit Size): the em, 0.02 to 2 frame heights, geometrically.
double SizeFromParam( float v );
float ParamFromSize( double frameHeights );
/// Margin: 0 to 0.45 of the frame on each side.
double MarginFromParam( float v );
float ParamFromMargin( double fraction );
/// Tracking: -0.2 to +0.6 em after each glyph, 0 a quarter of the way.
double TrackingFromParam( float v );
float ParamFromTracking( double em );
/// Line Spacing: 0.5 to 2 line heights.
double LineSpacingFromParam( float v );
float ParamFromLineSpacing( double lines );

/// Position: -1 to +1 frame widths (X) or heights (Y), 0 in the middle.
double PositionFromParam( float v );
float ParamFromPosition( double frames );
/// Rotation: -180 to +180 degrees.
double RotationFromParam( float v );
float ParamFromRotation( double degrees );
/// Zoom: 1 to 1000 times, geometrically.
constexpr double kMaxZoom = 1000.0;
double ZoomFromParam( float v );
float ParamFromZoom( double zoom );

/// Grow: -0.1 to +0.1 em, the outline pushed out (or in) along its normal.
double GrowFromParam( float v );
float ParamFromGrow( double em );
/// Feather: 0 to 0.1 frame heights of soft edge, squared so the fine end
/// has the slider.
double FeatherFromParam( float v );
float ParamFromFeather( double frameHeights );
/// Outline: 0 (filled letters) to 0.2 em of stroke along the outline.
double OutlineFromParam( float v );
float ParamFromOutline( double em );

} // namespace knockout
