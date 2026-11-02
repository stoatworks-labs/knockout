#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace knockout
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double linear( float v, double low, double high )
{
	return low + ( high - low ) * clamp01( v );
}
float inverseLinear( double value, double low, double high )
{
	return static_cast< float >( std::clamp( ( value - low ) / ( high - low ), 0.0, 1.0 ) );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
constexpr double kSizeLow = 0.02, kSizeHigh = 2.0;
constexpr double kMarginMax = 0.45;
constexpr double kTrackingLow = -0.2, kTrackingHigh = 0.6;
constexpr double kSpacingLow = 0.5, kSpacingHigh = 2.0;
constexpr double kGrowMax = 0.1;
constexpr double kFeatherMax = 0.1;
constexpr double kOutlineMax = 0.2;
} // namespace

const char* GroupOf( unsigned int id )
{
	if( id <= PT_ITALIC )
		return "Text";
	if( id <= PT_LINE_SPACING )
		return "Layout";
	if( id <= PT_ZOOM_Y )
		return "Transform";
	if( id <= PT_OUTLINE )
		return "Edge";
	if( id <= PT_REVEAL_ORDER )
		return "Reveal";
	if( id <= PT_MIX )
		return "Mask";
	return "About";
}

const char* NameOf( unsigned int id )
{
	switch( id )
	{
	case PT_TEXT: return "Text";
	case PT_FONT: return "Font";
	case PT_FONT_NAME: return "Font Name";
	case PT_FONT_FILE: return "Font File";
	case PT_WEIGHT: return "Weight";
	case PT_ITALIC: return "Italic";
	case PT_FIT: return "Fit";
	case PT_SIZE: return "Size";
	case PT_MARGIN: return "Margin";
	case PT_ALIGN: return "Align";
	case PT_TRACKING: return "Tracking";
	case PT_LINE_SPACING: return "Line Spacing";
	case PT_POSITION_X: return "Position X";
	case PT_POSITION_Y: return "Position Y";
	case PT_ROTATION: return "Rotation";
	case PT_ZOOM: return "Zoom";
	case PT_ZOOM_X: return "Zoom X";
	case PT_ZOOM_Y: return "Zoom Y";
	case PT_GROW: return "Grow";
	case PT_FEATHER: return "Feather";
	case PT_OUTLINE: return "Outline";
	case PT_REVEAL: return "Reveal";
	case PT_REVEAL_SPREAD: return "Reveal Spread";
	case PT_REVEAL_ORDER: return "Reveal Order";
	case PT_MASK: return "Mask";
	case PT_BACKGROUND: return "Background";
	//Resolume draws a RED, GREEN, BLUE run as one swatch named after the
	//first; the other two names follow the SDK's own convention (asciify).
	case PT_COLOUR_R: return "Colour";
	case PT_COLOUR_G: return "Colour_Green";
	case PT_COLOUR_B: return "Colour_Blue";
	case PT_MIX: return "Mix";
	default: return "?";
	}
}

bool IsTextParam( unsigned int id )
{
	return id == PT_TEXT || id == PT_FONT_NAME || id == PT_FONT_FILE;
}

const char* FitName( Fit fit )
{
	switch( fit )
	{
	case Fit::Frame: return "Frame";
	case Fit::Width: return "Width";
	case Fit::Height: return "Height";
	case Fit::Size: return "Size";
	default: return "?";
	}
}

const char* AlignName( int align )
{
	static const char* const names[] = { "Left", "Centre", "Right" };
	return align >= 0 && align < 3 ? names[ align ] : "?";
}

const char* WeightName( int index )
{
	static const char* const names[ kWeightCount ] = { "Thin", "Extra Light", "Light", "Regular", "Medium",
		                                               "Semibold", "Bold", "Extra Bold", "Black" };
	return index >= 0 && index < kWeightCount ? names[ index ] : "?";
}

int WeightOf( int index )
{
	return 100 * ( std::clamp( index, 0, kWeightCount - 1 ) + 1 );
}

const char* OrderName( Order order )
{
	switch( order )
	{
	case Order::Forward: return "Forward";
	case Order::Backward: return "Backward";
	case Order::CentreOut: return "Centre Out";
	case Order::Random: return "Random";
	default: return "?";
	}
}

const char* MaskName( Mask mask )
{
	switch( mask )
	{
	case Mask::Inside: return "Inside";
	case Mask::Outside: return "Outside";
	default: return "?";
	}
}

const char* BackgroundName( Background background )
{
	switch( background )
	{
	case Background::Transparent: return "Transparent";
	case Background::Black: return "Black";
	case Background::White: return "White";
	case Background::Colour: return "Colour";
	default: return "?";
	}
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double SizeFromParam( float v )
{
	return geometric( v, kSizeLow, kSizeHigh );
}
float ParamFromSize( double frameHeights )
{
	return inverseGeometric( frameHeights, kSizeLow, kSizeHigh );
}
double MarginFromParam( float v )
{
	return linear( v, 0.0, kMarginMax );
}
float ParamFromMargin( double fraction )
{
	return inverseLinear( fraction, 0.0, kMarginMax );
}
double TrackingFromParam( float v )
{
	return linear( v, kTrackingLow, kTrackingHigh );
}
float ParamFromTracking( double em )
{
	return inverseLinear( em, kTrackingLow, kTrackingHigh );
}
double LineSpacingFromParam( float v )
{
	return linear( v, kSpacingLow, kSpacingHigh );
}
float ParamFromLineSpacing( double lines )
{
	return inverseLinear( lines, kSpacingLow, kSpacingHigh );
}

double PositionFromParam( float v )
{
	return linear( v, -1.0, 1.0 );
}
float ParamFromPosition( double frames )
{
	return inverseLinear( frames, -1.0, 1.0 );
}
double RotationFromParam( float v )
{
	return linear( v, -180.0, 180.0 );
}
float ParamFromRotation( double degrees )
{
	return inverseLinear( degrees, -180.0, 180.0 );
}
double ZoomFromParam( float v )
{
	return geometric( v, 1.0, kMaxZoom );
}
float ParamFromZoom( double zoom )
{
	return inverseGeometric( zoom, 1.0, kMaxZoom );
}

double GrowFromParam( float v )
{
	return linear( v, -kGrowMax, kGrowMax );
}
float ParamFromGrow( double em )
{
	return inverseLinear( em, -kGrowMax, kGrowMax );
}
double FeatherFromParam( float v )
{
	const double x = clamp01( v );
	return kFeatherMax * x * x;
}
float ParamFromFeather( double frameHeights )
{
	return static_cast< float >( std::sqrt( std::clamp( frameHeights / kFeatherMax, 0.0, 1.0 ) ) );
}
double OutlineFromParam( float v )
{
	return linear( v, 0.0, kOutlineMax );
}
float ParamFromOutline( double em )
{
	return inverseLinear( em, 0.0, kOutlineMax );
}

} // namespace knockout
