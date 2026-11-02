#include "Knockout.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

using namespace ffglex;

namespace knockout
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
/// The most glyphs a setting draws: a paragraph, not a book. A host's text
/// field will take more, and a mask of more is unreadable anyway.
constexpr size_t kMaxCodepoints = 4096;

const char* glStringOrUnknown( GLenum name )
{
	const GLubyte* s = glGetString( name );
	return s ? reinterpret_cast< const char* >( s ) : "unknown";
}

/// PCG's output mix, for the Random reveal order's shuffle.
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

void createBufferTexture( GLuint& buffer, GLuint& texture )
{
	GLint previous = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_BUFFER, &previous );
	glGenBuffers( 1, &buffer );
	glBindBuffer( GL_TEXTURE_BUFFER, buffer );
	const float zero[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glBufferData( GL_TEXTURE_BUFFER, sizeof( zero ), zero, GL_DYNAMIC_DRAW );
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_BUFFER, texture );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, buffer );
	glBindTexture( GL_TEXTURE_BUFFER, static_cast< GLuint >( previous ) );
	glBindBuffer( GL_TEXTURE_BUFFER, 0 );
}

void fillBuffer( GLuint buffer, const std::vector< float >& data, GLenum usage )
{
	glBindBuffer( GL_TEXTURE_BUFFER, buffer );
	glBufferData( GL_TEXTURE_BUFFER, static_cast< GLsizeiptr >( data.size() * sizeof( float ) ), data.data(), usage );
	glBindBuffer( GL_TEXTURE_BUFFER, 0 );
}
} // namespace

static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

Vec2 FrameState::ScreenFromLayout( Vec2 l ) const
{
	const double ix = 0.5 * ( inkX0 + inkX1 ), iy = 0.5 * ( inkY0 + inkY1 );
	const double c = std::cos( rotation ), s = std::sin( rotation );
	const double dx = ( l.x - ix ) * fitScale, dy = ( l.y - iy ) * fitScale;
	const double px = 0.5 * width + offsetX + c * dx - s * dy;
	const double py = 0.5 * height + offsetY + s * dx + c * dy;
	return { zoomX + zoom * ( px - zoomX ), zoomY + zoom * ( py - zoomY ) };
}

