#include "Fonts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "Diag.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#if defined( _WIN32 )
	// CMakeLists sets NOMINMAX globally; repeated so the file is right on its
	// own (polyhedral's trap: std::min becomes `std::(...)`, C2589, Windows
	// only).
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <dirent.h>
	#include <sys/stat.h>
#endif

namespace knockout
{
namespace
{
//---------------------------------------------------------------------------
// Just enough of the OpenType container to describe a face without reading
// the whole file.
//---------------------------------------------------------------------------
uint16_t ReadU16( const unsigned char* p )
{
	return static_cast< uint16_t >( ( p[ 0 ] << 8 ) | p[ 1 ] );
}

uint32_t ReadU32( const unsigned char* p )
{
	return ( static_cast< uint32_t >( p[ 0 ] ) << 24 ) | ( static_cast< uint32_t >( p[ 1 ] ) << 16 )
	       | ( static_cast< uint32_t >( p[ 2 ] ) << 8 ) | static_cast< uint32_t >( p[ 3 ] );
}

bool ReadAt( std::FILE* file, long offset, void* into, size_t bytes )
{
	if( std::fseek( file, offset, SEEK_SET ) != 0 )
		return false;
	return std::fread( into, 1, bytes, file ) == bytes;
}

/// UTF-16BE (platforms 0 and 3) to UTF-8, enough for names: the BMP, and a
/// '?' for anything a surrogate pair would need.
std::string DecodeNameUtf16( const std::vector< unsigned char >& raw )
{
	std::string out;
	for( size_t i = 0; i + 1 < raw.size(); i += 2 )
	{
		const unsigned c = ( static_cast< unsigned >( raw[ i ] ) << 8 ) | raw[ i + 1 ];
		if( c >= 0xD800 && c < 0xE000 )
			out += '?';
		else if( c < 0x80 )
			out += static_cast< char >( c );
		else if( c < 0x800 )
		{
			out += static_cast< char >( 0xC0 | ( c >> 6 ) );
			out += static_cast< char >( 0x80 | ( c & 0x3F ) );
		}
		else
		{
			out += static_cast< char >( 0xE0 | ( c >> 12 ) );
			out += static_cast< char >( 0x80 | ( ( c >> 6 ) & 0x3F ) );
			out += static_cast< char >( 0x80 | ( c & 0x3F ) );
		}
	}
	return out;
}

/// Mac Roman's ASCII half; anything above it is a '?'.
std::string DecodeNameAscii( const std::vector< unsigned char >& raw )
{
	std::string out;
	for( unsigned char c : raw )
		out += c < 0x80 ? static_cast< char >( c ) : '?';
	return out;
}

struct TableRecord
{
	uint32_t offset = 0, length = 0;
};

struct Directory
{
	TableRecord name, os2, head, glyf, cff, cff2;
	bool bitmaps = false;///< sbix or CBDT: the glyphs are pictures
};

bool ReadDirectory( std::FILE* file, uint32_t faceOffset, Directory& into )
{
	unsigned char header[ 12 ];
	if( !ReadAt( file, static_cast< long >( faceOffset ), header, sizeof( header ) ) )
		return false;
	const uint16_t tableCount = ReadU16( header + 4 );
	if( tableCount == 0 || tableCount > 512 )
		return false;
	std::vector< unsigned char > records( static_cast< size_t >( tableCount ) * 16u );
	if( !ReadAt( file, static_cast< long >( faceOffset ) + 12, records.data(), records.size() ) )
		return false;
	for( uint16_t i = 0; i < tableCount; ++i )
	{
		const unsigned char* r = records.data() + static_cast< size_t >( i ) * 16u;
		const TableRecord record { ReadU32( r + 8 ), ReadU32( r + 12 ) };
		if( std::memcmp( r, "name", 4 ) == 0 )
			into.name = record;
		else if( std::memcmp( r, "OS/2", 4 ) == 0 )
			into.os2 = record;
		else if( std::memcmp( r, "head", 4 ) == 0 )
			into.head = record;
		else if( std::memcmp( r, "glyf", 4 ) == 0 )
			into.glyf = record;
		else if( std::memcmp( r, "CFF ", 4 ) == 0 )
			into.cff = record;
		else if( std::memcmp( r, "CFF2", 4 ) == 0 )
			into.cff2 = record;
		else if( std::memcmp( r, "sbix", 4 ) == 0 || std::memcmp( r, "CBDT", 4 ) == 0 )
			into.bitmaps = true;
	}
	return true;
}

/// The best-scoring record for a pair of name IDs: the typographic one
/// (`preferred`, 16 or 17) over the legacy one (1 or 2), English over not,
/// Windows over Mac. polyhedral's scoring, with its rule for skipping a
/// record mostly made of '?' (a Chinese family name next to an English one).
std::string ReadName( std::FILE* file, const TableRecord& name, uint16_t preferred, uint16_t legacy )
{
	if( name.offset == 0 || name.length < 6 || name.length > 1u << 20 )
		return {};
	std::vector< unsigned char > table( name.length );
	if( !ReadAt( file, static_cast< long >( name.offset ), table.data(), table.size() ) )
		return {};
	const uint16_t recordCount = ReadU16( table.data() + 2 );
	const uint16_t storage     = ReadU16( table.data() + 4 );
	std::string best;
	int bestScore = -1;
	for( uint16_t i = 0; i < recordCount; ++i )
	{
		const size_t at = 6u + static_cast< size_t >( i ) * 12u;
		if( at + 12 > table.size() )
			break;
		const unsigned char* record = table.data() + at;
		const uint16_t platform     = ReadU16( record + 0 );
		const uint16_t language     = ReadU16( record + 4 );
		const uint16_t nameId       = ReadU16( record + 6 );
		const uint16_t length       = ReadU16( record + 8 );
		const uint16_t offset       = ReadU16( record + 10 );
		if( nameId != preferred && nameId != legacy )
			continue;
		const size_t start = static_cast< size_t >( storage ) + offset;
		if( start + length > table.size() )
			continue;
		const std::vector< unsigned char > raw( table.begin() + static_cast< long >( start ),
		                                        table.begin() + static_cast< long >( start + length ) );
		const std::string decoded = platform == 3 || platform == 0 ? DecodeNameUtf16( raw ) : DecodeNameAscii( raw );
		if( decoded.empty() )
			continue;
		const size_t unknown = static_cast< size_t >( std::count( decoded.begin(), decoded.end(), '?' ) );
		if( unknown * 4 > decoded.size() )
			continue;
		const bool english = ( platform == 3 && language == 0x409 ) || ( platform == 1 && language == 0 );
		const int score    = ( nameId == preferred ? 4 : 0 ) + ( english ? 2 : 0 ) + ( platform == 3 ? 1 : 0 );
		if( score > bestScore )
		{
			bestScore = score;
			best      = decoded;
		}
	}
	return best;
}

bool DescribeAt( std::FILE* file, uint32_t faceOffset, FontFace& face )
{
	Directory directory;
	if( !ReadDirectory( file, faceOffset, directory ) )
		return false;
	//Outlines this plugin can read: TrueType quadratics or CFF cubics. A
	//face with neither is bitmaps (or CFF2, which stb_truetype cannot parse).
	//A face with bitmap glyphs is left out even with a glyf: Apple Color
	//Emoji has one, of placeholder boxes, next to 190 MB of sbix pictures.
	if( ( directory.glyf.offset == 0 && directory.cff.offset == 0 ) || directory.bitmaps )
		return false;
	face.family = ReadName( file, directory.name, 16, 1 );
	face.style  = ReadName( file, directory.name, 17, 2 );
	if( face.family.empty() )
		return false;
	face.weight = 400;
	face.italic = false;
	unsigned char os2[ 64 ];
	if( directory.os2.offset != 0 && directory.os2.length >= 64 && ReadAt( file, static_cast< long >( directory.os2.offset ), os2, sizeof( os2 ) ) )
	{
		const uint16_t weight = ReadU16( os2 + 4 );
		//fsSelection bit 0 is ITALIC, bit 9 OBLIQUE: both are slanted faces
		//to someone choosing one, and CSS falls back between them.
		const uint16_t selection = ReadU16( os2 + 62 );
		if( weight >= 1 && weight <= 1000 )
			face.weight = weight;
		face.italic = ( selection & 0x0201 ) != 0;
	}
	else
	{
		unsigned char head[ 46 ];
		if( directory.head.offset != 0 && ReadAt( file, static_cast< long >( directory.head.offset ), head, sizeof( head ) ) )
		{
			const uint16_t macStyle = ReadU16( head + 44 );
			face.weight             = ( macStyle & 1 ) != 0 ? 700 : 400;
			face.italic             = ( macStyle & 2 ) != 0;
		}
	}
	return true;
}

/// The offsets of the faces in a file: one, or a collection's.
std::vector< uint32_t > FaceOffsets( std::FILE* file )
{
	unsigned char tag[ 12 ];
	std::vector< uint32_t > offsets;
	if( !ReadAt( file, 0, tag, sizeof( tag ) ) )
		return offsets;
	if( std::memcmp( tag, "ttcf", 4 ) == 0 )
	{
		const uint32_t count = ReadU32( tag + 8 );
		if( count > 0 && count < 4096 )
		{
			std::vector< unsigned char > table( static_cast< size_t >( count ) * 4u );
			if( ReadAt( file, 12, table.data(), table.size() ) )
				for( uint32_t i = 0; i < count; ++i )
					offsets.push_back( ReadU32( table.data() + static_cast< size_t >( i ) * 4u ) );
		}
		return offsets;
	}
	//0x00010000 is TrueType outlines, 'OTTO' CFF, 'true' an old Mac TrueType.
	const uint32_t version = ReadU32( tag );
	if( version == 0x00010000u || version == 0x4F54544Fu || version == 0x74727565u )
		offsets.push_back( 0 );
	return offsets;
}

void ScanFontFile( const std::string& path, std::vector< FontFace >& into )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
		return;
	const std::vector< uint32_t > offsets = FaceOffsets( file );
	for( size_t i = 0; i < offsets.size(); ++i )
	{
		FontFace face;
		if( !DescribeAt( file, offsets[ i ], face ) )
			continue;
		//".Aqua Kana", ".SF NS" and friends: UI machinery, not typefaces.
		//Apple's own font menus hide them on this rule.
		if( face.family[ 0 ] == '.' )
			continue;
		face.path            = path;
		face.collectionIndex = static_cast< int >( i );
		into.push_back( face );
	}
	std::fclose( file );
}

bool HasFontExtension( const std::string& name )
{
	if( name.size() < 5 )
		return false;
	std::string tail = name.substr( name.size() - 4 );
	for( char& c : tail )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return tail == ".ttf" || tail == ".otf" || tail == ".ttc" || tail == ".otc";
}

void ScanDirectory( const std::string& directory, std::vector< FontFace >& into, int depth )
{
	//Two levels: macOS keeps Supplemental and per-family folders one down,
	//and an unbounded walk is how a plugin constructor becomes a hang.
	if( depth > 2 )
		return;
#if defined( _WIN32 )
	const std::string pattern = directory + "\\*";
	WIN32_FIND_DATAA found {};
	HANDLE handle = FindFirstFileA( pattern.c_str(), &found );
	if( handle == INVALID_HANDLE_VALUE )
		return;
	do
	{
		const std::string name = found.cFileName;
		if( name == "." || name == ".." )
			continue;
		const std::string full = directory + "\\" + name;
		if( ( found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 )
			ScanDirectory( full, into, depth + 1 );
		else if( HasFontExtension( name ) )
			ScanFontFile( full, into );
	} while( FindNextFileA( handle, &found ) );
	FindClose( handle );
#else
	DIR* dir = opendir( directory.c_str() );
	if( dir == nullptr )
		return;
	while( struct dirent* entry = readdir( dir ) )
	{
		const std::string name = entry->d_name;
		if( name.empty() || name[ 0 ] == '.' )
			continue;
		const std::string full = directory + "/" + name;
		struct stat info {};
		if( stat( full.c_str(), &info ) != 0 )
			continue;
		if( S_ISDIR( info.st_mode ) )
			ScanDirectory( full, into, depth + 1 );
		else if( S_ISREG( info.st_mode ) && HasFontExtension( name ) )
			ScanFontFile( full, into );
	}
	closedir( dir );
#endif
}

std::vector< std::string > FontDirectories()
{
	std::vector< std::string > directories;
	//The harness points the scan at a folder of its own, so a check of
	//"what the dropdown lists" is not a check of this Mac's fonts.
	if( const char* only = std::getenv( "KNOCKOUT_FONT_DIR" ) )
	{
		directories.push_back( only );
		return directories;
	}
#if defined( _WIN32 )
	char windows[ MAX_PATH ] {};
	if( GetWindowsDirectoryA( windows, MAX_PATH ) != 0 )
		directories.push_back( std::string( windows ) + "\\Fonts" );
	if( const char* local = std::getenv( "LOCALAPPDATA" ) )
		directories.push_back( std::string( local ) + "\\Microsoft\\Windows\\Fonts" );
#else
	if( const char* home = std::getenv( "HOME" ) )
		directories.push_back( std::string( home ) + "/Library/Fonts" );
	directories.push_back( "/Library/Fonts" );
	directories.push_back( "/System/Library/Fonts" );
#endif
	return directories;
}

std::string Lowered( const std::string& text )
{
	std::string out = text;
	for( char& c : out )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return out;
}

std::string FileName( const std::string& path )
{
	const size_t slash = path.find_last_of( "/\\" );
	return slash == std::string::npos ? path : path.substr( slash + 1 );
}
} // namespace

//---------------------------------------------------------------------------
const std::vector< FontFace >& InstalledFaces()
{
	static const std::vector< FontFace > faces = [] {
		std::vector< FontFace > found;
		for( const std::string& directory : FontDirectories() )
			ScanDirectory( directory, found, 0 );
		std::sort( found.begin(), found.end(), []( const FontFace& a, const FontFace& b ) {
			const std::string left = Lowered( a.family ), right = Lowered( b.family );
			if( left != right )
				return left < right;
			if( a.weight != b.weight )
				return a.weight < b.weight;
			if( a.italic != b.italic )
				return !a.italic;
			//The same face twice (a user copy of a system font): the first
			//face of a collection, then the shorter file name -- polyhedral's
			//fix for downpour's "Georgia Bold Italic.ttf" sorting first.
			if( a.collectionIndex != b.collectionIndex )
				return a.collectionIndex < b.collectionIndex;
			const size_t la = FileName( a.path ).size(), lb = FileName( b.path ).size();
			if( la != lb )
				return la < lb;
			return a.path < b.path;
		} );
		diag::info( "font scan found " + std::to_string( found.size() ) + " faces" );
		return found;
	}();
	return faces;
}

const std::vector< FontFamily >& InstalledFamilies()
{
	static const std::vector< FontFamily > families = [] {
		std::vector< FontFamily > out;
		const std::vector< FontFace >& faces = InstalledFaces();
		for( size_t i = 0; i < faces.size(); ++i )
		{
			if( out.empty() || Lowered( out.back().name ) != Lowered( faces[ i ].family ) )
				out.push_back( { faces[ i ].family, {} } );
			out.back().faces.push_back( static_cast< int >( i ) );
		}
		return out;
	}();
	return families;
}

int FindFamily( const std::string& name )
{
	if( name.empty() )
		return -1;
	const std::string wanted                = Lowered( name );
	const std::vector< FontFamily >& families = InstalledFamilies();
	for( size_t i = 0; i < families.size(); ++i )
		if( Lowered( families[ i ].name ) == wanted )
			return static_cast< int >( i );
	return -1;
}

int NearestWeight( int wanted, const std::vector< int >& available )
{
	if( available.empty() )
		return -1;
	//CSS Fonts 4, 5.2: between 400 and 500, up to 500 first, then down, then
	//above 500; below 400, down first, then up; above 500, up, then down.
	auto rank = [ wanted ]( int w ) -> long {
		const long d = std::labs( static_cast< long >( w ) - wanted );
		if( wanted >= 400 && wanted <= 500 )
		{
			if( w >= wanted && w <= 500 )
				return d;
			if( w < wanted )
				return 10000 + d;
			return 20000 + d;
		}
		if( wanted < 400 )
			return w <= wanted ? d : 10000 + d;
		return w >= wanted ? d : 10000 + d;
	};
	int best      = available[ 0 ];
	long bestRank = rank( best );
	for( int w : available )
		if( rank( w ) < bestRank )
		{
			best     = w;
			bestRank = rank( w );
		}
	return best;
}

int MatchFace( int family, int weight, bool italic )
{
	const std::vector< FontFamily >& families = InstalledFamilies();
	if( family < 0 || family >= static_cast< int >( families.size() ) )
		return -1;
	const std::vector< FontFace >& faces = InstalledFaces();
	//Slant first (CSS: italic falls back to oblique, then to upright), then
	//weight among the faces that are left.
	std::vector< int > candidates;
	for( int f : families[ static_cast< size_t >( family ) ].faces )
		if( faces[ static_cast< size_t >( f ) ].italic == italic )
			candidates.push_back( f );
	if( candidates.empty() )
		candidates = families[ static_cast< size_t >( family ) ].faces;
	std::vector< int > weights;
	for( int f : candidates )
		weights.push_back( faces[ static_cast< size_t >( f ) ].weight );
	const int chosen = NearestWeight( weight, weights );
	//The scan's order puts the preferred copy of a weight first.
	for( int f : candidates )
		if( faces[ static_cast< size_t >( f ) ].weight == chosen )
			return f;
	return candidates.front();
}

int DefaultFamily()
{
	for( const char* name : { "Arial Black", "Arial", "Helvetica", "Helvetica Neue" } )
	{
		const int found = FindFamily( name );
		if( found >= 0 )
			return found;
	}
	return InstalledFamilies().empty() ? -1 : 0;
}

std::vector< uint32_t > FamilyValues( const std::vector< std::string >& names )
{
	std::vector< uint32_t > values;
	std::unordered_set< uint32_t > taken;
	for( const std::string& name : names )
	{
		uint32_t hash = 2166136261u;
		for( char c : Lowered( name ) )
			hash = ( hash ^ static_cast< unsigned char >( c ) ) * 16777619u;
		uint32_t value = ( ( hash >> 24 ) ^ hash ) & 0xFFFFFFu;
		while( !taken.insert( value ).second )
			value = ( value + 1 ) & 0xFFFFFFu;
		values.push_back( value );
	}
	return values;
}

static const std::vector< uint32_t >& InstalledValues()
{
	static const std::vector< uint32_t > values = [] {
		std::vector< std::string > names;
		for( const FontFamily& family : InstalledFamilies() )
			names.push_back( family.name );
		return FamilyValues( names );
	}();
	return values;
}

float FamilyValue( int family )
{
	const std::vector< uint32_t >& values = InstalledValues();
	if( family < 0 || family >= static_cast< int >( values.size() ) )
		return 0.0f;
	return static_cast< float >( values[ static_cast< size_t >( family ) ] );
}

int FamilyOfValue( float value )
{
	if( !( value >= 0.0f ) )
		return -1;
	const long wanted                     = std::lround( value );
	const std::vector< uint32_t >& values = InstalledValues();
	for( size_t i = 0; i < values.size(); ++i )
		if( static_cast< long >( values[ i ] ) == wanted )
			return static_cast< int >( i );
	return -1;
}

bool DescribeFace( const std::string& path, int collectionIndex, FontFace& into )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
		return false;
	const std::vector< uint32_t > offsets = FaceOffsets( file );
	bool ok = collectionIndex >= 0 && collectionIndex < static_cast< int >( offsets.size() )
	          && DescribeAt( file, offsets[ static_cast< size_t >( collectionIndex ) ], into );
	std::fclose( file );
	if( ok )
	{
		into.path            = path;
		into.collectionIndex = collectionIndex;
	}
	return ok;
}

