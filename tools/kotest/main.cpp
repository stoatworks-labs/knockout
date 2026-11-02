/**
    kotest -- render Knockout offline, and measure what it drew.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, in a headless CGL context, and reads back the mask (the plugin's
    own R16F target, before the composite) and the output.

        kotest --out /tmp/k.png           the defaults over the harness's card
        kotest --list                     every parameter and its default
        kotest --set "Text=HELLO"         a control by name (an option by its
                                          element's name), repeatable
        kotest --at 30 "Reveal=0.5"       a setting from frame 30 on
        kotest --frames N                 render N frames before --out (1)
        kotest --size 1280x720
        kotest --pipe                     raw RGBA frames in, raw frames out
        kotest --script cues              the fleet's cue format
        kotest --offline                  the checks that need no GL context
        kotest --coverage --zoom ...      one or more GL checks; --all for all
        kotest --negative                 the wrong models, each of which a
                                          check must catch
        kotest --system-fonts --real      the checks against this Mac's fonts
        kotest --system-fonts --bench     milliseconds a frame

    ## Where the truth comes from

    The fonts are the harness's own (tools/make-test-font.py): squares, a
    ring, a plus made of two crossing bars, triangles, a disc. Their outlines
    are written down twice -- once in the font, once here as polygons -- and
    the checks compare the plugin's mask with the mask those polygons predict,
    pixel for pixel: the exact signed distance to the polygon, through the
    one-pixel ramp, after the transform the parameters ask for. Nothing in
    that prediction reads the plugin's FrameState or its outlines.

    Where a polygon cannot say it (the disc's curves, real fonts) the mask is
    compared with stb_truetype's own rasteriser, compiled into this file: an
    exact-area scanline fill that shares nothing with the shader but the
    glyph's points.

    By default the plugin's font scan is pointed at tools/fixtures
    (KNOCKOUT_FONT_DIR), so "the Font list" is four faces in two families on
    every machine. --system-fonts leaves the scan on the OS folders.

    `--script` lines are `frame  Parameter Name  value`, held before the first
    key and after the last. A STANDARD (0..1) control is linear between its
    keys; an option, a boolean or an event STEPS.
*/

#include "Controls.h"
#include "Fonts.h"
#include "Knockout.h"
#include "Layout.h"
#include "Outline.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

// stb_truetype's rasteriser, a copy of its own in this file: the reference
// the shader is compared with for curves and real fonts.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

using namespace knockout;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;
bool g_quiet   = false;
bool g_systemFonts = false;
/// The harness's own wrong model, for --negative: its prediction of the
/// rotation turned the other way.
bool g_wrongRotation = false;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	if( !g_quiet || !condition )
		std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

void Section( const char* name )
{
	if( !g_quiet )
		std::printf( "\n%s\n", name );
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written top
/// row first, premultiplied colour over black.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "KOTEST_RENDERER" );
	const bool software      = renderer != nullptr && std::strcmp( renderer, "software" ) == 0;
	if( CGLChoosePixelFormat( software ? generic : accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// White and opaque: masked, the output's alpha IS the mask.
Floats whiteCard( int width, int height )
{
	return Floats( static_cast< size_t >( width ) * height * 4, 1.0f );
}

/// For the eye and the composite: a graded, striped clip, premultiplied, with
/// a band of half alpha across the middle. Rows bottom first.
Floats colourCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			const double stripe = 0.5 + 0.5 * std::sin( ( u * 9.0 + v * 4.0 ) * kPi );
			const double a      = std::fabs( v - 0.5 ) < 0.08 ? 0.5 : 1.0;
			float* px           = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			px[ 0 ]             = static_cast< float >( a * ( 0.15 + 0.85 * u ) );
			px[ 1 ]             = static_cast< float >( a * ( 0.2 + 0.6 * stripe ) );
			px[ 2 ]             = static_cast< float >( a * ( 1.0 - 0.8 * u ) );
			px[ 3 ]             = static_cast< float >( a );
		}
	return card;
}

//---------------------------------------------------------------------------
// A rig: the real plugin and a float output framebuffer.
//---------------------------------------------------------------------------
struct Rig
{
	KnockoutPlugin plugin;
	int width = 0, height = 0;
	GLuint clipTexture = 0, outputTexture = 0, outputFBO = 0, readFBO = 0;
	int frame = 0;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
		if( readFBO )
			glDeleteFramebuffers( 1, &readFBO );
	}
	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		for( GLuint* t : { &outputTexture, &clipTexture } )
			if( *t )
				glDeleteTextures( 1, t );
		outputFBO = outputTexture = clipTexture = 0;
	}
	bool floatClip = false;///< the clip as 32-bit float, for checks that need it exact

	bool attach( int w, int h, const Floats* clip )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr, GL_RGBA32F );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = clip ? *clip : whiteCard( width, height );
		//8 bit, as Resolume's clips are, unless a check needs it exact.
		clipTexture       = makeTexture( width, height, card.data(), floatClip ? GL_RGBA32F : GL_RGBA8 );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = clipTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}
	bool Init( int w, int h, const Floats* clip = nullptr, int flags = 0 )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/knockout for which shader\n" );
			return false;
		}
		plugin.SetTestFlagsForTest( flags );
		//The fixtures' default weight would be Black, which finds their Bold:
		//the checks draw the Regular unless they ask (--fonts asks).
		if( !g_systemFonts )
			plugin.SetFloatParameter( PT_WEIGHT, 3.0f );
		return attach( w, h, clip );
	}
	bool Resize( int w, int h, const Floats* clip = nullptr )
	{
		release();
		return attach( w, h, clip );
	}
	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}
	void Text( unsigned int id, const std::string& value )
	{
		plugin.SetTextParameter( id, value.c_str() );
	}
	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}
	Floats Read( GLuint texture )
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		if( !readFBO )
			glGenFramebuffers( 1, &readFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, readFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return pixels;
	}
	Floats Output()
	{
		return Read( outputTexture );
	}
	/// The mask, one float a pixel, row 0 at the bottom.
	Floats Mask()
	{
		const Floats rgba = Read( plugin.MaskTextureID() );
		Floats mask( rgba.size() / 4 );
		for( size_t i = 0; i < mask.size(); ++i )
			mask[ i ] = rgba[ i * 4 ];
		return mask;
	}
};

float at( const Floats& mask, int width, int x, int y )
{
	return mask[ static_cast< size_t >( y ) * width + x ];
}

//---------------------------------------------------------------------------
// Parameters by name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( KnockoutPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

bool isTextType( unsigned int type )
{
	return type == FF_TYPE_TEXT || type == FF_TYPE_FILE;
}

/// `Name=Value`. A text or file parameter takes the value as it is; an
/// option its element's name or its index; anything else must be wholly a
/// number (polyhedral's trap: strtof( "Fixed" ) is 0, silently).
bool applySetting( KnockoutPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( isTextType( parameter.type ) )
				return plugin.SetTextParameter( parameter.index, value.c_str() ) == FF_SUCCESS;
			if( parameter.type == FF_TYPE_OPTION )
			{
				const unsigned int count = plugin.GetNumParamElements( parameter.index );
				for( unsigned int e = 0; e < count; ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == nullptr || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

bool applyAll( KnockoutPlugin& plugin, const std::vector< std::string >& settings )
{
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return false;
		}
	}
	return true;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == nullptr || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( KnockoutPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first && !isTextType( parameter.type ) )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a numeric parameter (try --list)";
			return false;
		}
	}
	return true;
}

struct Timed
{
	int frame;
	std::string setting;
};

//===========================================================================
// The harness's own model of the fixture fonts: make-test-font.py's glyphs
// as polygons, set the way the README says text is set, placed the way the
// README says the controls place it.
//===========================================================================
using Polygon = std::vector< Vec2 >;

/// One quadratic: on-curve, control, on-curve.
struct Quad
{
	Vec2 p0, c, p2;
};

struct SpecGlyph
{
	int advance = 800;
	std::vector< Polygon > polygons;///< straight letters
	std::vector< Quad > quads;      ///< the C; no glyph has both, and neither is no ink
};

Polygon box( double x0, double y0, double x1, double y1 )
{
	return { { x0, y0 }, { x0, y1 }, { x1, y1 }, { x1, y0 } };
}

/// The C: the disc as make-test-font.py writes it, eight quadratics with
/// their on-curve points on the circle and each control where the tangents
/// meet, rounded to the unit as the font stores them -- worked out here from
/// that description, not read from the font.
std::vector< Quad > specDisc()
{
	const double cx = 400, cy = 300, r = 300, step = 2.0 * kPi / 8.0, rc = r / std::cos( 0.5 * step );
	auto onCurve = [ & ]( int k ) {
		const double a = -k * step;
		return Vec2 { std::round( cx + r * std::cos( a ) ), std::round( cy + r * std::sin( a ) ) };
	};
	std::vector< Quad > out;
	for( int k = 0; k < 8; ++k )
	{
		const double m = -k * step - 0.5 * step;
		out.push_back( { onCurve( k ), { std::round( cx + rc * std::cos( m ) ), std::round( cy + rc * std::sin( m ) ) }, onCurve( k + 1 ) } );
	}
	return out;
}

/// Signed distance to a closed, convex run of quadratics, positive inside,
/// by brute force that shares nothing with the plugin: the nearest of 33
/// samples a quadratic, closed in on by golden section, and the side from the
/// normal at the nearest point (the disc is convex and wound clockwise, so
/// outward is the left of the way it runs). Font units.
double quadSd( const std::vector< Quad >& quads, Vec2 q )
{
	double best = 1e300;
	Vec2 at {}, along {};
	for( const Quad& k : quads )
	{
		auto point = [ & ]( double t ) {
			const double u = 1.0 - t;
			return Vec2 { u * u * k.p0.x + 2.0 * u * t * k.c.x + t * t * k.p2.x, u * u * k.p0.y + 2.0 * u * t * k.c.y + t * t * k.p2.y };
		};
		auto away = [ & ]( double t ) {
			const Vec2 b = point( t );
			return ( b.x - q.x ) * ( b.x - q.x ) + ( b.y - q.y ) * ( b.y - q.y );
		};
		int nearest = 0;
		for( int i = 1; i <= 32; ++i )
			if( away( i / 32.0 ) < away( nearest / 32.0 ) )
				nearest = i;
		double lo = std::max( 0.0, ( nearest - 1 ) / 32.0 ), hi = std::min( 1.0, ( nearest + 1 ) / 32.0 );
		const double g = 0.5 * ( std::sqrt( 5.0 ) - 1.0 );
		for( int i = 0; i < 48; ++i )
		{
			const double a = hi - g * ( hi - lo ), b = lo + g * ( hi - lo );
			if( away( a ) < away( b ) )
				hi = b;
			else
				lo = a;
		}
		double t = 0.5 * ( lo + hi );
		if( away( nearest / 32.0 ) < away( t ) )
			t = nearest / 32.0;
		if( away( t ) < best )
		{
			best  = away( t );
			at    = point( t );
			along = { 2.0 * ( 1.0 - t ) * ( k.c.x - k.p0.x ) + 2.0 * t * ( k.p2.x - k.c.x ), 2.0 * ( 1.0 - t ) * ( k.c.y - k.p0.y ) + 2.0 * t * ( k.p2.y - k.c.y ) };
		}
	}
	const bool inside = ( q.x - at.x ) * -along.y + ( q.y - at.y ) * along.x < 0.0;
	return inside ? std::sqrt( best ) : -std::sqrt( best );
}

/// One glyph of one face of the fixtures: "regular", "bold", "italic" or
/// "twin". Only H differs between them.
SpecGlyph specGlyph( char32_t c, const std::string& face )
{
	SpecGlyph g;
	switch( c )
	{
	case U' ': g.advance = 250; break;
	case U'H':
		if( face == "bold" )
			g.polygons = { box( 50, 0, 750, 600 ) };
		else if( face == "twin" )
			g.polygons = { box( 200, 0, 600, 600 ) };
		else if( face == "italic" )
			g.polygons = { { { 100, 0 }, { 250, 600 }, { 850, 600 }, { 700, 0 } } };
		else
			g.polygons = { box( 100, 0, 700, 600 ) };
		break;
	case U'O': g.polygons = { box( 100, 0, 700, 600 ), box( 250, 150, 550, 450 ) }; break;
	case U'X':
		g.polygons = { { { 100, 200 }, { 300, 200 }, { 300, 0 }, { 500, 0 }, { 500, 200 }, { 700, 200 }, { 700, 400 }, { 500, 400 }, { 500, 600 }, { 300, 600 }, { 300, 400 }, { 100, 400 } } };
		break;
	case U'C': g.quads = specDisc(); break;
	case U'A': g.polygons = { { { 100, 0 }, { 400, 600 }, { 700, 0 } } }; break;
	case U'V': g.polygons = { { { 100, 600 }, { 700, 600 }, { 400, 0 } } }; break;
	case U'I':
		g.advance  = 300;
		g.polygons = { box( 100, 0, 200, 600 ) };
		break;
	default: break;// not in the font: .notdef, which is empty and 800 wide
	}
	return g;
}

bool specInked( char32_t c )
{
	const SpecGlyph g = specGlyph( c, "regular" );
	return !g.polygons.empty() || !g.quads.empty();
}

/// Signed distance from q to a set of polygons (even-odd inside, so a hole
/// is a second polygon), positive inside. Font units.
double polygonSd( const std::vector< Polygon >& polygons, Vec2 q )
{
	bool inside = false;
	double best = 1e300;
	for( const Polygon& p : polygons )
		for( size_t i = 0, j = p.size() - 1; i < p.size(); j = i++ )
		{
			const Vec2 a = p[ j ], b = p[ i ];
			if( ( a.y > q.y ) != ( b.y > q.y ) && q.x < a.x + ( b.x - a.x ) * ( q.y - a.y ) / ( b.y - a.y ) )
				inside = !inside;
			const double dx = b.x - a.x, dy = b.y - a.y;
			const double len = dx * dx + dy * dy;
			const double t   = len > 0.0 ? std::clamp( ( ( q.x - a.x ) * dx + ( q.y - a.y ) * dy ) / len, 0.0, 1.0 ) : 0.0;
			best             = std::min( best, std::hypot( a.x + dx * t - q.x, a.y + dy * t - q.y ) );
		}
	return inside ? best : -best;
}