//---------------------------------------------------------------------------
KnockoutPlugin::KnockoutPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//-------------------------------------------------------------------
	// Defaults: the word, in the heaviest face of the house default, as
	// large as fits the frame, the clip inside it and nothing outside.
	//-------------------------------------------------------------------
	const std::vector< FontFamily >& families = InstalledFamilies();
	const int family                          = DefaultFamily();
	params[ PT_FONT ]          = FamilyValue( family );
	params[ PT_WEIGHT ]        = static_cast< float >( kWeightCount - 1 );
	params[ PT_ITALIC ]        = 0.0f;
	params[ PT_FIT ]           = static_cast< float >( Fit::Frame );
	params[ PT_SIZE ]          = ParamFromSize( 0.35 );
	params[ PT_MARGIN ]        = ParamFromMargin( 0.06 );
	params[ PT_ALIGN ]         = static_cast< float >( Align::Centre );
	params[ PT_TRACKING ]      = ParamFromTracking( 0.0 );
	params[ PT_LINE_SPACING ]  = ParamFromLineSpacing( 1.0 );
	params[ PT_POSITION_X ]    = 0.5f;
	params[ PT_POSITION_Y ]    = 0.5f;
	params[ PT_ROTATION ]      = 0.5f;
	params[ PT_ZOOM ]          = 0.0f;
	params[ PT_ZOOM_X ]        = 0.5f;
	params[ PT_ZOOM_Y ]        = 0.5f;
	params[ PT_GROW ]          = ParamFromGrow( 0.0 );
	params[ PT_FEATHER ]       = 0.0f;
	params[ PT_OUTLINE ]       = 0.0f;
	params[ PT_REVEAL ]        = 1.0f;
	params[ PT_REVEAL_SPREAD ] = 0.0f;
	params[ PT_REVEAL_ORDER ]  = static_cast< float >( Order::Forward );
	params[ PT_MASK ]          = static_cast< float >( Mask::Inside );
	params[ PT_BACKGROUND ]    = static_cast< float >( Background::Transparent );
	params[ PT_COLOUR_R ]      = 1.0f;
	params[ PT_COLOUR_G ]      = 0.6f;
	params[ PT_COLOUR_B ]      = 0.1f;
	params[ PT_MIX ]           = 1.0f;

	auto option = [ this ]( unsigned int id, int count, auto nameOf ) {
		SetOptionParamInfo( id, NameOf( id ), static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameOf( i ), static_cast< float >( i ) );
	};
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		switch( id )
		{
		case PT_TEXT:
			SetParamInfo( id, NameOf( id ), FF_TYPE_TEXT, "KNOCKOUT" );
			break;
		case PT_FONT:
			//The dropdown is this machine's families, so its elements carry
			//FamilyValue, not their position: the host saves the value, and
			//a position is a different family on the next rig. The host never
			//reads back the Font Name we fill in, so this is what travels.
			if( families.empty() )
				option( id, 1, []( int ) { return "None"; } );
			else
			{
				SetOptionParamInfo( id, NameOf( id ), static_cast< unsigned int >( families.size() ), params[ id ] );
				for( size_t i = 0; i < families.size(); ++i )
					SetParamElementInfo( id, static_cast< unsigned int >( i ), families[ i ].name.c_str(), FamilyValue( static_cast< int >( i ) ) );
			}
			break;
		case PT_FONT_NAME:
			SetParamInfo( id, NameOf( id ), FF_TYPE_TEXT, "" );
			break;
		case PT_FONT_FILE:
			SetFileParamInfo( id, NameOf( id ), { "ttf", "otf", "ttc", "otc" }, "" );
			break;
		case PT_WEIGHT:
			option( id, kWeightCount, []( int i ) { return WeightName( i ); } );
			break;
		case PT_FIT:
			option( id, static_cast< int >( Fit::Count ), []( int i ) { return FitName( static_cast< Fit >( i ) ); } );
			break;
		case PT_ALIGN:
			option( id, static_cast< int >( Align::Count ), []( int i ) { return AlignName( i ); } );
			break;
		case PT_REVEAL_ORDER:
			option( id, static_cast< int >( Order::Count ), []( int i ) { return OrderName( static_cast< Order >( i ) ); } );
			break;
		case PT_MASK:
			option( id, static_cast< int >( Mask::Count ), []( int i ) { return MaskName( static_cast< Mask >( i ) ); } );
			break;
		case PT_BACKGROUND:
			option( id, static_cast< int >( Background::Count ),
			        []( int i ) { return BackgroundName( static_cast< Background >( i ) ); } );
			break;
		case PT_ITALIC:
			SetParamInfo( id, NameOf( id ), FF_TYPE_BOOLEAN, params[ id ] > 0.5f );
			break;
		case PT_COLOUR_R:
			SetParamInfo( id, NameOf( id ), FF_TYPE_RED, params[ id ] );
			break;
		case PT_COLOUR_G:
			SetParamInfo( id, NameOf( id ), FF_TYPE_GREEN, params[ id ] );
			break;
		case PT_COLOUR_B:
			SetParamInfo( id, NameOf( id ), FF_TYPE_BLUE, params[ id ] );
			break;
		case PT_ABOUT_TEXT:
			SetParamInfo( id, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
			break;
		default:
			if( id > PT_ABOUT_TEXT )
				SetParamInfo( id, stoatworks::about::buttons()[ id - PT_ABOUT_TEXT - 1 ].label, FF_TYPE_EVENT, false );
			else
				SetParamInfo( id, NameOf( id ), FF_TYPE_STANDARD, params[ id ] );
			break;
		}
		SetParamGroup( id, GroupOf( id ) );
	}
}