//---------------------------------------------------------------------------
// The open face.
//---------------------------------------------------------------------------
struct Font::Impl
{
	std::vector< unsigned char > data;
	stbtt_fontinfo info {};
};

Font::Font() :
	impl( new Impl )
{
}

Font::~Font() = default;

bool Font::Load( const std::string& path, int collectionIndex )
{
	loaded = false;
	impl->data.clear();
	if( path.empty() )
		return false;
	FontFace described;
	if( !DescribeFace( path, collectionIndex, described ) )
	{
		diag::warn( "not a face this plugin can draw: " + path );
		return false;
	}
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
		return false;
	std::fseek( file, 0, SEEK_END );
	const long size = std::ftell( file );
	std::fseek( file, 0, SEEK_SET );
	//A font over 64 MB is a pan-CJK collection; loading one to draw a word is
	//not a trade to make inside somebody else's process.
	if( size <= 0 || size > 64L * 1024L * 1024L )
	{
		std::fclose( file );
		diag::warn( "font is an implausible size, ignoring: " + path );
		return false;
	}
	impl->data.resize( static_cast< size_t >( size ) );
	const size_t read = std::fread( impl->data.data(), 1, impl->data.size(), file );
	std::fclose( file );
	if( read != impl->data.size() )
		return false;
	const int offset = stbtt_GetFontOffsetForIndex( impl->data.data(), collectionIndex );
	if( offset < 0 || stbtt_InitFont( &impl->info, impl->data.data(), offset ) == 0 )
	{
		diag::warn( "font will not parse: " + path );
		impl->data.clear();
		return false;
	}
	//unitsPerEm is head + 18; stb_truetype keeps the table's offset.
	const unsigned char* head = impl->info.data + impl->info.head;
	unitsPerEm                = ReadU16( head + 18 );
	if( unitsPerEm < 16 || unitsPerEm > 16384 )
		unitsPerEm = 1000;
	stbtt_GetFontVMetrics( &impl->info, &ascent, &descent, &lineGap );
	face   = described;
	loaded = true;
	diag::info( "font loaded: " + face.family + " " + face.style + " (" + std::to_string( face.weight ) + ")" );
	return true;
}