struct SpecPlaced
{
	char32_t c;
	double x, y;
};

struct SpecSet
{
	std::vector< SpecPlaced > glyphs;
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::string face = "regular";
};

/// The fixtures' setting, by the README's rules: advances, the one kerning
/// pair (A then V, -150), Tracking between glyphs, lines 1000 units apart
/// times Line Spacing, aligned by advance width.
SpecSet specSet( const std::u32string& text, double trackingEm = 0.0, double lineSpacing = 1.0, Align align = Align::Centre,
                 const std::string& face = "regular", bool kerning = true )
{
	SpecSet set;
	set.face  = face;
	size_t start = 0;
	int line     = 0;
	while( start <= text.size() )
	{
		size_t end = text.find( U'\n', start );
		if( end == std::u32string::npos )
			end = text.size();
		std::vector< SpecPlaced > placed;
		double pen = 0.0, width = 0.0;
		char32_t previous = 0;
		for( size_t i = start; i < end; ++i )
		{
			const char32_t c = text[ i ];
			if( kerning && previous == U'A' && c == U'V' )
				pen -= 150.0;
			if( specInked( c ) )
				placed.push_back( { c, pen, -line * 1000.0 * lineSpacing } );
			pen += specGlyph( c, face ).advance;
			width = pen;
			pen += trackingEm * 1000.0;
			previous = c;
		}
		const double shift = align == Align::Left ? 0.0 : align == Align::Centre ? -0.5 * width : -width;
		for( SpecPlaced& p : placed )
		{
			p.x += shift;
			set.glyphs.push_back( p );
		}
		++line;
		start = end + 1;
	}
	set.x0 = set.y0 = 1e300;
	set.x1 = set.y1 = -1e300;
	for( const SpecPlaced& p : set.glyphs )
	{
		const SpecGlyph g = specGlyph( p.c, face );
		//The disc's extremes are on-curve points, so its quadratics' ends
		//find them.
		Polygon ends;
		for( const Quad& k : g.quads )
			ends.push_back( k.p0 );
		std::vector< Polygon > polygons = g.polygons;
		if( !ends.empty() )
			polygons.push_back( ends );
		for( const Polygon& poly : polygons )
			for( const Vec2& v : poly )
			{
				set.x0 = std::min( set.x0, p.x + v.x );
				set.y0 = std::min( set.y0, p.y + v.y );
				set.x1 = std::max( set.x1, p.x + v.x );
				set.y1 = std::max( set.y1, p.y + v.y );
			}
	}
	if( set.glyphs.empty() )
		set.x0 = set.y0 = set.x1 = set.y1 = 0.0;
	return set;
}

/// Where the controls put the setting, from the parameters as the plugin
/// holds them (read back, so a float's rounding is the plugin's too) and
/// Controls' mappings -- which --offline checks on their own.
struct Placement
{
	double W = 0, H = 0, scale = 1, rotation = 0, offX = 0, offY = 0, zoom = 1, zx = 0, zy = 0, ix = 0, iy = 0;
	double unitPx() const
	{
		return scale * zoom;
	}
	Vec2 Screen( Vec2 l ) const
	{
		const double r = g_wrongRotation ? -rotation : rotation;
		const double c = std::cos( r ), s = std::sin( r );
		const double dx = ( l.x - ix ) * scale, dy = ( l.y - iy ) * scale;
		const double px = 0.5 * W + offX + c * dx - s * dy, py = 0.5 * H + offY + s * dx + c * dy;
		return { zx + zoom * ( px - zx ), zy + zoom * ( py - zy ) };
	}
	Vec2 Setting( Vec2 p ) const
	{
		const double r = g_wrongRotation ? -rotation : rotation;
		const double c = std::cos( r ), s = std::sin( r );
		const double px = ( p.x - zx ) / zoom + zx - 0.5 * W - offX, py = ( p.y - zy ) / zoom + zy - 0.5 * H - offY;
		return { ix + ( c * px + s * py ) / scale, iy + ( -s * px + c * py ) / scale };
	}
};

float param( Rig& rig, unsigned int id )
{
	return rig.plugin.GetFloatParameter( id );
}

Placement placementFor( Rig& rig, const SpecSet& set )
{
	Placement p;
	p.W = rig.width;
	p.H = rig.height;
	const double inkW = std::max( set.x1 - set.x0, 1.0 ), inkH = std::max( set.y1 - set.y0, 1.0 );
	const double room = 1.0 - 2.0 * MarginFromParam( param( rig, PT_MARGIN ) );
	switch( static_cast< Fit >( OptionIndex( param( rig, PT_FIT ), static_cast< int >( Fit::Count ) ) ) )
	{
	case Fit::Frame: p.scale = std::min( p.W * room / inkW, p.H * room / inkH ); break;
	case Fit::Width: p.scale = p.W * room / inkW; break;
	case Fit::Height: p.scale = p.H * room / inkH; break;
	default: p.scale = SizeFromParam( param( rig, PT_SIZE ) ) * p.H / 1000.0; break;
	}
	p.rotation = RotationFromParam( param( rig, PT_ROTATION ) ) * kPi / 180.0;
	p.offX     = PositionFromParam( param( rig, PT_POSITION_X ) ) * p.W;
	p.offY     = PositionFromParam( param( rig, PT_POSITION_Y ) ) * p.H;
	p.zoom     = ZoomFromParam( param( rig, PT_ZOOM ) );
	p.zx       = std::clamp( static_cast< double >( param( rig, PT_ZOOM_X ) ), 0.0, 1.0 ) * p.W;
	p.zy       = std::clamp( static_cast< double >( param( rig, PT_ZOOM_Y ) ), 0.0, 1.0 ) * p.H;
	p.ix       = 0.5 * ( set.x0 + set.x1 );
	p.iy       = 0.5 * ( set.y0 + set.y1 );
	return p;
}

/// The edge, by the README: a one-pixel ramp across the outline, widened by
/// Feather; Grow moves the outline along its normal; Outline makes a stroke
/// of that width centred on it.
struct Edge
{
	double ramp = 1.0, grow = 0.0, outline = 0.0;///< pixels
	double Mask( double sdPx ) const
	{
		const double sd = sdPx + grow;
		if( outline > 0.0 )
			return std::clamp( 0.5 + ( 0.5 * outline - std::fabs( sd ) ) / ramp, 0.0, 1.0 );
		return std::clamp( 0.5 + sd / ramp, 0.0, 1.0 );
	}
};

Edge edgeFor( Rig& rig, double unitPx )
{
	Edge e;
	e.ramp    = 1.0 + FeatherFromParam( param( rig, PT_FEATHER ) ) * rig.height;
	e.grow    = GrowFromParam( param( rig, PT_GROW ) ) * 1000.0 * unitPx;
	e.outline = OutlineFromParam( param( rig, PT_OUTLINE ) ) * 1000.0 * unitPx;
	return e;
}

/// The mask the polygons predict, every glyph its own mask and the picture
/// their maximum (README: overlapping letters do not merge their edges).
/// `alphas` per glyph, or empty for all 1.
Floats predictMask( Rig& rig, const SpecSet& set, const std::vector< double >& alphas = {} )
{
	const Placement p = placementFor( rig, set );
	const Edge e      = edgeFor( rig, p.unitPx() );
	const double reachUnits = ( std::fabs( e.grow ) + 0.5 * e.outline + 0.5 * e.ramp + 2.0 ) / p.unitPx();
	Floats mask( static_cast< size_t >( rig.width ) * rig.height, 0.0f );
	std::vector< SpecGlyph > shapes;
	for( const SpecPlaced& g : set.glyphs )
		shapes.push_back( specGlyph( g.c, set.face ) );
	for( int y = 0; y < rig.height; ++y )
		for( int x = 0; x < rig.width; ++x )
		{
			const Vec2 l = p.Setting( { x + 0.5, y + 0.5 } );
			double m     = 0.0;
			for( size_t k = 0; k < set.glyphs.size(); ++k )
			{
				const Vec2 q = { l.x - set.glyphs[ k ].x, l.y - set.glyphs[ k ].y };
				if( q.x < -reachUnits || q.x > 900 + reachUnits || q.y < -reachUnits || q.y > 600 + reachUnits )
					continue;
				const double a = alphas.empty() ? 1.0 : alphas[ k ];
				const double sd = shapes[ k ].quads.empty() ? polygonSd( shapes[ k ].polygons, q ) : quadSd( shapes[ k ].quads, q );
				m               = std::max( m, a * e.Mask( sd * p.unitPx() ) );
			}
			mask[ static_cast< size_t >( y ) * rig.width + x ] = static_cast< float >( m );
		}
	return mask;
}

struct Difference
{
	double worst = 0.0;
	int x = -1, y = -1;
	double got = 0.0, want = 0.0;
	double mean = 0.0;
};

Difference compare( const Floats& got, const Floats& want, int width )
{
	Difference d;
	double sum = 0.0;
	for( size_t i = 0; i < got.size(); ++i )
	{
		const double e = std::fabs( static_cast< double >( got[ i ] ) - want[ i ] );
		sum += e;
		if( e > d.worst )
		{
			d.worst = e;
			d.x     = static_cast< int >( i % static_cast< size_t >( width ) );
			d.y     = static_cast< int >( i / static_cast< size_t >( width ) );
			d.got   = got[ i ];
			d.want  = want[ i ];
		}
	}
	d.mean = got.empty() ? 0.0 : sum / static_cast< double >( got.size() );
	return d;
}

std::string describe( const Difference& d )
{
	return fmt( "worst %.4f at (%d,%d): %.4f, want %.4f", d.worst, d.x, d.y, d.got, d.want );
}

/// R16F holds 0.5 to within 2^-12 and 1 to within 2^-11, and the shader
/// works in float: half a percent of a pixel's coverage is the most that
/// rounding explains. A wrong model is wrong by a tenth or more.
constexpr double kCoverageTolerance = 0.005;

/// Set `text` and the settings, render, and compare the mask with the
/// polygons' prediction.
Difference renderAndCompare( Rig& rig, const std::string& text, const std::vector< std::string >& settings, const std::string& face,
                             double tracking, double lineSpacing, Align align )
{
	rig.Text( PT_TEXT, text );
	applyAll( rig.plugin, settings );
	rig.Render( 1 );
	std::u32string u = DecodeText( text );
	const SpecSet set = specSet( u, tracking, lineSpacing, align, face );
	return compare( rig.Mask(), predictMask( rig, set ), rig.width );
}

std::string sizeSetting( double frameHeights )
{
	return fmt( "Size=%.9g", ParamFromSize( frameHeights ) );
}

//===========================================================================
// Measurements straight from the picture, which use no model at all.
//===========================================================================

/// Where the mask crosses 0.5 between two neighbouring samples a and b
/// (centres at `at` and at + 1) of an edge square to the scan. The ramp is
/// linear and `ramp` pixels wide, so the sample nearer 0.5 is always on it
/// and says exactly where the edge is; the other is usually clamped, and
/// interpolating between the two would pull the answer towards it.
double edgeBetween( double a, double b, double at, double ramp )
{
	const bool rising = b > a;
	if( std::fabs( a - 0.5 ) <= std::fabs( b - 0.5 ) )
		return at + ( rising ? 0.5 - a : a - 0.5 ) * ramp;
	return at + 1.0 - ( rising ? b - 0.5 : 0.5 - b ) * ramp;
}

/// Where a row's mask crosses 0.5: the edges of a straight, upright outline
/// to a small fraction of a pixel.
std::vector< double > crossings( const Floats& mask, int width, int y, double ramp = 1.0 )
{
	std::vector< double > out;
	for( int x = 0; x + 1 < width; ++x )
	{
		const double a = at( mask, width, x, y ), b = at( mask, width, x + 1, y );
		if( ( a < 0.5 ) != ( b < 0.5 ) )
			out.push_back( edgeBetween( a, b, x + 0.5, ramp ) );
	}
	return out;
}

std::vector< double > columnCrossings( const Floats& mask, int width, int height, int x, double ramp = 1.0 )
{
	std::vector< double > out;
	for( int y = 0; y + 1 < height; ++y )
	{
		const double a = at( mask, width, x, y ), b = at( mask, width, x, y + 1 );
		if( ( a < 0.5 ) != ( b < 0.5 ) )
			out.push_back( edgeBetween( a, b, y + 0.5, ramp ) );
	}
	return out;
}

double rowSum( const Floats& mask, int width, int y, int x0 = 0, int x1 = -1 )
{
	if( x1 < 0 )
		x1 = width;
	double sum = 0.0;
	for( int x = x0; x < x1; ++x )
		sum += at( mask, width, x, y );
	return sum;
}

/// The mask's centroid and the angle of its long axis, in a column range.
struct Moments
{
	double mass = 0, cx = 0, cy = 0, angle = 0;
};

Moments momentsOf( const Floats& mask, int width, int height, int x0 = 0, int x1 = -1 )
{
	if( x1 < 0 )
		x1 = width;
	Moments m;
	double sx = 0, sy = 0;
	for( int y = 0; y < height; ++y )
		for( int x = x0; x < x1; ++x )
		{
			const double v = at( mask, width, x, y );
			m.mass += v;
			sx += v * ( x + 0.5 );
			sy += v * ( y + 0.5 );
		}
	if( m.mass <= 0.0 )
		return m;
	m.cx = sx / m.mass;
	m.cy = sy / m.mass;
	double xx = 0, yy = 0, xy = 0;
	for( int y = 0; y < height; ++y )
		for( int x = x0; x < x1; ++x )
		{
			const double v = at( mask, width, x, y ), dx = x + 0.5 - m.cx, dy = y + 0.5 - m.cy;
			xx += v * dx * dx;
			yy += v * dy * dy;
			xy += v * dx * dy;
		}
	m.angle = 0.5 * std::atan2( 2.0 * xy, xx - yy ) * 180.0 / kPi;
	return m;
}

//===========================================================================
// The checks.
//===========================================================================
constexpr int kW = 640, kH = 360;
const std::pair< int, int > kRasters[] = { { kW, kH }, { 320, 180 } };