//---------------------------------------------------------------------------
FFResult KnockoutPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	using namespace shaders;
	const std::string glyphVertex = std::string( kVersion ) + kGlyphVertex;
	const std::string quadVertex  = std::string( kVersion ) + kQuadVertex;
	if( !glyphShader.Compile( glyphVertex.c_str(), Assemble( { kGlyphFragment } ).c_str() ) )
	{
		diag::error( "the glyph shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Knockout: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !compositeShader.Compile( quadVertex.c_str(), Assemble( { kCompositeFragment } ).c_str() ) )
	{
		diag::error( "the composite shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Knockout: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}
	//A core profile draws nothing without a vertex array bound, even with no
	//attributes: the glyph pass makes its quads from gl_VertexID.
	GLint previousVAO = 0;
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &previousVAO );
	glGenVertexArrays( 1, &emptyVAO );
	glBindVertexArray( static_cast< GLuint >( previousVAO ) );
	createBufferTexture( pieceBuffer, pieceTexture );
	createBufferTexture( instanceBuffer, instanceTexture );
	uploadedOutlines = 0;
	piecesDirty      = true;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
// The face, the setting, the table of pieces.
//---------------------------------------------------------------------------
void KnockoutPlugin::resolveFont()
{
	std::string file, name;
	bool nameSet = false, indexMoved = false;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		file           = fontFile;
		name           = fontName;
		nameSet        = fontNameSet;
		indexMoved     = fontIndexMoved;
		fontNameSet    = false;
		fontIndexMoved = false;
	}
	const std::vector< FontFamily >& families = InstalledFamilies();
	const std::vector< FontFace >& faces      = InstalledFaces();
	const int weight                          = WeightOf( OptionIndex( params[ PT_WEIGHT ], kWeightCount ) );
	const bool italic                         = params[ PT_ITALIC ] > 0.5f;

	bool loaded = false, fromFile = false;
	//An explicit file beats everything: the escape hatch for a face that is
	//not installed, and someone who picked one meant it.
	if( !file.empty() )
	{
		loaded = fromFile = font.Load( file, 0 );
		if( !loaded )
			diag::warn( "Font File will not load: " + file + "; using the Font list" );
	}
	if( !loaded )
	{
		//The dropdown holds a family's FamilyValue, Font Name the family as
		//typed. A composition restores both before the next frame, and a
		//typed name wins. Only the dropdown moving on its own picks from it.
		int family        = -1;
		bool typedMissing = false;
		if( indexMoved && !nameSet )
			family = FamilyOfValue( params[ PT_FONT ] );
		else
		{
			family = FindFamily( name );
			if( family < 0 && !name.empty() )
			{
				typedMissing = true;
				diag::warn( "font '" + name + "' is not installed here; using the Font list" );
			}
			if( family < 0 )
				family = FamilyOfValue( params[ PT_FONT ] );
		}
		if( family < 0 && !families.empty() && !typedMissing )
			diag::warn( "the Font list's value " + std::to_string( std::lround( params[ PT_FONT ] ) ) + " is no family installed here; using the default" );
		if( family < 0 || family >= static_cast< int >( families.size() ) )
			family = DefaultFamily();
		for( int attempt = 0; attempt < 2 && !loaded && family >= 0; ++attempt )
		{
			const int face = MatchFace( family, weight, italic );
			if( face >= 0 )
				loaded = font.Load( faces[ static_cast< size_t >( face ) ].path, faces[ static_cast< size_t >( face ) ].collectionIndex );
			if( !loaded )
			{
				diag::warn( "the face chosen from " + families[ static_cast< size_t >( family ) ].name + " will not load; trying the default" );
				family = DefaultFamily();
			}
		}
		if( loaded && family >= 0 )
		{
			params[ PT_FONT ] = FamilyValue( family );
			//The field shows the family in use -- to us. Arena keeps its own
			//copy and saves that, so a family picked from the list travels as
			//its FamilyValue alone. A typed name that was not found is kept as
			//typed so the same composition finds it on a machine that has it.
			if( !typedMissing )
			{
				std::lock_guard< std::mutex > lock( textMutex );
				fontName = families[ static_cast< size_t >( family ) ].name;
			}
		}
	}
	if( !loaded )
		diag::error( "no font would load: nothing to draw" );

	OutlineOptions options;
	options.resolveOverlaps = ( testFlags & kTestOverlaps ) == 0;
	setter.UseFont( loaded ? &font : nullptr, options );
	frameState.family     = loaded ? font.Face().family : std::string();
	frameState.style      = loaded ? font.Face().style : std::string();
	frameState.weight     = loaded ? font.Face().weight : 0;
	frameState.italic     = loaded && font.Face().italic;
	frameState.fromFile   = fromFile;
	frameState.unitsPerEm = loaded ? font.UnitsPerEm() : 1000;
	uploadedOutlines      = 0;
	piecesDirty           = true;
	layoutDirty           = true;
	fontDirty             = false;
}

void KnockoutPlugin::relayout()
{
	SetOptions options;
	options.trackingEm  = TrackingFromParam( params[ PT_TRACKING ] );
	options.lineSpacing = LineSpacingFromParam( params[ PT_LINE_SPACING ] );
	options.align       = static_cast< Align >( OptionIndex( params[ PT_ALIGN ], static_cast< int >( Align::Count ) ) );
	options.kerning     = ( testFlags & kTestNoKerning ) == 0;
	std::u32string codepoints = decoded;
	if( codepoints.size() > kMaxCodepoints )
	{
		diag::warn( "the text is " + std::to_string( codepoints.size() ) + " characters; drawing the first " + std::to_string( kMaxCodepoints ) );
		codepoints.resize( kMaxCodepoints );
	}
	setting           = setter.Set( codepoints, options );
	frameState.glyphs = static_cast< int >( setting.glyphs.size() );
	frameState.lines  = setting.lines;
	layoutDirty       = false;
}

void KnockoutPlugin::uploadPieces()
{
	const std::vector< GlyphOutline >& outlines = setter.Outlines();
	pieceFirst.assign( outlines.size(), 0 );
	pieceCount.assign( outlines.size(), 0 );
	std::vector< float > data;
	int at = 0;
	for( size_t i = 0; i < outlines.size(); ++i )
	{
		pieceFirst[ i ] = at;
		pieceCount[ i ] = static_cast< int >( outlines[ i ].pieces.size() );
		for( const Piece& p : outlines[ i ].pieces )
		{
			const float flags = static_cast< float >( ( p.line ? 1 : 0 ) | ( p.boundary ? 2 : 0 ) );
			data.insert( data.end(), { static_cast< float >( p.p0.x ), static_cast< float >( p.p0.y ), static_cast< float >( p.p1.x ),
			                           static_cast< float >( p.p1.y ), static_cast< float >( p.p2.x ), static_cast< float >( p.p2.y ), flags, 0.0f } );
			++at;
		}
	}
	GLint limit = 65536;
	glGetIntegerv( GL_MAX_TEXTURE_BUFFER_SIZE, &limit );
	if( static_cast< long >( at ) * 2 > limit )
	{
		diag::error( "the outlines need " + std::to_string( at * 2 ) + " texels and this GL allows " + std::to_string( limit ) );
		std::fill( pieceCount.begin(), pieceCount.end(), 0 );
		data.clear();
	}
	if( data.empty() )
		data.assign( 8, 0.0f );
	fillBuffer( pieceBuffer, data, GL_STATIC_DRAW );
	uploadedOutlines  = static_cast< int >( outlines.size() );
	frameState.pieces = at;
	piecesDirty       = false;
}

void KnockoutPlugin::SetTestFlagsForTest( int flags )
{
	if( ( flags & ( kTestOverlaps | kTestNoKerning ) ) != ( testFlags & ( kTestOverlaps | kTestNoKerning ) ) )
		fontDirty = true;
	testFlags = flags;
}

//---------------------------------------------------------------------------
FFResult KnockoutPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct* input = pgl->inputTextures[ 0 ];

	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	const int width           = static_cast< int >( input->Width );
	const int height          = static_cast< int >( input->Height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;
	glDisable( GL_BLEND );

	{
		std::lock_guard< std::mutex > lock( textMutex );
		if( textDirty )
		{
			decoded     = DecodeText( text );
			textDirty   = false;
			layoutDirty = true;
		}
	}
	if( fontDirty )
		resolveFont();
	if( layoutDirty )
		relayout();
	if( piecesDirty || uploadedOutlines != static_cast< int >( setter.Outlines().size() ) )
		uploadPieces();

	//-------------------------------------------------------------------
	// The transform, in double. Screen = Zc + zoom (F + offset + s R (L - I)
	// - Zc): the ink's centre I to the frame's centre F, scaled by the fit,
	// turned, moved by Position, then the whole picture zoomed about Zc.
	//-------------------------------------------------------------------
	FrameState& f   = frameState;
	f.width         = width;
	f.height        = height;
	const double W  = width, H = height;
	const double em = f.unitsPerEm;
	f.inkX0         = setting.x0;
	f.inkY0         = setting.y0;
	f.inkX1         = setting.x1;
	f.inkY1         = setting.y1;
	const double inkW = std::max( setting.x1 - setting.x0, 1.0 ), inkH = std::max( setting.y1 - setting.y0, 1.0 );
	const double room = 1.0 - 2.0 * MarginFromParam( params[ PT_MARGIN ] );
	switch( static_cast< Fit >( OptionIndex( params[ PT_FIT ], static_cast< int >( Fit::Count ) ) ) )
	{
	case Fit::Frame: f.fitScale = std::min( W * room / inkW, H * room / inkH ); break;
	case Fit::Width: f.fitScale = W * room / inkW; break;
	case Fit::Height: f.fitScale = H * room / inkH; break;
	default: f.fitScale = SizeFromParam( params[ PT_SIZE ] ) * H / em; break;
	}
	f.fitScale  = std::max( f.fitScale, 1.0e-9 );
	f.zoom      = ZoomFromParam( params[ PT_ZOOM ] );
	f.rotation  = RotationFromParam( params[ PT_ROTATION ] ) * kPi / 180.0;
	f.offsetX   = PositionFromParam( params[ PT_POSITION_X ] ) * W;
	f.offsetY   = PositionFromParam( params[ PT_POSITION_Y ] ) * H;
	f.zoomX     = std::clamp( static_cast< double >( params[ PT_ZOOM_X ] ), 0.0, 1.0 ) * W;
	f.zoomY     = std::clamp( static_cast< double >( params[ PT_ZOOM_Y ] ), 0.0, 1.0 ) * H;
	f.unitPx    = f.fitScale * f.zoom;
	f.rampPx    = ( ( testFlags & kTestWideRamp ) != 0 ? 2.0 : 1.0 ) + FeatherFromParam( params[ PT_FEATHER ] ) * H;
	f.growPx    = GrowFromParam( params[ PT_GROW ] ) * em * f.unitPx * ( ( testFlags & kTestGrowInward ) != 0 ? -1.0 : 1.0 );
	f.outlinePx = OutlineFromParam( params[ PT_OUTLINE ] ) * em * f.unitPx;
	f.reachPx   = std::fabs( f.growPx ) + ( f.outlinePx > 0.0 ? 0.5 * f.outlinePx : 0.0 ) + 0.5 * f.rampPx + 1.0;
	const double reach = f.reachPx / f.unitPx;

	const double c = std::cos( f.rotation ), s = std::sin( f.rotation );
	const double fx = 0.5 * W, fy = 0.5 * H;
	const double ix = 0.5 * ( setting.x0 + setting.x1 ), iy = 0.5 * ( setting.y0 + setting.y1 );
	//Where the frame's centre is in the setting: the inverse of the above.
	const double ux = ( fx - f.zoomX ) / f.zoom + f.zoomX - fx - f.offsetX;
	const double uy = ( fy - f.zoomY ) / f.zoom + f.zoomY - fy - f.offsetY;
	const double centreX = ix + ( c * ux + s * uy ) / f.fitScale;
	const double centreY = iy + ( -s * ux + c * uy ) / f.fitScale;

	//-------------------------------------------------------------------
	// Reveal: each glyph's rank in the order, and its alpha.
	//-------------------------------------------------------------------
	const int n = static_cast< int >( setting.glyphs.size() );
	std::vector< double > rank( static_cast< size_t >( n ), 0.0 );
	const Order order = static_cast< Order >( OptionIndex( params[ PT_REVEAL_ORDER ], static_cast< int >( Order::Count ) ) );
	if( order == Order::Random )
	{
		std::vector< int > shuffled( static_cast< size_t >( n ) );
		std::iota( shuffled.begin(), shuffled.end(), 0 );
		for( int i = n - 1; i > 0; --i )
			std::swap( shuffled[ static_cast< size_t >( i ) ], shuffled[ pcg( static_cast< uint32_t >( i ) * 0x9E3779B9u ) % static_cast< uint32_t >( i + 1 ) ] );
		for( int i = 0; i < n; ++i )
			rank[ static_cast< size_t >( shuffled[ static_cast< size_t >( i ) ] ) ] = i;
	}
	else
		for( int i = 0; i < n; ++i )
			rank[ static_cast< size_t >( i ) ] = order == Order::Forward ? i : order == Order::Backward ? n - 1 - i : std::fabs( i - 0.5 * ( n - 1 ) );
	double lowest = 0.0, highest = 0.0;
	if( n > 0 )
	{
		lowest  = *std::min_element( rank.begin(), rank.end() );
		highest = *std::max_element( rank.begin(), rank.end() ) - lowest;
	}
	const double steps  = highest + 1.0;
	const double spread = 1.0 + std::clamp( static_cast< double >( params[ PT_REVEAL_SPREAD ] ), 0.0, 1.0 ) * ( steps - 1.0 );
	const double reveal = std::clamp( static_cast< double >( params[ PT_REVEAL ] ), 0.0, 1.0 );
	f.alphas.assign( static_cast< size_t >( n ), 0.0 );

	//-------------------------------------------------------------------
	// One instance per glyph on the screen.
	//-------------------------------------------------------------------
	instanceData.clear();
	f.drawn = f.culled = 0;
	for( int i = 0; i < n; ++i )
	{
		const PlacedGlyph& g = setting.glyphs[ static_cast< size_t >( i ) ];
		const double alpha   = std::clamp( ( reveal * ( steps - 1.0 + spread ) - ( rank[ static_cast< size_t >( i ) ] - lowest ) ) / spread, 0.0, 1.0 );
		f.alphas[ static_cast< size_t >( i ) ] = alpha;
		if( alpha <= 0.0 || static_cast< size_t >( g.glyph ) >= pieceCount.size() || pieceCount[ static_cast< size_t >( g.glyph ) ] == 0 )
		{
			++f.culled;
			continue;
		}
		const GlyphOutline& o = setter.Outlines()[ static_cast< size_t >( g.glyph ) ];
		//The frame's centre in the glyph's own units, in double, then split
		//for the shader into an anchor (that centre rounded to a float) and
		//what the rounding left (small, so exact enough in float). The shader
		//works relative to the anchor, outline and pixel alike, so a pixel's
		//precision depends on how far it is from the frame's centre on the
		//screen, not on how far the zoom has gone or how long the line is.
		const double cgx = centreX - g.x, cgy = centreY - g.y;
		double anchorX = static_cast< float >( cgx ), anchorY = static_cast< float >( cgy );
		double restX = cgx - anchorX, restY = cgy - anchorY;
		if( ( testFlags & kTestFloatCentre ) != 0 )
		{
			anchorX = anchorY = 0.0;
			restX           = static_cast< double >( static_cast< float >( centreX ) - static_cast< float >( g.x ) );
			restY           = static_cast< double >( static_cast< float >( centreY ) - static_cast< float >( g.y ) );
		}
		double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
		for( int corner = 0; corner < 4; ++corner )
		{
			const double lx = ( corner & 1 ? o.x1 + reach : o.x0 - reach ) - cgx;
			const double ly = ( corner & 2 ? o.y1 + reach : o.y0 - reach ) - cgy;
			const double sx = fx + f.unitPx * ( c * lx - s * ly );
			const double sy = fy + f.unitPx * ( s * lx + c * ly );
			x0 = std::min( x0, sx );
			y0 = std::min( y0, sy );
			x1 = std::max( x1, sx );
			y1 = std::max( y1, sy );
		}
		x0 = std::max( std::floor( x0 ), 0.0 );
		y0 = std::max( std::floor( y0 ), 0.0 );
		x1 = std::min( std::ceil( x1 ), W );
		y1 = std::min( std::ceil( y1 ), H );
		if( x1 <= x0 || y1 <= y0 )
		{
			++f.culled;
			continue;
		}
		instanceData.insert( instanceData.end(),
		                     { static_cast< float >( x0 ), static_cast< float >( y0 ), static_cast< float >( x1 ), static_cast< float >( y1 ),
		                       static_cast< float >( restX ), static_cast< float >( restY ), static_cast< float >( alpha ), 0.0f,
		                       static_cast< float >( pieceFirst[ static_cast< size_t >( g.glyph ) ] ),
		                       static_cast< float >( pieceCount[ static_cast< size_t >( g.glyph ) ] ), static_cast< float >( anchorX ),
		                       static_cast< float >( anchorY ) } );
		++f.drawn;
	}

	//-------------------------------------------------------------------
	// The mask.
	//-------------------------------------------------------------------
	if( !mask.Ensure( width, height, { GL_R16F, 1, false, false, false } ) )
	{
		diag::error( "the mask buffer would not allocate" );
		return FF_FAIL;
	}
	mask.Bind();
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	if( f.drawn > 0 )
	{
		fillBuffer( instanceBuffer, instanceData, GL_STREAM_DRAW );
		ScopedShaderBinding shader( glyphShader.GetGLID() );
		FFGLShader& sh = glyphShader;
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_BUFFER, pieceTexture );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_BUFFER, instanceTexture );
		glActiveTexture( GL_TEXTURE0 );
		sh.Set( "Pieces", 0 );
		sh.Set( "Instances", 1 );
		sh.Set( "Raster", static_cast< float >( W ), static_cast< float >( H ) );
		//column-major: ToLocal = R(-theta) / unitPx.
		const float toLocal[ 4 ] = { static_cast< float >( c / f.unitPx ), static_cast< float >( -s / f.unitPx ),
			                         static_cast< float >( s / f.unitPx ), static_cast< float >( c / f.unitPx ) };
		glUniformMatrix2fv( glGetUniformLocation( sh.GetGLID(), "ToLocal" ), 1, GL_FALSE, toLocal );
		sh.Set( "FrameCentre", static_cast< float >( fx ), static_cast< float >( fy ) );
		sh.Set( "UnitPx", static_cast< float >( f.unitPx ) );
		sh.Set( "Reach", static_cast< float >( reach ) );
		sh.Set( "Ramp", static_cast< float >( f.rampPx ) );
		sh.Set( "GrowPx", static_cast< float >( f.growPx ) );
		sh.Set( "OutlinePx", static_cast< float >( f.outlinePx ) );
		sh.Set( "TestFlags", testFlags & ( shaders::kEvenOdd | shaders::kAllPieces ) );
		setMaxBlend();
		glBindVertexArray( emptyVAO );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, f.drawn );
		glDisable( GL_BLEND );
		glBlendEquation( GL_FUNC_ADD );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}

	//-------------------------------------------------------------------
	// Composite, into the host's framebuffer.
	//-------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( compositeShader.GetGLID() );
		FFGLShader& sh = compositeShader;
		bindUnit( 0, input->Handle );
		bindUnit( 1, mask.Texture() );
		sh.Set( "ClipIn", 0 );
		sh.Set( "MaskIn", 1 );
		const Mask which = static_cast< Mask >( OptionIndex( params[ PT_MASK ], static_cast< int >( Mask::Count ) ) );
		sh.Set( "Outside", which == Mask::Outside ? 1 : 0 );
		float ground[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
		switch( static_cast< Background >( OptionIndex( params[ PT_BACKGROUND ], static_cast< int >( Background::Count ) ) ) )
		{
		case Background::Black: ground[ 3 ] = 1.0f; break;
		case Background::White: ground[ 0 ] = ground[ 1 ] = ground[ 2 ] = ground[ 3 ] = 1.0f; break;
		case Background::Colour:
			ground[ 0 ] = std::clamp( params[ PT_COLOUR_R ], 0.0f, 1.0f );
			ground[ 1 ] = std::clamp( params[ PT_COLOUR_G ], 0.0f, 1.0f );
			ground[ 2 ] = std::clamp( params[ PT_COLOUR_B ], 0.0f, 1.0f );
			ground[ 3 ] = 1.0f;
			break;
		default: break;
		}
		sh.Set( "Ground", ground[ 0 ], ground[ 1 ], ground[ 2 ], ground[ 3 ] );
		sh.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		sh.Set( "ViewOrigin", static_cast< float >( hostViewport[ 0 ] ), static_cast< float >( hostViewport[ 1 ] ) );
		quad.Draw();
		unbindTextureUnits( 2 );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void KnockoutPlugin::releaseBuffers()
{
	for( GLuint* t : { &pieceTexture, &instanceTexture } )
		if( *t )
		{
			glDeleteTextures( 1, t );
			*t = 0;
		}
	for( GLuint* b : { &pieceBuffer, &instanceBuffer } )
		if( *b )
		{
			glDeleteBuffers( 1, b );
			*b = 0;
		}
	if( emptyVAO )
		glDeleteVertexArrays( 1, &emptyVAO );
	emptyVAO = 0;
}

FFResult KnockoutPlugin::DeInitGL()
{
	glyphShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();
	mask.Destroy();
	releaseBuffers();
	uploadedOutlines = 0;
	piecesDirty      = true;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// Parameters.
//---------------------------------------------------------------------------
FFResult KnockoutPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	if( IsTextParam( index ) )
		return FF_SUCCESS;
	const float previous = params[ index ];
	params[ index ]      = value;
	if( value == previous )
		return FF_SUCCESS;
	switch( index )
	{
	case PT_FONT:
	{
		std::lock_guard< std::mutex > lock( textMutex );
		fontIndexMoved = true;
		fontDirty      = true;
		break;
	}
	case PT_WEIGHT:
	case PT_ITALIC:
		fontDirty = true;
		break;
	case PT_ALIGN:
	case PT_TRACKING:
	case PT_LINE_SPACING:
		layoutDirty = true;
		break;
	default:
		break;
	}
	return FF_SUCCESS;
}

float KnockoutPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_ABOUT_TEXT ? params[ index ] : 0.0f;
}