int Font::GlyphFor( uint32_t codepoint ) const
{
	return loaded ? stbtt_FindGlyphIndex( &impl->info, static_cast< int >( codepoint ) ) : 0;
}

int Font::Advance( int glyph ) const
{
	if( !loaded )
		return 0;
	int advance = 0, bearing = 0;
	stbtt_GetGlyphHMetrics( &impl->info, glyph, &advance, &bearing );
	return advance;
}

int Font::Kern( int left, int right ) const
{
	return loaded ? stbtt_GetGlyphKernAdvance( &impl->info, left, right ) : 0;
}

std::vector< Font::Vertex > Font::Shape( int glyph ) const
{
	std::vector< Vertex > out;
	if( !loaded )
		return out;
	stbtt_vertex* vertices = nullptr;
	const int count        = stbtt_GetGlyphShape( &impl->info, glyph, &vertices );
	for( int i = 0; i < count; ++i )
	{
		const stbtt_vertex& v = vertices[ i ];
		Vertex::Type type     = Vertex::Line;
		switch( v.type )
		{
		case STBTT_vmove: type = Vertex::Move; break;
		case STBTT_vline: type = Vertex::Line; break;
		case STBTT_vcurve: type = Vertex::Quad; break;
		case STBTT_vcubic: type = Vertex::Cubic; break;
		default: continue;
		}
		out.push_back( { type, v.x, v.y, v.cx, v.cy, v.cx1, v.cy1 } );
	}
	stbtt_FreeShape( &impl->info, vertices );
	return out;
}

} // namespace knockout