/// --coverage: the mask, pixel for pixel, against the polygons, over the
/// layouts, fits and transforms, at two rasters.
void checkCoverage( int flags )
{
	Section( "coverage: the mask against the polygons' own, pixel for pixel" );
	struct Case
	{
		const char* what;
		std::string text;
		std::vector< std::string > settings;
		double tracking, spacing;
		Align align;
	};
	const std::string size = sizeSetting( 0.3 );
	const std::vector< Case > cases = {
		{ "H, Fit Frame (the defaults)", "H", {}, 0, 1, Align::Centre },
		{ "HOXAVI at Size 0.3", "HOXAVI", { "Fit=Size", size }, 0, 1, Align::Centre },
		{ "C, the curves, Fit Frame", "C", {}, 0, 1, Align::Centre },
		{ "HCO, turned 63 degrees", "HCO", { "Fit=Frame", fmt( "Rotation=%.9g", ParamFromRotation( 63.0 ) ) }, 0, 1, Align::Centre },
		//About the point at 30 degrees round the disc: (320, 180) + 158.4 (cos, sin).
		{ "C zoomed 40x onto its curve", "C", { "Fit=Frame", fmt( "Zoom=%.9g", ParamFromZoom( 40.0 ) ), "Zoom X=0.7143", "Zoom Y=0.72" }, 0, 1,
		  Align::Centre },
		{ "AVAV, kerned", "AVAV", { "Fit=Width" }, 0, 1, Align::Centre },
		{ "two lines, left, tracked, spaced", "HI\\nIOX", { "Fit=Frame", "Align=Left", fmt( "Tracking=%.9g", ParamFromTracking( 0.12 ) ),
		                                                  fmt( "Line Spacing=%.9g", ParamFromLineSpacing( 1.3 ) ) },
		  0.12, 1.3, Align::Left },
		{ "three lines, right, Fit Height", "H\\nOX\\nI", { "Fit=Height", "Align=Right", fmt( "Margin=%.9g", ParamFromMargin( 0.2 ) ) }, 0, 1, Align::Right },
		{ "turned 30 degrees and moved", "HOX", { "Fit=Size", size, fmt( "Rotation=%.9g", ParamFromRotation( 30.0 ) ),
		                                         fmt( "Position X=%.9g", ParamFromPosition( 0.1 ) ), fmt( "Position Y=%.9g", ParamFromPosition( -0.07 ) ) },
		  0, 1, Align::Centre },
		{ "turned -137 degrees", "AVO", { "Fit=Frame", fmt( "Rotation=%.9g", ParamFromRotation( -137.0 ) ) }, 0, 1, Align::Centre },
		{ "zoomed 7x about (0.3, 0.6)", "HOX", { "Fit=Frame", fmt( "Zoom=%.9g", ParamFromZoom( 7.0 ) ), "Zoom X=0.3", "Zoom Y=0.6" }, 0, 1, Align::Centre },
		{ "all of it at once", "XO\\nHAV", { "Fit=Width", fmt( "Rotation=%.9g", ParamFromRotation( 11.0 ) ), fmt( "Zoom=%.9g", ParamFromZoom( 2.5 ) ),
		                                     "Zoom X=0.45", "Zoom Y=0.55", fmt( "Position X=%.9g", ParamFromPosition( -0.05 ) ),
		                                     fmt( "Tracking=%.9g", ParamFromTracking( -0.05 ) ) },
		  -0.05, 1, Align::Centre },
	};
	for( auto raster : kRasters )
		for( const Case& c : cases )
		{
			Rig rig;
			if( !rig.Init( raster.first, raster.second, nullptr, flags ) )
				return Check( false, "init" );
			const Difference d = renderAndCompare( rig, c.text, c.settings, "regular", c.tracking, c.spacing, c.align );
			//Two empty frames agree perfectly: each case must have its edge
			//on the screen to be measuring anything.
			int edge = 0;
			for( float m : rig.Mask() )
				edge += m > 0.02f && m < 0.98f ? 1 : 0;
			Check( d.worst < kCoverageTolerance && edge >= 20,
			       fmt( "%dx%d %-34s %s, %d edge pixels", raster.first, raster.second, c.what, describe( d ).c_str(), edge ) );
		}
}

/// --zoom: a thousand times into an edge, and into the far end of a long
/// line, where a float would have lost the place.
void checkZoom( int flags )
{
	Section( "zoom: an edge at 10, 100 and 1000 times, and at the end of a long line" );
	for( double zoom : { 10.0, 100.0, 1000.0 } )
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		//The H's left edge, half way up: (W/2 - 300 s, H/2) at Size 0.5.
		const double s     = 0.5 * kH / 1000.0;
		const double edgeX = 0.5 * kW - 300.0 * s;
		const Difference d = renderAndCompare( rig, "H", { "Fit=Size", sizeSetting( 0.5 ), fmt( "Zoom=%.9g", ParamFromZoom( zoom ) ),
		                                                   fmt( "Zoom X=%.9g", edgeX / kW ), "Zoom Y=0.5" },
		                                       "regular", 0, 1, Align::Centre );
		Check( d.worst < kCoverageTolerance, fmt( "H's left edge at %6.0fx: %s", zoom, describe( d ).c_str() ) );
	}
	{
		//Sixteen glyphs across a 4K frame, a thousand times into the last
		//one's top right corner, 6300 units from the setting's centre: a float
		//holds 6300 to 1/2048 of a unit, and a unit is 274 px here.
		Rig rig;
		if( !rig.Init( 3840, 2160, nullptr, flags ) )
			return Check( false, "init" );
		const std::string text = "HHHHHHHHHHHHHHHH";
		rig.Text( PT_TEXT, text );
		applyAll( rig.plugin, { "Fit=Width", fmt( "Margin=%.9g", ParamFromMargin( 0.05 ) ) } );
		const SpecSet set = specSet( DecodeText( text ) );
		const Placement p = placementFor( rig, set );
		const Vec2 corner = p.Screen( { set.glyphs.back().x + 700.0, set.glyphs.back().y + 600.0 } );
		Check( corner.x > 0 && corner.x < rig.width && corner.y > 0 && corner.y < rig.height,
		       fmt( "the corner is on the screen before the zoom: (%.1f, %.1f)", corner.x, corner.y ) );
		applyAll( rig.plugin, { fmt( "Zoom=%.9g", ParamFromZoom( 1000.0 ) ), fmt( "Zoom X=%.9g", corner.x / rig.width ), fmt( "Zoom Y=%.9g", corner.y / rig.height ) } );
		rig.Render( 1 );
		const Floats mask   = rig.Mask();
		const double inked  = std::accumulate( mask.begin(), mask.end(), 0.0 ) / static_cast< double >( mask.size() );
		const Difference d  = compare( mask, predictMask( rig, set ), rig.width );
		Check( d.worst < kCoverageTolerance && inked > 0.1 && inked < 0.9,
		       fmt( "the 16th H's corner at 1000x, 4K: %s; %.0f%% of the frame inked", describe( d ).c_str(), inked * 100.0 ) );
	}
}

/// --edge: Grow, Feather and Outline, against the polygons and measured.
void checkEdge( int flags )
{
	Section( "edge: Grow, Feather and Outline" );
	struct Case
	{
		const char* what;
		std::vector< std::string > settings;
	};
	const std::vector< Case > cases = {
		{ "Grow +0.04 em", { fmt( "Grow=%.9g", ParamFromGrow( 0.04 ) ) } },
		{ "Grow -0.04 em", { fmt( "Grow=%.9g", ParamFromGrow( -0.04 ) ) } },
		{ "Feather 0.5", { "Feather=0.5" } },
		{ "Outline 0.03 em", { fmt( "Outline=%.9g", ParamFromOutline( 0.03 ) ) } },
		{ "Outline, Grow and Feather", { fmt( "Outline=%.9g", ParamFromOutline( 0.05 ) ), fmt( "Grow=%.9g", ParamFromGrow( 0.02 ) ), "Feather=0.3" } },
	};
	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		std::vector< std::string > settings = { "Fit=Size", sizeSetting( 0.35 ) };
		settings.insert( settings.end(), c.settings.begin(), c.settings.end() );
		const Difference d = renderAndCompare( rig, "HOX", settings, "regular", 0, 1, Align::Centre );
		Check( d.worst < kCoverageTolerance, fmt( "%-26s %s", c.what, describe( d ).c_str() ) );
	}

	//Measured: a row through the middle of one H, its sum is the width.
	auto rowOfH = [ & ]( const std::vector< std::string >& settings, double& sum, std::vector< double >& edges ) {
		Rig rig;
		rig.Init( kW, kH, nullptr, flags );
		rig.Text( PT_TEXT, "H" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.35 ) } );
		applyAll( rig.plugin, settings );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		sum               = rowSum( mask, kW, kH / 2 );
		edges             = crossings( mask, kW, kH / 2 );
	};
	const double s = 0.35 * kH / 1000.0;
	double sum     = 0.0;
	std::vector< double > edges;
	rowOfH( { fmt( "Grow=%.9g", ParamFromGrow( 0.05 ) ) }, sum, edges );
	Check( std::fabs( sum - 700.0 * s ) < 0.02, fmt( "Grow +0.05 em widens the H by 0.1 em: %.4f px, want %.4f", sum, 700.0 * s ) );
	rowOfH( { fmt( "Grow=%.9g", ParamFromGrow( -0.05 ) ) }, sum, edges );
	Check( std::fabs( sum - 500.0 * s ) < 0.02, fmt( "Grow -0.05 em narrows it by 0.1 em: %.4f px, want %.4f", sum, 500.0 * s ) );
	rowOfH( { "Feather=0.6" }, sum, edges );
	Check( std::fabs( sum - 600.0 * s ) < 0.02, fmt( "Feather keeps the edge where it was: the row still sums to %.4f px (want %.4f)", sum, 600.0 * s ) );
	{
		//The ramp is linear: from 10% to 90% is 0.8 of it.
		Rig rig;
		rig.Init( kW, kH, nullptr, flags );
		rig.Text( PT_TEXT, "H" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.35 ), "Feather=0.6" } );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		auto crossing     = [ & ]( double level ) {
            for( int x = 0; x + 1 < kW; ++x )
            {
                const double a = at( mask, kW, x, kH / 2 ), b = at( mask, kW, x + 1, kH / 2 );
                if( a < level && b >= level )
                    return x + 0.5 + ( level - a ) / ( b - a );
            }
            return -1.0;
		};
		const double ramp = 1.0 + FeatherFromParam( 0.6f ) * kH;
		const double w    = crossing( 0.9 ) - crossing( 0.1 );
		Check( std::fabs( w - 0.8 * ramp ) < 0.02, fmt( "Feather 0.6: 10-90%% across %.4f px, want 0.8 x %.4f", w, ramp ) );
	}
	{
		//A stroke thinner than a pixel: the ramp is a pixel wide whatever the
		//stroke, so a w-pixel stroke draws (1 + w)^2 / 4 of a pixel's ink
		//across it, not w. Hairlines come out heavier rather than fading:
		//measured here so the README can say it.
		for( double w : { 0.3, 0.6, 1.0, 2.0 } )
		{
			//The I's bar is 3.6 px at Size 0.1; Grow takes it down to w. A
			//sub-pixel stroke's row sum depends on where it falls between
			//pixel centres, so it is averaged over sixteen positions a
			//sixteenth of a pixel apart: the average is the integral.
			const double s     = 0.1 * kH / 1000.0;
			const double grow  = ( w / s - 100.0 ) / 2.0 / 1000.0;
			double sum         = 0.0;
			for( int phase = 0; phase < 16; ++phase )
			{
				Rig rig;
				rig.Init( kW, kH, nullptr, flags );
				rig.Text( PT_TEXT, "I" );
				applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.1 ), fmt( "Grow=%.9g", ParamFromGrow( grow ) ),
				                        fmt( "Position X=%.9g", ParamFromPosition( phase / 16.0 / kW ) ) } );
				rig.Render( 1 );
				sum += rowSum( rig.Mask(), kW, kH / 2 ) / 16.0;
			}
			const double want = w >= 1.0 ? w : ( 1.0 + w ) * ( 1.0 + w ) / 4.0;
			Check( std::fabs( sum - want ) < 0.01, fmt( "a %.1f px stroke draws %.4f px of ink across it, want %.4f", w, sum, want ) );
		}
	}
	{
		//An outline stroke's coverage across it is its width.
		Rig rig;
		rig.Init( kW, kH, nullptr, flags );
		rig.Text( PT_TEXT, "H" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.35 ), fmt( "Outline=%.9g", ParamFromOutline( 0.04 ) ) } );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		const double left = rowSum( mask, kW, kH / 2, 0, kW / 2 );
		const double mid  = at( mask, kW, kW / 2, kH / 2 );
		Check( std::fabs( left - 40.0 * s ) < 0.02 && mid < 1e-4,
		       fmt( "Outline 0.04 em: the left stroke is %.4f px (want %.4f), the middle %.4f (want 0)", left, 40.0 * s, mid ) );
	}
}

/// The four middles of X's internal edges, in screen pixels: on the edges
/// that were inside the other bar before the overlap was resolved.
std::vector< std::pair< int, int > > internalMiddles( Rig& rig, const SpecSet& set )
{
	const Placement p = placementFor( rig, set );
	std::vector< std::pair< int, int > > out;
	for( Vec2 v : { Vec2 { 300, 300 }, Vec2 { 500, 300 }, Vec2 { 400, 200 }, Vec2 { 400, 400 } } )
	{
		const Vec2 s = p.Screen( { set.glyphs[ 0 ].x + v.x, set.glyphs[ 0 ].y + v.y } );
		out.push_back( { static_cast< int >( std::floor( s.x ) ), static_cast< int >( std::floor( s.y ) ) } );
	}
	return out;
}