FFResult KnockoutPlugin::SetTextParameter( unsigned int index, const char* value )
{
	//Display-only, and it MUST still succeed: instantiateGL pushes every
	//declared default back through the setters and deletes the instance if
	//one fails.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	const std::string incoming = value != nullptr ? value : "";
	std::lock_guard< std::mutex > lock( textMutex );
	switch( index )
	{
	case PT_TEXT:
		if( incoming != text )
		{
			text      = incoming;
			textDirty = true;
		}
		return FF_SUCCESS;
	case PT_FONT_FILE:
		if( incoming != fontFile )
		{
			fontFile  = incoming;
			fontDirty = true;
		}
		return FF_SUCCESS;
	case PT_FONT_NAME:
		if( incoming != fontName )
		{
			fontName    = incoming;
			fontNameSet = true;
			fontDirty   = true;
		}
		return FF_SUCCESS;
	default:
		return FF_FAIL;
	}
}

char* KnockoutPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		static const std::string line = stoatworks::about::textParam( 0 );
		return const_cast< char* >( line.c_str() );
	}
	std::lock_guard< std::mutex > lock( textMutex );
	switch( index )
	{
	case PT_TEXT: textReturn = text; break;
	case PT_FONT_NAME: textReturn = fontName; break;
	case PT_FONT_FILE: textReturn = fontFile; break;
	default: return CFFGLPlugin::GetTextParameter( index );
	}
	return const_cast< char* >( textReturn.c_str() );
}

std::string KnockoutPlugin::TextValue( unsigned int id )
{
	const char* value = GetTextParameter( id );
	return value ? value : "";
}

} // namespace knockout
