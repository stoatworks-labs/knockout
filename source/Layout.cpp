#include "Layout.h"

#include <algorithm>
#include <limits>

namespace knockout
{
std::u32string DecodeText( const std::string& utf8 )
{
	std::u32string decoded;
	const size_t n = utf8.size();
	for( size_t i = 0; i < n; )
	{
		const unsigned char c = static_cast< unsigned char >( utf8[ i ] );
		char32_t code         = 0xFFFD;
		size_t width          = 1;
		if( c < 0x80 )
			code = c;
		else if( c >= 0xC2 && c < 0xF5 )
		{
			const size_t need = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
			char32_t value    = c & ( need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07 );
			bool ok           = true;
			for( size_t k = 1; ok && k <= need; ++k )
			{
				if( i + k >= n )
				{
					ok = false;
					break;
				}
				const unsigned char cont = static_cast< unsigned char >( utf8[ i + k ] );
				if( ( cont & 0xC0 ) != 0x80 )
					ok = false;
				else
					value = ( value << 6 ) | ( cont & 0x3F );
			}
			//Overlong forms, surrogates and past U+10FFFF are malformed too.
			const char32_t least = need == 1 ? 0x80 : need == 2 ? 0x800 : 0x10000;
			if( ok && value >= least && value <= 0x10FFFF && !( value >= 0xD800 && value < 0xE000 ) )
			{
				code  = value;
				width = need + 1;
			}
		}
		i += width;

		if( code == '\\' && i < n && ( utf8[ i ] == 'n' || utf8[ i ] == '\\' ) )
		{
			decoded += utf8[ i ] == 'n' ? U'\n' : U'\\';
			++i;
			continue;
		}
		if( code == '\r' )
		{
			if( i < n && utf8[ i ] == '\n' )
				++i;
			decoded += U'\n';
			continue;
		}
		if( code == '\t' )
		{
			decoded += U' ';
			continue;
		}
		if( code == '\n' || code >= 0x20 )
			decoded += code;
	}
	return decoded;
}

void Typesetter::UseFont( const Font* f, const OutlineOptions& options )
{
	font           = f;
	outlineOptions = options;
	byGlyph.clear();
	outlines.clear();
}

int Typesetter::outlineFor( int glyphIndex )
{
	const auto found = byGlyph.find( glyphIndex );
	if( found != byGlyph.end() )
		return found->second;
	const int index = static_cast< int >( outlines.size() );
	outlines.push_back( BuildOutline( font->Shape( glyphIndex ), font->UnitsPerEm(), outlineOptions ) );
	byGlyph[ glyphIndex ] = index;
	return index;
}

Setting Typesetter::Set( const std::u32string& text, const SetOptions& options )
{
	Setting setting;
	if( font == nullptr || !font->IsLoaded() )
		return setting;
	const double em         = font->UnitsPerEm();
	const double tracking   = options.trackingEm * em;
	const double lineHeight = static_cast< double >( font->Ascent() - font->Descent() + font->LineGap() ) * options.lineSpacing;

	//One line at a time: set it from x = 0, then move it by its width.
	size_t start = 0;
	int line     = 0;
	int order    = 0;
	while( start <= text.size() )
	{
		size_t end = text.find( U'\n', start );
		if( end == std::u32string::npos )
			end = text.size();
		std::vector< PlacedGlyph > placed;
		double pen = 0.0, width = 0.0;
		int previous = -1;
		for( size_t i = start; i < end; ++i )
		{
			const int glyph = font->GlyphFor( text[ i ] );
			if( previous >= 0 && options.kerning )
				pen += font->Kern( previous, glyph );
			const int outline = outlineFor( glyph );
			if( outlines[ static_cast< size_t >( outline ) ].HasInk() )
				placed.push_back( { outline, pen, -line * lineHeight, 0, line } );
			pen += font->Advance( glyph );
			//A line's width is its pen after the last advance, without the
			//tracking that would follow it.
			width = pen;
			pen += tracking;
			previous = glyph;
		}
		const double shift = options.align == Align::Left ? 0.0 : options.align == Align::Centre ? -0.5 * width : -width;
		for( PlacedGlyph& g : placed )
		{
			g.x += shift;
			g.order = order++;
			setting.glyphs.push_back( g );
		}
		++line;
		start = end + 1;
	}
	setting.lines = line;

	setting.x0 = setting.y0 = std::numeric_limits< double >::max();
	setting.x1 = setting.y1 = -std::numeric_limits< double >::max();
	for( const PlacedGlyph& g : setting.glyphs )
	{
		const GlyphOutline& o = outlines[ static_cast< size_t >( g.glyph ) ];
		setting.x0            = std::min( setting.x0, g.x + o.x0 );
		setting.y0            = std::min( setting.y0, g.y + o.y0 );
		setting.x1            = std::max( setting.x1, g.x + o.x1 );
		setting.y1            = std::max( setting.y1, g.y + o.y1 );
	}
	if( setting.glyphs.empty() )
		setting.x0 = setting.y0 = setting.x1 = setting.y1 = 0.0;
	return setting;
}

} // namespace knockout