/// --overlap: two crossing bars make one plus, with no seam where they cross.
void checkOverlap( int flags )
{
	Section( "overlap: crossing contours draw as one shape" );
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, "X" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.5 ) } );
		rig.Render( 1 );
		const std::vector< GlyphOutline >& outlines = rig.plugin.SetterForTest().Outlines();
		int internal = 0, crossed = 0;
		for( const GlyphOutline& o : outlines )
		{
			internal += o.internal;
			crossed += o.crossings;
		}
		Check( internal == 4, fmt( "X: four pieces found inside the other bar (%d), at %d crossings", internal, crossed ) );
		const Floats mask = rig.Mask();
		double lowest     = 1.0;
		for( auto px : internalMiddles( rig, specSet( U"X" ) ) )
			lowest = std::min( lowest, static_cast< double >( at( mask, kW, px.first, px.second ) ) );
		Check( lowest > 0.999, fmt( "filled: no seam along the internal edges (lowest %.4f, want 1)", lowest ) );
	}
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, "X" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.5 ), fmt( "Outline=%.9g", ParamFromOutline( 0.04 ) ) } );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		double highest    = 0.0;
		for( auto px : internalMiddles( rig, specSet( U"X" ) ) )
			highest = std::max( highest, static_cast< double >( at( mask, kW, px.first, px.second ) ) );
		Check( highest < 0.001, fmt( "outlined: no stroke along the internal edges (highest %.4f, want 0)", highest ) );
		const Difference d = compare( mask, predictMask( rig, specSet( U"X" ) ), kW );
		Check( d.worst < kCoverageTolerance, fmt( "outlined X against the plus's own outline: %s", describe( d ).c_str() ) );
	}
}

/// --winding: a hole is a hole, and where two contours wound the same way
/// overlap the letter is still filled.
void checkWinding( int flags )
{
	Section( "winding: non-zero, as TrueType and CFF fill" );
	Rig rig;
	if( !rig.Init( kW, kH, nullptr, flags ) )
		return Check( false, "init" );
	rig.Text( PT_TEXT, "OX" );
	applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.4 ) } );
	rig.Render( 1 );
	const Floats mask = rig.Mask();
	const SpecSet set = specSet( U"OX" );
	const Placement p = placementFor( rig, set );
	auto sample       = [ & ]( int glyph, double x, double y ) {
        const Vec2 s = p.Screen( { set.glyphs[ static_cast< size_t >( glyph ) ].x + x, set.glyphs[ static_cast< size_t >( glyph ) ].y + y } );
        return at( mask, kW, static_cast< int >( s.x ), static_cast< int >( s.y ) );
	};
	Check( sample( 0, 400, 300 ) < 1e-4, fmt( "O's hole is empty: %.4f", sample( 0, 400, 300 ) ) );
	Check( sample( 0, 175, 300 ) > 0.9999, fmt( "O's ring is full: %.4f", sample( 0, 175, 300 ) ) );
	Check( sample( 1, 400, 300 ) > 0.9999, fmt( "X's middle, winding 2, is full: %.4f", sample( 1, 400, 300 ) ) );
}

/// --layout: where the setting puts each glyph, against the README's rules,
/// and the same distances measured in the picture.
void checkLayout( int flags )
{
	Section( "layout: advances, kerning, tracking, lines, alignment" );
	struct Case
	{
		const char* what;
		std::string text;
		double tracking, spacing;
		Align align;
	};
	const std::vector< Case > cases = {
		{ "AV kerned -150", "AV", 0, 1, Align::Centre },
		{ "VA not kerned", "VA", 0, 1, Align::Centre },
		{ "HH tracked +0.25 em", "HH", 0.25, 1, Align::Centre },
		{ "H I H, a space and a narrow glyph", "H IH", 0, 1, Align::Centre },
		{ "a missing glyph still advances", "HZH", 0, 1, Align::Centre },
		{ "lines left", "I\\nHH\\nAVA", 0, 1.5, Align::Left },
		{ "lines centred", "I\\nHH\\nAVA", 0, 0.7, Align::Centre },
		{ "lines right, tracked", "I\\nHH\\nAVA", 0.1, 1, Align::Right },
	};
	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( 320, 180, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, c.text );
		rig.Set( PT_TRACKING, ParamFromTracking( c.tracking ) );
		rig.Set( PT_LINE_SPACING, ParamFromLineSpacing( c.spacing ) );
		rig.Set( PT_ALIGN, static_cast< float >( c.align ) );
		rig.Render( 1 );
		//The plugin's own float rounding of the controls, read back.
		const SpecSet want = specSet( DecodeText( c.text ), TrackingFromParam( param( rig, PT_TRACKING ) ),
		                              LineSpacingFromParam( param( rig, PT_LINE_SPACING ) ), c.align );
		const Setting& got = rig.plugin.SettingForTest();
		double worst       = got.glyphs.size() == want.glyphs.size() ? 0.0 : 1e9;
		for( size_t i = 0; i < std::min( got.glyphs.size(), want.glyphs.size() ); ++i )
			worst = std::max( { worst, std::fabs( got.glyphs[ i ].x - want.glyphs[ i ].x ), std::fabs( got.glyphs[ i ].y - want.glyphs[ i ].y ) } );
		Check( worst < 1e-6, fmt( "%-34s %zu glyphs, worst %.2g units", c.what, got.glyphs.size(), worst ) );
	}
	{
		//In the picture: the two H's centroids are 1050 units apart.
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, "HH" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.2 ), fmt( "Tracking=%.9g", ParamFromTracking( 0.25 ) ) } );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		const Moments a = momentsOf( mask, kW, kH, 0, kW / 2 ), b = momentsOf( mask, kW, kH, kW / 2, kW );
		const double want = ( 800.0 + 1000.0 * TrackingFromParam( param( rig, PT_TRACKING ) ) ) * 0.2 * kH / 1000.0;
		Check( std::fabs( ( b.cx - a.cx ) - want ) < 0.01, fmt( "measured: the H's are %.4f px apart, want %.4f", b.cx - a.cx, want ) );
		Check( std::fabs( a.cy - 0.5 * kH ) < 0.01 && std::fabs( 0.5 * ( a.cx + b.cx ) - 0.5 * kW ) < 0.01,
		       fmt( "measured: the pair is centred, (%.4f, %.4f)", 0.5 * ( a.cx + b.cx ), a.cy ) );
	}
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, "AV" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.25 ) } );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		//The V's tip and the A's apex: x = 400 + 650 and 400, at Size 0.25.
		const Moments a = momentsOf( mask, kW, kH, 0, kW / 2 ), v = momentsOf( mask, kW, kH, kW / 2, kW );
		const double want = 650.0 * 0.25 * kH / 1000.0;
		Check( std::fabs( ( v.cx - a.cx ) - want ) < 0.05, fmt( "measured: kerned AV centroids %.4f px apart, want %.4f", v.cx - a.cx, want ) );
	}
}

/// --fit: the ink's extent measured in the picture.
void checkFit( int flags )
{
	Section( "fit: Frame, Width, Height, Size, with Margin" );
	struct Case
	{
		const char* what;
		std::string text;
		std::vector< std::string > settings;
		char axis;  ///< which extent the fit fixes
		double want;///< as a fraction of that frame dimension, or of H for Size
	};
	const std::vector< Case > cases = {
		{ "Frame, a wide word: its width", "HHHH", { "Fit=Frame", fmt( "Margin=%.9g", ParamFromMargin( 0.05 ) ) }, 'x', 0.9 },
		{ "Frame, a tall block: its height", "I\\nI\\nI", { "Fit=Frame", fmt( "Margin=%.9g", ParamFromMargin( 0.1 ) ) }, 'y', 0.8 },
		{ "Width, a tall block: its width", "I\\nI\\nI", { "Fit=Width", fmt( "Margin=%.9g", ParamFromMargin( 0.3 ) ) }, 'x', 0.4 },
		{ "Height, a wide word: its height", "HH", { "Fit=Height", fmt( "Margin=%.9g", ParamFromMargin( 0.25 ) ) }, 'y', 0.5 },
		{ "Size 0.4: an H is 0.6 em", "H", { "Fit=Size", sizeSetting( 0.4 ) }, 'y', 0.6 * 0.4 },
	};
	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, c.text );
		applyAll( rig.plugin, c.settings );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		double extent     = 0.0;
		if( c.axis == 'x' )
		{
			//Along the row through the first line's middle (the block is
			//centred, and every fixture glyph is full height at 300).
			double lo = 1e9, hi = -1e9;
			for( int y = 0; y < kH; ++y )
			{
				const std::vector< double > e = crossings( mask, kW, y );
				if( !e.empty() )
				{
					lo = std::min( lo, e.front() );
					hi = std::max( hi, e.back() );
				}
			}
			extent = ( hi - lo ) / kW;
		}
		else
		{
			double lo = 1e9, hi = -1e9;
			for( int x = 0; x < kW; ++x )
			{
				const std::vector< double > e = columnCrossings( mask, kW, kH, x );
				if( !e.empty() )
				{
					lo = std::min( lo, e.front() );
					hi = std::max( hi, e.back() );
				}
			}
			extent = ( hi - lo ) / kH;
		}
		const double dimension = c.axis == 'x' ? kW : kH;
		Check( std::fabs( extent - c.want ) * dimension < 0.02,
		       fmt( "%-36s %.5f of the frame, want %.5f", c.what, extent, c.want ) );
	}
}

/// --transform: Position, Rotation and Zoom, measured as moments of the mask.
void checkTransform( int flags )
{
	Section( "transform: Position, Rotation, Zoom, measured" );
	auto measure = [ & ]( const std::string& text, const std::vector< std::string >& settings ) {
		Rig rig;
		rig.Init( kW, kH, nullptr, flags );
		rig.Text( PT_TEXT, text );
		applyAll( rig.plugin, settings );
		rig.Render( 1 );
		return momentsOf( rig.Mask(), kW, kH );
	};
	const std::string size = sizeSetting( 0.15 );
	{
		const Moments m = measure( "H", { "Fit=Size", size, fmt( "Position X=%.9g", ParamFromPosition( 0.25 ) ), fmt( "Position Y=%.9g", ParamFromPosition( -0.1 ) ) } );
		Check( std::fabs( m.cx - 0.75 * kW ) < 0.01 && std::fabs( m.cy - 0.4 * kH ) < 0.01,
		       fmt( "Position (+0.25, -0.1): centre at (%.4f, %.4f), want (%.1f, %.1f)", m.cx, m.cy, 0.75 * kW, 0.4 * kH ) );
	}
	for( double degrees : { 30.0, -75.0, 150.0 } )
	{
		//The I is a bar up the page: its long axis at 90 degrees, turned
		//anticlockwise by Rotation (y up, as GL and the font have it).
		Placement p;
		p.rotation        = degrees * kPi / 180.0;
		const Vec2 tip    = p.Screen( { 0.0, 1.0 } );
		const double want = std::atan2( tip.y - p.Screen( { 0, 0 } ).y, tip.x - p.Screen( { 0, 0 } ).x ) * 180.0 / kPi;
		const Moments m   = measure( "I", { "Fit=Size", sizeSetting( 0.5 ), fmt( "Rotation=%.9g", ParamFromRotation( degrees ) ) } );
		double error      = std::fmod( m.angle - want + 540.0, 180.0 );
		error             = std::min( error, 180.0 - error );
		Check( error < 0.05 && std::fabs( m.cx - 0.5 * kW ) < 0.01 && std::fabs( m.cy - 0.5 * kH ) < 0.01,
		       fmt( "Rotation %+.0f: long axis at %.3f degrees (want %.3f), about the centre (%.3f, %.3f)", degrees, m.angle, want, m.cx, m.cy ) );
	}
	{
		//Zoom 4 about (0.4, 0.5): the frame's centre goes to Zc + 4 (F - Zc).
		const Moments plain  = measure( "H", { "Fit=Size", size } );
		const Moments zoomed = measure( "H", { "Fit=Size", size, fmt( "Zoom=%.9g", ParamFromZoom( 4.0 ) ), "Zoom X=0.4", "Zoom Y=0.5" } );
		const double want    = 0.4 * kW + 4.0 * ( 0.5 * kW - 0.4 * kW );
		Check( std::fabs( zoomed.cx - want ) < 0.01 && std::fabs( zoomed.mass / plain.mass - 16.0 ) < 0.01,
		       fmt( "Zoom 4 about (0.4, 0.5): centre at %.4f (want %.1f), area x%.4f (want 16)", zoomed.cx, want, zoomed.mass / plain.mass ) );
	}
	{
		//All three at once, each in its place: Rotation turns the text about
		//its own centre, so the centre lands at Zc + 1.5 (F + P - Zc) whatever
		//the turn. Alone, a turn has no offset to get wrong.
		Placement p;
		p.rotation          = 30.0 * kPi / 180.0;
		const Vec2 tip      = p.Screen( { 0.0, 1.0 } );
		const double angle  = std::atan2( tip.y - p.Screen( { 0, 0 } ).y, tip.x - p.Screen( { 0, 0 } ).x ) * 180.0 / kPi;
		const Moments m     = measure( "I", { "Fit=Size", sizeSetting( 0.5 ), fmt( "Rotation=%.9g", ParamFromRotation( 30.0 ) ),
		                                      fmt( "Position X=%.9g", ParamFromPosition( 0.1 ) ), fmt( "Position Y=%.9g", ParamFromPosition( -0.05 ) ),
		                                      fmt( "Zoom=%.9g", ParamFromZoom( 1.5 ) ), "Zoom X=0.4", "Zoom Y=0.6" } );
		const double wantX  = 0.4 * kW + 1.5 * ( 0.6 * kW - 0.4 * kW ), wantY = 0.6 * kH + 1.5 * ( 0.45 * kH - 0.6 * kH );
		double error        = std::fmod( m.angle - angle + 540.0, 180.0 );
		error               = std::min( error, 180.0 - error );
		Check( error < 0.05 && std::fabs( m.cx - wantX ) < 0.01 && std::fabs( m.cy - wantY ) < 0.01,
		       fmt( "Rotation +30, Position (+0.1, -0.05), Zoom 1.5 about (0.4, 0.6): axis at %.3f (want %.3f), centre (%.3f, %.3f), want (%.1f, %.1f)",
		            m.angle, angle, m.cx, m.cy, wantX, wantY ) );
	}
}

/// The reveal, by the README: glyph k at rank r_k of R steps fades in over
/// sigma = 1 + Spread (R - 1) steps, alpha = clamp((Reveal (R - 1 + sigma) -
/// r_k) / sigma, 0, 1).
std::vector< double > predictReveal( int n, Order order, double reveal, double spread, const std::vector< int >& randomRanks = {} )
{
	std::vector< double > rank( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
		rank[ static_cast< size_t >( i ) ] = order == Order::Forward    ? i
		                                     : order == Order::Backward ? n - 1 - i
		                                     : order == Order::CentreOut ? std::fabs( i - 0.5 * ( n - 1 ) )
		                                                                 : randomRanks[ static_cast< size_t >( i ) ];
	const double lo = *std::min_element( rank.begin(), rank.end() );
	const double R  = *std::max_element( rank.begin(), rank.end() ) - lo + 1.0;
	const double sigma = 1.0 + spread * ( R - 1.0 );
	std::vector< double > alpha( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
		alpha[ static_cast< size_t >( i ) ] = std::clamp( ( reveal * ( R - 1.0 + sigma ) - ( rank[ static_cast< size_t >( i ) ] - lo ) ) / sigma, 0.0, 1.0 );
	return alpha;
}

/// --reveal: letter by letter, in each order.
void checkReveal( int flags )
{
	Section( "reveal: letter by letter" );
	const std::string text = "HHHHHH";
	const int n            = 6;
	const SpecSet set      = specSet( DecodeText( text ) );
	for( Order order : { Order::Forward, Order::Backward, Order::CentreOut, Order::Random } )
	{
		std::vector< int > ranks;
		if( order == Order::Random )
		{
			//The ranks are the plugin's (a fixed shuffle); what is checked is
			//that they are a permutation and the same for every instance.
			Rig a, b;
			a.Init( 320, 180, nullptr, flags );
			b.Init( 320, 180, nullptr, flags );
			for( Rig* r : { &a, &b } )
			{
				r->Text( PT_TEXT, text );
				applyAll( r->plugin, { "Reveal Order=Random", "Reveal=0" } );
			}
			for( int k = 0; k < n; ++k )
			{
				//Exactly one more glyph is up at each step: its rank is k.
				const float reveal = static_cast< float >( ( k + 0.5 ) / n );
				for( Rig* r : { &a, &b } )
				{
					r->Set( PT_REVEAL, reveal );
					r->Render( 1 );
				}
				for( int i = 0; i < n; ++i )
					if( a.plugin.LastFrame().alphas[ static_cast< size_t >( i ) ] > 0.0 && std::find( ranks.begin(), ranks.end(), i ) == ranks.end()
					    && static_cast< int >( ranks.size() ) <= k )
						ranks.push_back( i );
				Check( a.plugin.LastFrame().alphas == b.plugin.LastFrame().alphas, fmt( "Random: two instances agree at Reveal %.3f", reveal ) );
			}
			std::vector< int > byGlyph( static_cast< size_t >( n ), -1 );
			for( int k = 0; k < static_cast< int >( ranks.size() ); ++k )
				byGlyph[ static_cast< size_t >( ranks[ static_cast< size_t >( k ) ] ) ] = k;
			std::vector< int > sorted = byGlyph;
			std::sort( sorted.begin(), sorted.end() );
			bool permutation = true, identity = true;
			for( int i = 0; i < n; ++i )
			{
				permutation = permutation && sorted[ static_cast< size_t >( i ) ] == i;
				identity    = identity && byGlyph[ static_cast< size_t >( i ) ] == i;
			}
			Check( permutation && !identity, "Random: the glyphs come up one at a time, in an order that is not reading order" );
			ranks = byGlyph;
		}
		for( double spread : { 0.0, 0.5, 1.0 } )
		{
			double worstAlpha = 0.0, worstPicture = 0.0;
			bool monotonic = true;
			std::vector< double > previous( static_cast< size_t >( n ), 0.0 );
			Rig rig;
			if( !rig.Init( kW, kH, nullptr, flags ) )
				return Check( false, "init" );
			rig.Text( PT_TEXT, text );
			applyAll( rig.plugin, { "Fit=Width", fmt( "Reveal Spread=%.9g", spread ) } );
			rig.Set( PT_REVEAL_ORDER, static_cast< float >( order ) );
			for( int step = 0; step <= 20; ++step )
			{
				const float reveal = static_cast< float >( step / 20.0 );
				rig.Set( PT_REVEAL, reveal );
				rig.Render( 1 );
				const std::vector< double > want = predictReveal( n, order, reveal, param( rig, PT_REVEAL_SPREAD ), ranks );
				const std::vector< double >& got = rig.plugin.LastFrame().alphas;
				for( int i = 0; i < n; ++i )
				{
					worstAlpha = std::max( worstAlpha, std::fabs( got[ static_cast< size_t >( i ) ] - want[ static_cast< size_t >( i ) ] ) );
					monotonic  = monotonic && got[ static_cast< size_t >( i ) ] >= previous[ static_cast< size_t >( i ) ] - 1e-12;
				}
				previous = got;
				if( step % 5 == 0 )
				{
					//And in the picture: the mask the polygons predict with
					//those alphas.
					const Difference d = compare( rig.Mask(), predictMask( rig, set, want ), kW );
					worstPicture       = std::max( worstPicture, d.worst );
				}
			}
			Check( worstAlpha < 1e-9 && worstPicture < kCoverageTolerance && monotonic && previous == std::vector< double >( static_cast< size_t >( n ), 1.0 ),
			       fmt( "%-10s Spread %.1f: alphas within %.1g, picture within %.4f, rising, all up at 1", OrderName( order ), spread, worstAlpha, worstPicture ) );
		}
	}
	{
		Rig rig;
		rig.Init( kW, kH, nullptr, flags );
		rig.Text( PT_TEXT, text );
		rig.Set( PT_REVEAL, 0.0f );
		rig.Render( 1 );
		const Floats mask = rig.Mask();
		Check( *std::max_element( mask.begin(), mask.end() ) == 0.0f && rig.plugin.LastFrame().drawn == 0,
		       fmt( "Reveal 0 draws nothing (%d drawn)", rig.plugin.LastFrame().drawn ) );
	}
}

/// --composite: the output from the clip and the mask, every Mask and
/// Background, at three Mixes.
void checkComposite( int flags )
{
	Section( "composite: Inside, Outside, the Backgrounds, Mix" );
	const Floats card = colourCard( kW, kH );
	for( int which = 0; which < static_cast< int >( Mask::Count ); ++which )
		for( int ground = 0; ground < static_cast< int >( Background::Count ); ++ground )
			for( float mix : { 1.0f, 0.5f, 0.0f } )
			{
				//A float clip: an 8-bit one's texelFetch and glReadPixels may
				//round to different floats, and then "bit for bit" would be a
				//comparison of two conversions.
				Rig rig;
				rig.floatClip = true;
				if( !rig.Init( kW, kH, &card, flags ) )
					return Check( false, "init" );
				rig.Text( PT_TEXT, "HOX" );
				applyAll( rig.plugin, { "Fit=Frame", "Feather=0.3", fmt( "Colour=%g", 0.2 ), fmt( "Colour_Green=%g", 0.7 ), fmt( "Colour_Blue=%g", 0.4 ) } );
				rig.Set( PT_MASK, static_cast< float >( which ) );
				rig.Set( PT_BACKGROUND, static_cast< float >( ground ) );
				rig.Set( PT_MIX, mix );
				rig.Render( 1 );
				const Floats out  = rig.Output();
				const Floats mask = rig.Mask();
				const Floats& clip = card;
				float g[ 4 ]      = { 0, 0, 0, 0 };
				if( ground == static_cast< int >( Background::Black ) )
					g[ 3 ] = 1;
				else if( ground == static_cast< int >( Background::White ) )
					g[ 0 ] = g[ 1 ] = g[ 2 ] = g[ 3 ] = 1;
				else if( ground == static_cast< int >( Background::Colour ) )
				{
					g[ 0 ] = 0.2f;
					g[ 1 ] = 0.7f;
					g[ 2 ] = 0.4f;
					g[ 3 ] = 1;
				}
				double worst = 0.0;
				bool exact   = true;
				for( size_t p = 0; p < mask.size(); ++p )
				{
					const double k = which == static_cast< int >( Mask::Outside ) ? 1.0 - mask[ p ] : mask[ p ];
					for( int c = 0; c < 4; ++c )
					{
						const double cl   = clip[ p * 4 + c ];
						const double want = cl + ( cl * k + g[ c ] * ( 1.0 - k ) - cl ) * mix;
						worst             = std::max( worst, std::fabs( out[ p * 4 + c ] - want ) );
						exact             = exact && ( mix != 0.0f || out[ p * 4 + c ] == static_cast< float >( cl ) );
					}
				}
				Check( worst < 2e-6 && exact, fmt( "%-7s on %-11s Mix %.1f: worst %.2g%s", MaskName( static_cast< Mask >( which ) ),
				                                   BackgroundName( static_cast< Background >( ground ) ), mix, worst, mix == 0.0f ? ", the clip bit for bit" : "" ) );
			}
}

/// --state: the host's GL state comes back as it went in.
void checkState( int flags )
{
	Section( "state: the host's GL state comes back as it went in" );
	Rig rig;
	if( !rig.Init( kW, kH, nullptr, flags ) )
		return Check( false, "init" );
	rig.Text( PT_TEXT, "HOX" );
	GLuint hostVAO = 0;
	glGenVertexArrays( 1, &hostVAO );
	glBindVertexArray( hostVAO );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glBlendEquation( GL_FUNC_REVERSE_SUBTRACT );
	glActiveTexture( GL_TEXTURE0 );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 8 );
	glClearColor( 0.25f, 0.5f, 0.75f, 1.0f );
	//Render without the rig's clear, which would set its own colour.
	glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
	glViewport( 0, 0, kW, kH );
	const bool ok = rig.plugin.ProcessOpenGL( &rig.process ) == FF_SUCCESS;
	GLint src = 0, dst = 0, equation = 0, active = 0, unpack = 0, program = 0, vao = 0, viewport[ 4 ];
	GLfloat clear[ 4 ];
	glGetIntegerv( GL_BLEND_SRC_RGB, &src );
	glGetIntegerv( GL_BLEND_DST_RGB, &dst );
	glGetIntegerv( GL_BLEND_EQUATION_RGB, &equation );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &active );
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &unpack );
	glGetIntegerv( GL_CURRENT_PROGRAM, &program );
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &vao );
	glGetIntegerv( GL_VIEWPORT, viewport );
	glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
	Check( ok, "rendered" );
	Check( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blending as the host left it" );
	Check( equation == GL_FUNC_REVERSE_SUBTRACT, "the host's blend equation, not MAX" );
	Check( active == GL_TEXTURE0, "texture unit 0 active" );
	Check( unpack == 8, "the host's unpack alignment" );
	Check( program == 0, "no program left bound" );
	Check( vao == static_cast< GLint >( hostVAO ), "the host's vertex array" );
	Check( viewport[ 2 ] == kW && viewport[ 3 ] == kH, "the host's viewport" );
	Check( clear[ 0 ] == 0.25f && clear[ 2 ] == 0.75f, "the host's clear colour" );
	GLint bound = 0, buffers = 0;
	for( int unit = 0; unit < 4; ++unit )
	{
		glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + unit ) );
		GLint t = 0, b = 0;
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &t );
		glGetIntegerv( GL_TEXTURE_BINDING_BUFFER, &b );
		bound   = std::max( bound, t );
		buffers = std::max( buffers, b );
	}
	glActiveTexture( GL_TEXTURE0 );
	Check( bound == 0 && buffers == 0, "no texture or texture buffer left on units 0-3" );
	glBindVertexArray( 0 );
	glDeleteVertexArrays( 1, &hostVAO );
	glDisable( GL_BLEND );
	glBlendEquation( GL_FUNC_ADD );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	const GLenum error = glGetError();
	Check( error == GL_NO_ERROR, fmt( "no GL error (0x%04x)", error ) );
}

/// --resize: the host's raster changes under a running instance.
void checkResize( int flags )
{
	Section( "resize: a new raster under a running instance" );
	Rig rig;
	if( !rig.Init( kW, kH, nullptr, flags ) )
		return Check( false, "init" );
	rig.Text( PT_TEXT, "HOX" );
	applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.3 ) } );
	bool ok = rig.Render( 2 );
	for( auto size : { std::make_pair( 1920, 1080 ), std::make_pair( 333, 777 ), std::make_pair( kW, kH ) } )
	{
		ok = ok && rig.Resize( size.first, size.second ) && rig.Render( 2 );
		const Difference d = compare( rig.Mask(), predictMask( rig, specSet( U"HOX" ) ), rig.width );
		Check( ok && d.worst < kCoverageTolerance, fmt( "%dx%d: %s", size.first, size.second, describe( d ).c_str() ) );
	}
}

/// --fonts: the Font list, Weight and Italic, Font Name, Font File.
void checkFonts( int flags )
{
	Section( "fonts: choosing a face" );
	const std::vector< FontFamily >& families = InstalledFamilies();
	std::string names;
	for( const FontFamily& f : families )
		names += ( names.empty() ? "" : ", " ) + f.name + " (" + std::to_string( f.faces.size() ) + ")";
	Check( families.size() == 2 && families[ 0 ].name == "Knockout Test" && families[ 0 ].faces.size() == 3 && families[ 1 ].name == "Knockout Twin",
	       "the fixtures' two families: " + names );
	Check( DefaultFamily() == 0, "with no Arial, the first family is the default" );

	struct Case
	{
		const char* what;
		std::vector< std::string > settings;
		std::string face;///< the spec face the picture must be
		int weight;
		bool italic;
		std::string fieldAfter;///< Font Name after the frame
	};
	const std::string bold = std::string( KNOCKOUT_FIXTURES ) + "/KnockoutTest-Bold.ttf";
	const std::vector< Case > cases = {
		{ "Black finds Bold (the default weight)", { "Weight=Black" }, "bold", 700, false, "Knockout Test" },
		{ "Regular", { "Weight=Regular" }, "regular", 400, false, "Knockout Test" },
		{ "Thin finds Regular (lighter first, then heavier)", { "Weight=Thin" }, "regular", 400, false, "Knockout Test" },
		{ "Medium finds Regular (500 looks down before up)", { "Weight=Medium" }, "regular", 400, false, "Knockout Test" },
		{ "Semibold finds Bold (above 500 looks up first)", { "Weight=Semibold" }, "bold", 700, false, "Knockout Test" },
		{ "Italic", { "Weight=Regular", "Italic=1" }, "italic", 400, true, "Knockout Test" },
		{ "Bold Italic finds the Italic (style before weight)", { "Weight=Bold", "Italic=1" }, "italic", 400, true, "Knockout Test" },
		{ "Font Name, any case, wins", { "Font Name=knockout twin" }, "twin", 400, false, "Knockout Twin" },
		{ "Font moved alone picks by index", { "Font=1" }, "twin", 400, false, "Knockout Twin" },
		{ "a composition's restore: the name wins over the index", { "Font=1", "Font Name=Knockout Test" }, "regular", 400, false, "Knockout Test" },
		{ "a name not installed is kept as typed", { "Weight=Regular", "Font Name=Nonexistent Sans" }, "regular", 400, false, "Nonexistent Sans" },
		{ "Font File beats the list", { "Weight=Regular", "Font Name=Knockout Twin", "Font File=" + bold }, "bold", 700, false, "Knockout Twin" },
		{ "a Font File that is not a font falls back", { "Weight=Regular", "Font File=" + std::string( KNOCKOUT_FIXTURES ) + "/../make-test-font.py" }, "regular", 400, false, "Knockout Test" },
		{ "a Font File that is not there falls back", { "Weight=Regular", "Font File=/nonexistent/face.ttf" }, "regular", 400, false, "Knockout Test" },
	};
	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, "HOX" );
		applyAll( rig.plugin, { "Fit=Size", sizeSetting( 0.3 ) } );
		applyAll( rig.plugin, c.settings );
		rig.Render( 1 );
		const FrameState& f = rig.plugin.LastFrame();
		const Floats mask   = rig.Mask();
		const Difference d  = compare( mask, predictMask( rig, specSet( U"HOX", 0, 1, Align::Centre, c.face ) ), kW );
		const std::string field = rig.plugin.TextValue( PT_FONT_NAME );
		Check( f.weight == c.weight && f.italic == c.italic && d.worst < kCoverageTolerance && field == c.fieldAfter,
		       fmt( "%-52s %s %s %d%s; Font Name '%s'; %s", c.what, f.family.c_str(), f.style.c_str(), f.weight, f.fromFile ? " (file)" : "",
		            field.c_str(), describe( d ).c_str() ) );
	}
}

/// --text: what the Text field turns into.
void checkText( int flags )
{
	Section( "text: the field, escapes, limits" );
	{
		Rig rig;
		if( !rig.Init( 320, 180, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, "" );
		const bool ok     = rig.Render( 1 );
		const Floats mask = rig.Mask();
		Check( ok && *std::max_element( mask.begin(), mask.end() ) == 0.0f, "empty text renders, and draws nothing" );
		rig.Text( PT_TEXT, "   " );
		Check( rig.Render( 1 ) && rig.plugin.LastFrame().glyphs == 0, "spaces alone: nothing to draw, no failure" );
		rig.Text( PT_TEXT, "HOX" );
		rig.Render( 1 );
		Check( rig.plugin.LastFrame().glyphs == 3 && rig.plugin.TextValue( PT_TEXT ) == "HOX", "changed between frames: set again, and read back" );
	}
	{
		Rig rig;
		if( !rig.Init( 320, 180, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, std::string( 5000, 'H' ) );
		const auto start = std::chrono::steady_clock::now();
		const bool ok    = rig.Render( 1 );
		const double ms  = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
		Check( ok && rig.plugin.LastFrame().glyphs == 4096, fmt( "5000 characters: the first 4096 drawn (%d), in %.0f ms", rig.plugin.LastFrame().glyphs, ms ) );
	}
	{
		//Every escape at once, set and measured.
		Rig rig;
		if( !rig.Init( 320, 180, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, "H\\nH\tH\r\nI" );
		rig.Render( 1 );
		Check( rig.plugin.LastFrame().lines == 3 && rig.plugin.LastFrame().glyphs == 4, fmt( "\\n, a tab and CR LF: %d lines, %d glyphs (want 3, 4)",
		                                                                                     rig.plugin.LastFrame().lines, rig.plugin.LastFrame().glyphs ) );
	}
}

/// stb_truetype's own rasterisation of one glyph at `scale` pixels per unit,
/// its origin at screen (ox, oy) (y up), into a full-frame float image.
void stbGlyph( const stbtt_fontinfo& info, int glyph, double scale, double ox, double oy, int width, int height, Floats& into )
{
	const double fx = std::floor( ox ), cy = std::ceil( oy );
	const float shiftX = static_cast< float >( ox - fx ), shiftY = static_cast< float >( cy - oy );
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	stbtt_GetGlyphBitmapBoxSubpixel( &info, glyph, static_cast< float >( scale ), static_cast< float >( scale ), shiftX, shiftY, &x0, &y0, &x1, &y1 );
	const int w = x1 - x0, h = y1 - y0;
	if( w <= 0 || h <= 0 )
		return;
	std::vector< unsigned char > bitmap( static_cast< size_t >( w ) * h );
	//stbtt_MakeGlyphBitmapSubpixel, but flattening the curves to 0.02 px
	//rather than its 0.35: at 0.35 its chords cut inside every convex curve
	//and a curvy face loses most of a percent of its area to them.
	stbtt_vertex* vertices = nullptr;
	const int count        = stbtt_GetGlyphShape( &info, glyph, &vertices );
	stbtt__bitmap target;
	target.w      = w;
	target.h      = h;
	target.stride = w;
	target.pixels = bitmap.data();
	stbtt_Rasterize( &target, 0.02f, vertices, count, static_cast< float >( scale ), static_cast< float >( scale ), shiftX, shiftY, x0, y0, 1, info.userdata );
	stbtt_FreeShape( &info, vertices );
	for( int by = 0; by < h; ++by )
		for( int bx = 0; bx < w; ++bx )
		{
			const int x = static_cast< int >( fx ) + x0 + bx;
			const int y = static_cast< int >( cy ) - y0 - by - 1;
			if( x < 0 || y < 0 || x >= width || y >= height )
				continue;
			float& v = into[ static_cast< size_t >( y ) * width + x ];
			v        = std::max( v, bitmap[ static_cast< size_t >( by ) * w + bx ] / 255.0f );
		}
}

struct Agreement
{
	int hard  = 0;  ///< pixels one calls inside (> 0.9) and the other outside (< 0.1)
	double mean = 0;///< mean |difference| over pixels either touches
	double area = 0;///< relative difference of the summed coverage
};

/// The plugin's mask against stb_truetype's rasteriser for the same face and
/// text, upright, no zoom: the face the plugin loaded, by its file.
Agreement compareWithStb( Rig& rig, const std::string& path, int collection )
{
	std::ifstream file( path, std::ios::binary );
	std::vector< unsigned char > data( ( std::istreambuf_iterator< char >( file ) ), std::istreambuf_iterator< char >() );
	stbtt_fontinfo info {};
	Agreement a;
	if( data.empty() || stbtt_InitFont( &info, data.data(), stbtt_GetFontOffsetForIndex( data.data(), collection ) ) == 0 )
	{
		a.hard = -1;
		return a;
	}
	const FrameState& f = rig.plugin.LastFrame();
	const Setting& s    = rig.plugin.SettingForTest();
	Floats reference( static_cast< size_t >( rig.width ) * rig.height, 0.0f );
	//The setting's glyphs, by the plugin's layout (checked on its own by
	//--layout): each origin to the screen with the plugin's transform.
	const std::vector< GlyphOutline >& outlines = rig.plugin.SetterForTest().Outlines();
	(void)outlines;
	const Font* font = rig.plugin.SetterForTest().CurrentFont();
	std::u32string text = DecodeText( rig.plugin.TextValue( PT_TEXT ) );
	std::vector< int > glyphIds;
	for( char32_t c : text )
		if( c != U'\n' && font != nullptr )
		{
			const int g = font->GlyphFor( c );
			if( !font->Shape( g ).empty() )
				glyphIds.push_back( g );
		}
	for( size_t i = 0; i < s.glyphs.size() && i < glyphIds.size(); ++i )
	{
		const Vec2 o = f.ScreenFromLayout( { s.glyphs[ i ].x, s.glyphs[ i ].y } );
		stbGlyph( info, glyphIds[ i ], f.unitPx, o.x, o.y, rig.width, rig.height, reference );
	}
	const Floats mask = rig.Mask();
	double sumMask = 0, sumRef = 0, sumDiff = 0;
	int touched = 0;
	for( size_t i = 0; i < mask.size(); ++i )
	{
		const double m = mask[ i ], r = reference[ i ];
		if( ( m > 0.9 && r < 0.1 ) || ( r > 0.9 && m < 0.1 ) )
			++a.hard;
		if( m > 0.0 || r > 0.0 )
		{
			++touched;
			sumDiff += std::fabs( m - r );
		}
		sumMask += m;
		sumRef += r;
	}
	a.mean = touched > 0 ? sumDiff / touched : 0.0;
	a.area = sumRef > 0 ? ( sumMask - sumRef ) / sumRef : 0.0;
	//KOTEST_DUMP=folder: the two pictures and their difference (red where
	//the plugin has more), to look at when this disagrees.
	if( const char* dump = std::getenv( "KOTEST_DUMP" ) )
	{
		Floats got( mask.size() * 4 ), want( mask.size() * 4 ), diff( mask.size() * 4 );
		for( size_t i = 0; i < mask.size(); ++i )
			for( int c = 0; c < 3; ++c )
			{
				got[ i * 4 + c ]  = mask[ i ];
				want[ i * 4 + c ] = reference[ i ];
				diff[ i * 4 + c ] = c == 0 ? std::max( 0.0f, mask[ i ] - reference[ i ] ) * 4.0f : c == 2 ? std::max( 0.0f, reference[ i ] - mask[ i ] ) * 4.0f : 0.0f;
			}
		const std::string stem = std::string( dump ) + "/" + std::to_string( g_checks );
		writePng( stem + "-plugin.png", rig.width, rig.height, got );
		writePng( stem + "-stb.png", rig.width, rig.height, want );
		writePng( stem + "-diff.png", rig.width, rig.height, diff );
	}
	return a;
}

/// --stb: the fixtures, curves included, against stb_truetype's rasteriser.
void checkStb( int flags )
{
	Section( "stb: the mask against stb_truetype's own rasteriser" );
	for( const char* text : { "HOXCAV", "C" } )
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, text );
		applyAll( rig.plugin, { "Fit=Frame", "Weight=Regular" } );
		rig.Render( 1 );
		const Agreement a = compareWithStb( rig, std::string( KNOCKOUT_FIXTURES ) + "/KnockoutTest-Regular.ttf", 0 );
		Check( a.hard == 0 && a.mean < 0.02 && std::fabs( a.area ) < 0.003,
		       fmt( "%-8s em %.0f px: %d hard disagreements, mean |difference| %.4f, area %+.4f%%", text, rig.plugin.LastFrame().unitPx * 1000.0, a.hard,
		            a.mean, a.area * 100.0 ) );
	}
}

/// --controls: every control changes the output where it should.
void checkControls( int flags )
{
	Section( "controls: every one changes the picture" );
	struct Probe
	{
		unsigned int id;
		std::vector< std::string > context;
		std::string a, b;
	};
	const std::string bold = std::string( KNOCKOUT_FIXTURES ) + "/KnockoutTest-Bold.ttf";
	const std::vector< Probe > probes = {
		{ PT_TEXT, {}, "Text=HOX", "Text=XOH" },
		{ PT_FONT, {}, "Font=0", "Font=1" },
		{ PT_FONT_NAME, {}, "Font Name=Knockout Test", "Font Name=Knockout Twin" },
		{ PT_FONT_FILE, { "Weight=Regular" }, "Font File=", "Font File=" + bold },
		{ PT_WEIGHT, {}, "Weight=Regular", "Weight=Bold" },
		{ PT_ITALIC, { "Weight=Regular" }, "Italic=0", "Italic=1" },
		{ PT_FIT, {}, "Fit=Frame", "Fit=Size" },
		{ PT_SIZE, { "Fit=Size" }, sizeSetting( 0.2 ), sizeSetting( 0.3 ) },
		{ PT_MARGIN, {}, "Margin=0.1", "Margin=0.3" },
		{ PT_ALIGN, { "Text=I\\nHH" }, "Align=Left", "Align=Right" },
		{ PT_TRACKING, {}, "Tracking=0.25", "Tracking=0.5" },
		{ PT_LINE_SPACING, { "Text=H\\nI", "Fit=Size", sizeSetting( 0.2 ) }, "Line Spacing=0.33", "Line Spacing=0.6" },
		{ PT_POSITION_X, {}, "Position X=0.5", "Position X=0.6" },
		{ PT_POSITION_Y, {}, "Position Y=0.5", "Position Y=0.6" },
		{ PT_ROTATION, {}, "Rotation=0.5", "Rotation=0.6" },
		{ PT_ZOOM, {}, "Zoom=0", "Zoom=0.2" },
		{ PT_ZOOM_X, { "Zoom=0.2" }, "Zoom X=0.3", "Zoom X=0.7" },
		{ PT_ZOOM_Y, { "Zoom=0.2" }, "Zoom Y=0.3", "Zoom Y=0.7" },
		{ PT_GROW, {}, "Grow=0.5", "Grow=0.7" },
		{ PT_FEATHER, {}, "Feather=0", "Feather=0.5" },
		{ PT_OUTLINE, {}, "Outline=0", "Outline=0.2" },
		{ PT_REVEAL, {}, "Reveal=1", "Reveal=0.5" },
		{ PT_REVEAL_SPREAD, { "Reveal=0.5" }, "Reveal Spread=0", "Reveal Spread=1" },
		{ PT_REVEAL_ORDER, { "Reveal=0.5" }, "Reveal Order=Forward", "Reveal Order=Backward" },
		{ PT_MASK, {}, "Mask=Inside", "Mask=Outside" },
		{ PT_BACKGROUND, {}, "Background=Transparent", "Background=White" },
		{ PT_COLOUR_R, { "Background=Colour" }, "Colour=0.2", "Colour=0.9" },
		{ PT_COLOUR_G, { "Background=Colour" }, "Colour_Green=0.2", "Colour_Green=0.9" },
		{ PT_COLOUR_B, { "Background=Colour" }, "Colour_Blue=0.2", "Colour_Blue=0.9" },
		{ PT_MIX, {}, "Mix=1", "Mix=0.5" },
	};
	std::vector< bool > covered( PT_ABOUT_TEXT, false );
	const int W = 320, H = 180;
	const Floats card = colourCard( W, H );
	for( const Probe& probe : probes )
	{
		Floats out[ 2 ];
		for( int side = 0; side < 2; ++side )
		{
			Rig rig;
			rig.Init( W, H, &card, flags );
			applyAll( rig.plugin, { "Text=HOX" } );
			applyAll( rig.plugin, probe.context );
			applyAll( rig.plugin, { side == 0 ? probe.a : probe.b } );
			rig.Render( 1 );
			out[ side ] = rig.Output();
		}
		double d = 0.0;
		for( size_t i = 0; i < out[ 0 ].size(); ++i )
			d += std::fabs( out[ 0 ][ i ] - out[ 1 ][ i ] );
		d /= static_cast< double >( out[ 0 ].size() );
		covered[ probe.id ] = true;
		Check( d > 1.0e-4, fmt( "%-14s %.5f", NameOf( probe.id ), d ) );
	}
	int missing = 0;
	for( unsigned int id = 0; id < PT_ABOUT_TEXT; ++id )
		if( !covered[ id ] )
		{
			++missing;
			std::printf( "  no probe for %s\n", NameOf( id ) );
		}
	Check( missing == 0, fmt( "every control has a probe (%d without)", missing ) );
}

/// --real (with --system-fonts): this Mac's faces, a CFF one and variable
/// ones with overlapping contours, against stb_truetype.
void checkReal( int flags )
{
	Section( "real: this Mac's fonts" );
	if( !g_systemFonts )
		return Check( false, "--real needs --system-fonts" );
	const std::vector< FontFamily >& families = InstalledFamilies();
	const std::vector< FontFace >& faces      = InstalledFaces();
	const int fallback                        = DefaultFamily();
	Check( !families.empty() && fallback >= 0, fmt( "%zu families, %zu faces; the default is %s", families.size(), faces.size(),
	                                                fallback >= 0 ? families[ static_cast< size_t >( fallback ) ].name.c_str() : "none" ) );
	{
		Rig rig;
		if( !rig.Init( kW, kH, nullptr, flags ) )
			return Check( false, "init" );
		rig.Render( 1 );
		const FrameState& f = rig.plugin.LastFrame();
		Check( f.family == "Arial Black" || f.family == "Arial", fmt( "a fresh instance: %s %s %d, %d glyphs", f.family.c_str(), f.style.c_str(), f.weight, f.glyphs ) );
	}
	//Its glyf is placeholder boxes; the pictures are sbix bitmaps.
	if( std::FILE* emoji = std::fopen( "/System/Library/Fonts/Apple Color Emoji.ttc", "rb" ) )
	{
		std::fclose( emoji );
		Check( FindFamily( "Apple Color Emoji" ) < 0, "Apple Color Emoji, whose glyphs are bitmaps, is not offered" );
	}
	struct Case
	{
		const char* family;///< by Font Name, or
		std::string file;  ///< by Font File
		bool italic;
		std::string text;
	};
	//SF ("System Font" to the scan) and New York are variable fonts whose
	//default instances draw letters as overlapping strokes. New York's family
	//name starts with a dot -- Apple's mark for a face apps should not list --
	//so it is reached by Font File, as a user would reach it.
	const std::vector< Case > cases = {
		{ "Arial Black", "", false, "KNOCKOUT 2026" },
		{ "Helvetica", "", false, "Rag & Bone, qpg" },
		{ "STIXGeneral", "", false, "Quiet @ Gravity 8%" },
		{ "STIXGeneral", "", true, "Swash fy&" },
		{ "System Font", "", false, "A&8R%Qk@" },
		{ nullptr, "/System/Library/Fonts/NewYork.ttf", false, "W\xC3\xA9g 8&@" },
	};
	for( const Case& c : cases )
	{
		const int family = c.family ? FindFamily( c.family ) : -1;
		if( c.family ? family < 0 : access( c.file.c_str(), R_OK ) != 0 )
		{
			std::printf( "  skip  %s is not installed\n", c.family ? c.family : c.file.c_str() );
			continue;
		}
		//At 720p: at 360 New York's hairlines are under a pixel, where the
		//ramp is known to draw them heavier (--edge measures it).
		Rig rig;
		if( !rig.Init( 1280, 720, nullptr, flags ) )
			return Check( false, "init" );
		rig.Text( PT_TEXT, c.text );
		applyAll( rig.plugin, { "Weight=Regular", c.italic ? "Italic=1" : "Italic=0", "Fit=Width" } );
		if( c.family )
			applyAll( rig.plugin, { std::string( "Font Name=" ) + c.family } );
		else
			applyAll( rig.plugin, { "Font File=" + c.file } );
		rig.Render( 1 );
		const FrameState& f = rig.plugin.LastFrame();
		std::string path    = c.file;
		int collection      = 0;
		if( c.family )
		{
			const FontFace& ff = faces[ static_cast< size_t >( MatchFace( family, 400, c.italic ) ) ];
			path               = ff.path;
			collection         = ff.collectionIndex;
		}
		const Agreement a = compareWithStb( rig, path, collection );
		int internal      = 0;
		for( const GlyphOutline& o : rig.plugin.SetterForTest().Outlines() )
			internal += o.internal;
		Check( a.hard == 0 && a.mean < 0.01 && std::fabs( a.area ) < 0.002 && ( c.family || f.fromFile ),
		       fmt( "%-12s %-7s %s: %d pieces, %d internal; %d hard, mean %.4f, area %+.3f%%", f.family.c_str(), f.style.c_str(),
		            rig.plugin.TextValue( PT_TEXT ).c_str(), f.pieces, internal, a.hard, a.mean, a.area * 100.0 ) );
	}
}

/// --bench: what a frame costs on this GPU.
void runBench()
{
	Section( "bench: milliseconds a frame, GPU finished" );
	struct Case
	{
		const char* what;
		std::vector< std::string > settings;
	};
	std::string paragraph;
	for( int i = 0; i < 8; ++i )
		paragraph += std::string( i ? "\\n" : "" ) + "The quick brown fox jumps over the lazy dog, 0123456789.";
	const Case cases[] = {
		{ "the defaults", {} },
		{ "a word, outlined and feathered", { fmt( "Outline=%.9g", ParamFromOutline( 0.04 ) ), "Feather=0.5" } },
		{ "a word, 1000x into a stroke", { fmt( "Zoom=%.9g", ParamFromZoom( 1000.0 ) ), "Zoom X=0.31", "Zoom Y=0.5" } },
		{ "eight lines of 56", { "Text=" + paragraph, "Weight=Regular" } },
	};
	for( const Case& c : cases )
		for( auto size : { std::make_pair( 1280, 720 ), std::make_pair( 1920, 1080 ), std::make_pair( 3840, 2160 ) } )
		{
			Rig rig;
			rig.Init( size.first, size.second );
			applyAll( rig.plugin, c.settings );
			rig.Render( 3 );
			glFinish();
			const int frames = 30;
			const auto start = std::chrono::steady_clock::now();
			rig.Render( frames );
			glFinish();
			const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;
			std::printf( "  %-32s %4dx%-4d  %7.2f ms  (%d glyphs, %d pieces)\n", c.what, size.first, size.second, ms, rig.plugin.LastFrame().glyphs,
			             rig.plugin.LastFrame().pieces );
		}
}

//---------------------------------------------------------------------------
// No GL.
//---------------------------------------------------------------------------
GlyphOutline fixtureOutline( char32_t c, bool resolve = true )
{
	static Font font;
	if( !font.IsLoaded() )
		font.Load( std::string( KNOCKOUT_FIXTURES ) + "/KnockoutTest-Regular.ttf", 0 );
	OutlineOptions options;
	options.resolveOverlaps = resolve;
	return BuildOutline( font.Shape( font.GlyphFor( c ) ), font.UnitsPerEm(), options );
}

void checkOffline()
{
	Section( "offline: the controls, the outlines, the text" );
	{
		//Each mapping and its inverse, and the documented ends.
		double worst = 0.0;
		for( float v = 0.0f; v <= 1.0f; v += 0.0625f )
		{
			auto off = [ v ]( float back ) { return std::fabs( static_cast< double >( back ) - v ); };
			worst    = std::max( { worst, off( ParamFromSize( SizeFromParam( v ) ) ), off( ParamFromMargin( MarginFromParam( v ) ) ),
                                off( ParamFromTracking( TrackingFromParam( v ) ) ), off( ParamFromLineSpacing( LineSpacingFromParam( v ) ) ),
                                off( ParamFromPosition( PositionFromParam( v ) ) ), off( ParamFromRotation( RotationFromParam( v ) ) ),
                                off( ParamFromZoom( ZoomFromParam( v ) ) ), off( ParamFromGrow( GrowFromParam( v ) ) ),
                                off( ParamFromFeather( FeatherFromParam( v ) ) ), off( ParamFromOutline( OutlineFromParam( v ) ) ) } );
		}
		Check( worst < 1e-5, fmt( "every mapping inverts (%.1e)", worst ) );
		Check( SizeFromParam( 0 ) == 0.02 && std::fabs( SizeFromParam( 1 ) - 2.0 ) < 1e-12 && ZoomFromParam( 0 ) == 1.0 && std::fabs( ZoomFromParam( 1 ) - 1000.0 ) < 1e-9
		           && RotationFromParam( 0.5f ) == 0.0 && PositionFromParam( 0.5f ) == 0.0 && GrowFromParam( 0.5f ) == 0.0 && FeatherFromParam( 0 ) == 0.0
		           && std::fabs( TrackingFromParam( 0.25f ) ) < 1e-12,
		       "the ends and the middles are where Controls.h says" );
	}
	{
		//CSS Fonts 4, 5.2: the specification's own order.
		const std::vector< int > some = { 100, 300, 400, 700, 900 };
		Check( NearestWeight( 400, some ) == 400 && NearestWeight( 500, some ) == 400 && NearestWeight( 450, { 300, 500, 700 } ) == 500
		           && NearestWeight( 450, { 300, 700 } ) == 300 && NearestWeight( 600, some ) == 700 && NearestWeight( 600, { 100, 300 } ) == 300
		           && NearestWeight( 200, some ) == 100 && NearestWeight( 50, { 300, 700 } ) == 300 && NearestWeight( 800, some ) == 900
		           && NearestWeight( 950, { 300, 700 } ) == 700,
		       "nearest weight follows CSS: 400 then 500 look down first, above 500 up, below 400 down" );
	}
	{
		//Cubics to quadratics: the deviation, measured densely and on its
		//own, within the tolerance, over random cubics an em across.
		std::mt19937 random( 7 );
		std::uniform_real_distribution< double > u( -500.0, 500.0 );
		double worst  = 0.0;
		size_t pieces = 0;
		for( int i = 0; i < 2000; ++i )
		{
			const Vec2 p0 { u( random ), u( random ) }, c1 { u( random ), u( random ) }, c2 { u( random ), u( random ) }, p3 { u( random ), u( random ) };
			const std::vector< Piece > q = CubicToQuads( p0, c1, c2, p3, kCubicTolerance * 1000.0 );
			pieces += q.size();
			worst = std::max( worst, CubicDeviation( p0, c1, c2, p3, q ) );
		}
		Check( worst <= kCubicTolerance * 1000.0 * 1.0001, fmt( "2000 random cubics: within %.2e units of their quadratics (tolerance %.2e), %.1f a cubic",
		                                                       worst, kCubicTolerance * 1000.0, pieces / 2000.0 ) );
	}
	{
		//Every piece monotonic in y; the winding and the distance against
		//the polygons, at random points.
		std::mt19937 random( 11 );
		std::uniform_real_distribution< double > ux( 0.0, 800.0 ), uy( -100.0, 700.0 );
		bool monotonic = true;
		double worstDistance = 0.0;
		int wrongInside      = 0;
		for( char32_t c : std::u32string( U"HOXAVI" ) )
		{
			const GlyphOutline o = fixtureOutline( c );
			for( const Piece& p : o.pieces )
				monotonic = monotonic && ( ( p.p1.y - p.p0.y ) * ( p.p2.y - p.p1.y ) >= 0.0 );
			const std::vector< Polygon > shape = specGlyph( c, "regular" ).polygons;
			for( int i = 0; i < 2000; ++i )
			{
				const Vec2 q { ux( random ), uy( random ) };
				const double sd = polygonSd( shape, q );
				if( ( WindingAt( o.pieces, q ) != 0 ) != ( sd > 0 ) && std::fabs( sd ) > 1e-9 )
					++wrongInside;
				worstDistance = std::max( worstDistance, std::fabs( BoundaryDistance( o.pieces, q ) - std::fabs( sd ) ) );
			}
		}
		Check( monotonic, "every piece of every fixture glyph is monotonic in y" );
		Check( wrongInside == 0, fmt( "the winding agrees with the polygons at 12000 points (%d wrong)", wrongInside ) );
		Check( worstDistance < 1e-9, fmt( "the boundary distance agrees with the polygons' (%.1e units)", worstDistance ) );
	}
	{
		//The disc: its quadratics, measured against brute force.
		const GlyphOutline o = fixtureOutline( U'C' );
		std::mt19937 random( 5 );
		std::uniform_real_distribution< double > u( 0.0, 800.0 );
		double worst = 0.0;
		for( int i = 0; i < 300; ++i )
		{
			const Vec2 q { u( random ), u( random ) - 100.0 };
			double brute = 1e300;
			for( const Piece& p : o.pieces )
				for( int k = 0; k <= 20000; ++k )
				{
					const Vec2 e = Evaluate( p, k / 20000.0 );
					brute        = std::min( brute, std::hypot( e.x - q.x, e.y - q.y ) );
				}
			worst = std::max( worst, std::fabs( BoundaryDistance( o.pieces, q ) - brute ) );
		}
		Check( worst < 0.01, fmt( "the disc's distance against 20 000 samples a piece: %.2e units", worst ) );
		Check( std::fabs( o.x0 - 100 ) < 1e-9 && std::fabs( o.x1 - 700 ) < 1e-9 && std::fabs( o.y0 ) < 1e-9 && std::fabs( o.y1 - 600 ) < 1e-9,
		       fmt( "the disc's ink box is its extremes: %.3f..%.3f x %.3f..%.3f", o.x0, o.x1, o.y0, o.y1 ) );
	}
	{
		const GlyphOutline resolved = fixtureOutline( U'X' ), raw = fixtureOutline( U'X', false );
		Check( resolved.internal == 4 && raw.internal == 0, fmt( "X: 4 internal pieces resolved (%d), none unresolved (%d)", resolved.internal, raw.internal ) );
		const GlyphOutline ring = fixtureOutline( U'O' );
		Check( ring.internal == 0, "O's hole is boundary, not overlap" );
	}
	{
		auto same = []( const std::u32string& a, const std::u32string& b ) { return a == b; };
		Check( same( DecodeText( "a\\nb" ), U"a\nb" ) && same( DecodeText( "a\\\\nb" ), U"a\\nb" ) && same( DecodeText( "a\r\nb\rc" ), U"a\nb\nc" )
		           && same( DecodeText( "a\tb" ), U"a b" ) && same( DecodeText( "a\x01" "b" ), U"ab" ),
		       "escapes: \\n breaks, \\\\ is a backslash, CR LF and CR are LF, a tab a space, controls dropped" );
		Check( same( DecodeText( "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" ), U"é€\U0001F600" ), "UTF-8: two, three and four bytes" );
		Check( same( DecodeText( "a\xFF" "b\xC3" ), U"a�b�" ) && same( DecodeText( "\xC0\xAF" ), U"��" ),
		       "malformed UTF-8 is U+FFFD, overlong forms included" );
	}
	{
		//The shader pieces under MSVC's limit, and the parameter table.
		size_t longest = 0;
		for( const char* piece : { shaders::kCommon, shaders::kGlyphVertex, shaders::kGlyphFragment, shaders::kQuadVertex, shaders::kCompositeFragment } )
			longest = std::max( longest, std::strlen( piece ) );
		Check( longest < 15000, fmt( "the longest shader piece is %zu bytes (MSVC stops at 16 380)", longest ) );
		KnockoutPlugin plugin;
		std::set< std::string > names;
		bool ranges = true;
		for( const NamedParameter& p : listParameters( plugin ) )
		{
			names.insert( p.name );
			if( p.type == FF_TYPE_STANDARD )
				ranges = ranges && p.value >= 0.0f && p.value <= 1.0f;
		}
		Check( names.size() == plugin.ParamCount(), fmt( "%zu parameters, every name different", names.size() ) );
		Check( ranges, "every ranged default inside 0..1" );
		Check( plugin.SetTextParameter( PT_ABOUT_TEXT, "anything" ) == FF_SUCCESS, "the About line accepts a text (a host pushes defaults back)" );
	}
}

//===========================================================================
// --pipe: raw RGBA, top row first.
//===========================================================================
int runPipe( int width, int height, const std::string& scriptPath, const std::vector< std::string >& settings, const std::vector< Timed >& timed )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	if( !applyAll( rig.plugin, settings ) )
		return 2;
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}
	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	Bytes out( in.size() );
	for( int index = 0;; ++index )
	{
		size_t filled = 0;
		while( filled < in.size() )
		{
			const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
			if( got <= 0 )
				break;
			filled += static_cast< size_t >( got );
		}
		if( filled < in.size() )
			break;
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
		glBindTexture( GL_TEXTURE_2D, rig.clipTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		for( const auto& entry : automation )
			rig.plugin.SetFloatParameter( entry.first, valueAt( entry.second.track, index, entry.second.ramp ) );
		for( const Timed& t : timed )
			if( t.frame == index )
			{
				std::string error;
				applySetting( rig.plugin, t.setting, error );
			}
		if( !rig.Render( 1 ) )
			return 1;
		const Floats frame = rig.Output();
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
				for( int c = 0; c < 4; ++c )
				{
					const float v = frame[ ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4 + c ];
					out[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ] = static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
				}
		if( std::fwrite( out.data(), 1, out.size(), stdout ) != out.size() )
			return 1;
	}
	return 0;
}

void usage()
{
	std::fprintf( stderr, "usage: kotest [--system-fonts] [--size WxH] [--set Name=Value]... [--at N Name=Value]... [--frames N]\n"
	                      "              [--out file.png | --pipe | --list] [--script cues]\n"
	                      "       kotest --offline | --all | --negative | --coverage --zoom --edge --overlap --winding --layout --fit\n"
	                      "              --transform --reveal --composite --state --resize --fonts --text --stb --controls\n"
	                      "       kotest --system-fonts --real | --bench\n" );
}
} // namespace

int main( int argc, char** argv )
{
	//A reader that hangs up ends the run with exit 1, not SIGPIPE's 141.
	std::signal( SIGPIPE, SIG_IGN );
	bool list = false, pipe = false, offline = false, all = false, negative = false;
	int width = 1280, height = 720, frames = 1;
	std::string outPath, scriptPath;
	std::vector< std::string > settings, checks;
	std::vector< Timed > timed;
	const char* const kChecks[] = { "--coverage", "--zoom",  "--edge",   "--overlap", "--winding", "--layout", "--fit",      "--transform", "--reveal",
		                            "--composite", "--state", "--resize", "--fonts",   "--text",    "--stb",    "--controls", "--real",      "--bench" };
	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		auto next           = [ & ]() -> std::string {
            if( i + 1 >= argc )
            {
                usage();
                std::exit( 2 );
            }
            return argv[ ++i ];
		};
		if( a == "--list" )
			list = true;
		else if( a == "--pipe" )
			pipe = true;
		else if( a == "--offline" )
			offline = true;
		else if( a == "--all" )
			all = true;
		else if( a == "--negative" )
			negative = true;
		else if( a == "--quiet" )
			g_quiet = true;
		else if( a == "--system-fonts" )
			g_systemFonts = true;
		else if( a == "--out" )
			outPath = next();
		else if( a == "--script" )
			scriptPath = next();
		else if( a == "--set" )
			settings.push_back( next() );
		else if( a == "--at" )
		{
			const int f = std::atoi( next().c_str() );
			timed.push_back( { f, next() } );
		}
		else if( a == "--frames" )
			frames = std::max( 1, std::atoi( next().c_str() ) );
		else if( a == "--size" )
		{
			const std::string s = next();
			if( std::sscanf( s.c_str(), "%dx%d", &width, &height ) != 2 || width <= 0 || height <= 0 )
			{
				usage();
				return 2;
			}
		}
		else if( std::find_if( std::begin( kChecks ), std::end( kChecks ), [ & ]( const char* c ) { return a == c; } ) != std::end( kChecks ) )
			checks.push_back( a );
		else
		{
			usage();
			return 2;
		}
	}

	//Before anything scans: the fixtures, unless this run is about the OS's.
	if( !g_systemFonts && std::getenv( "KNOCKOUT_FONT_DIR" ) == nullptr )
		setenv( "KNOCKOUT_FONT_DIR", KNOCKOUT_FIXTURES, 1 );

	if( offline )
	{
		checkOffline();
		std::printf( "\n  %s (%d checks)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks );
		return g_failures == 0 ? 0 : 1;
	}

	//The parameters need no GL either: a plugin is constructed, never
	//initialised.
	if( list )
	{
		KnockoutPlugin plugin;
		for( const NamedParameter& p : listParameters( plugin ) )
		{
			const char* type = p.type == FF_TYPE_OPTION    ? "option"
			                   : p.type == FF_TYPE_EVENT   ? "event"
			                   : p.type == FF_TYPE_BOOLEAN ? "bool"
			                   : p.type == FF_TYPE_TEXT    ? "text"
			                   : p.type == FF_TYPE_FILE    ? "file"
			                   : p.type == FF_TYPE_RED || p.type == FF_TYPE_GREEN || p.type == FF_TYPE_BLUE ? "colour"
			                                                                                                  : "standard";
			std::printf( "%3u  %-14s %-9s %-8s", p.index, p.name.c_str(), plugin.GetParamGroup( p.index ).c_str(), type );
			if( isTextType( p.type ) )
				std::printf( " '%s'", plugin.TextValue( p.index ).c_str() );
			else
				std::printf( " %g", p.value );
			if( p.type == FF_TYPE_OPTION )
			{
				const unsigned int count = plugin.GetNumParamElements( p.index );
				std::printf( "  [" );
				for( unsigned int e = 0; e < count && e < 12; ++e )
					std::printf( "%s%s", e ? ", " : "", plugin.GetParamElementName( p.index, e ) );
				std::printf( "%s]", count > 12 ? fmt( ", ... %u in all", count ).c_str() : "" );
			}
			std::printf( "\n" );
		}
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "no GL 4.1 context\n" );
		return 1;
	}

	if( negative )
	{
		//Each wrong model must fail at least one check of its own.
		struct Wrong
		{
			const char* what;
			std::function< void() > run;
		};
		const Wrong wrongs[] = {
			{ "inside by even-odd, not non-zero", [] { checkWinding( KnockoutPlugin::kTestEvenOdd ); } },
			{ "the distance measured to internal pieces too", [] { checkOverlap( KnockoutPlugin::kTestAllPieces ); } },
			{ "overlaps left unresolved", [] { checkOverlap( KnockoutPlugin::kTestOverlaps ); } },
			{ "a two-pixel ramp", [] { checkCoverage( KnockoutPlugin::kTestWideRamp ); } },
			{ "the face's kerning ignored", [] { checkLayout( KnockoutPlugin::kTestNoKerning ); } },
			{ "Grow turned inward", [] { checkEdge( KnockoutPlugin::kTestGrowInward ); } },
			{ "each glyph's centre in float", [] { checkZoom( KnockoutPlugin::kTestFloatCentre ); } },
			{ "the harness's rotation turned the other way", [] {
				 g_wrongRotation = true;
				 checkTransform( 0 );
				 g_wrongRotation = false;
			 } },
		};
		int caught          = 0;
		const bool wasQuiet = g_quiet;
		for( const Wrong& wrong : wrongs )
		{
			const int before = g_failures;
			g_quiet          = true;
			wrong.run();
			g_quiet           = wasQuiet;
			const bool failed = g_failures > before;
			g_failures        = before;
			caught += failed ? 1 : 0;
			std::printf( "  %s  %s\n", failed ? "ok  " : "MISS", wrong.what );
		}
		const int count = static_cast< int >( sizeof( wrongs ) / sizeof( wrongs[ 0 ] ) );
		std::printf( "\n  %s: %d of %d wrong models caught\n", caught == count ? "PASS" : "FAIL", caught, count );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return caught == count ? 0 : 1;
	}

	if( all || !checks.empty() )
	{
		auto wants = [ & ]( const char* name ) { return all || std::find( checks.begin(), checks.end(), name ) != checks.end(); };
		if( wants( "--coverage" ) )
			checkCoverage( 0 );
		if( wants( "--zoom" ) )
			checkZoom( 0 );
		if( wants( "--edge" ) )
			checkEdge( 0 );
		if( wants( "--overlap" ) )
			checkOverlap( 0 );
		if( wants( "--winding" ) )
			checkWinding( 0 );
		if( wants( "--layout" ) )
			checkLayout( 0 );
		if( wants( "--fit" ) )
			checkFit( 0 );
		if( wants( "--transform" ) )
			checkTransform( 0 );
		if( wants( "--reveal" ) )
			checkReveal( 0 );
		if( wants( "--composite" ) )
			checkComposite( 0 );
		if( wants( "--state" ) )
			checkState( 0 );
		if( wants( "--resize" ) )
			checkResize( 0 );
		if( wants( "--fonts" ) && !g_systemFonts )
			checkFonts( 0 );
		if( wants( "--text" ) )
			checkText( 0 );
		if( wants( "--stb" ) && !g_systemFonts )
			checkStb( 0 );
		if( wants( "--controls" ) && !g_systemFonts )
			checkControls( 0 );
		//Against this Mac's fonts, and the bench: asked for by name only.
		if( std::find( checks.begin(), checks.end(), "--real" ) != checks.end() )
			checkReal( 0 );
		if( std::find( checks.begin(), checks.end(), "--bench" ) != checks.end() )
			runBench();
		std::printf( "\n  %s (%d checks, %d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return g_failures == 0 ? 0 : 1;
	}

	int status = 0;
	if( pipe )
		status = runPipe( width, height, scriptPath, settings, timed );
	else
	{
		const Floats card = colourCard( width, height );
		Rig rig;
		if( !rig.Init( width, height, &card ) || !applyAll( rig.plugin, settings ) )
			status = 1;
		else
		{
			std::map< unsigned int, Cue > automation;
			std::string error;
			if( !scriptPath.empty() && !bindScript( rig.plugin, scriptPath, automation, error ) )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				status = 2;
			}
			for( int f = 0; status == 0 && f < frames; ++f )
			{
				for( const auto& entry : automation )
					rig.plugin.SetFloatParameter( entry.first, valueAt( entry.second.track, f, entry.second.ramp ) );
				for( const Timed& t : timed )
					if( t.frame == f && !applySetting( rig.plugin, t.setting, error ) )
						std::fprintf( stderr, "--at %d %s: %s\n", t.frame, t.setting.c_str(), error.c_str() );
				if( !rig.Render( 1 ) )
					status = 1;
			}
			if( status == 0 )
			{
				const FrameState& s = rig.plugin.LastFrame();
				std::printf( "%dx%d: %s %s (%d), %d glyphs on %d lines, %d pieces, %d drawn; %.4f px a unit\n", width, height, s.family.c_str(), s.style.c_str(),
				             s.weight, s.glyphs, s.lines, s.pieces, s.drawn, s.unitPx );
				if( !outPath.empty() && !writePng( outPath, width, height, rig.Output() ) )
				{
					std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
					status = 1;
				}
			}
		}
	}
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return status;
}
