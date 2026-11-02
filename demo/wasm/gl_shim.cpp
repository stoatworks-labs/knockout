/*
    The GL entry points emscripten's WebGL2 library cannot take from the plugin
    as they are. Every other GL call the plugin and the FFGL SDK make goes
    straight to emscripten's implementation, on the page's own WebGL2 context.

    glShaderSource
        The plugin hands GL desktop GLSL 4.10 (`#version 410 core`); WebGL2
        compiles GLSL ES 3.00. The page's `knockoutShaderSource` (plugin.js)
        first REQUIRES the text to be byte for byte one of the page's own
        assemblies of the plugin's shaders (demo/shaders.js, which
        demo/tools/check_shaders.py holds to source/Shaders.cpp, and its
        PROGRAMS table to Knockout.cpp's InitGL and Assemble()), then applies
        the demo kit's `port()` (the version line and the ES precision
        defaults), then the texture-buffer rewrite described below. A shader
        the plugin assembled differently from the page's copy is refused (an
        empty source, so the plugin's InitGL fails and says which pass), never
        compiled quietly.

    glEnable / glDisable / glIsEnabled
        GL_PROGRAM_POINT_SIZE is desktop-only state (WebGL2 rejects the enum
        with INVALID_ENUM). The plugin's GLState.h saves and restores it on
        every frame, as a plugin inside Resolume must. Here it reads as off and
        enabling it is ignored; the plugin draws no points.

    Texture buffers: glBindBuffer, glBufferData, glBindTexture, glTexBuffer,
    glGetIntegerv, glDeleteBuffers, glDeleteTextures
        The plugin keeps its outlines (two RGBA32F texels a piece) and its
        per-glyph instances (three texels a glyph) in BUFFER TEXTURES: a
        GL_TEXTURE_BUFFER buffer, filled with glBufferData, viewed through a
        texture made with glTexBuffer, read in the shaders as a samplerBuffer
        with texelFetch( table, i ). WebGL2 is OpenGL ES 3.0, which has no
        buffer textures at all (they arrived in ES 3.2), so each one is a 2D
        RGBA32F texture here:

          - glBindBuffer( GL_TEXTURE_BUFFER, b ) records the binding and never
            reaches WebGL; glBufferData on that target keeps the bytes here, on
            the CPU, and uploads them to every texture viewing that buffer.
          - glBindTexture( GL_TEXTURE_BUFFER, t ) binds t as the TEXTURE_2D of
            the active unit, remembering the 2D texture it displaced, and
            binding 0 to GL_TEXTURE_BUFFER gives that one back. Desktop GL
            keeps the two targets apart on a unit, WebGL2 cannot, so binding a
            2D texture on a unit while a buffer texture holds it THROWS rather
            than letting one quietly replace the other. The plugin never does.
          - glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, b ) attaches the bound
            texture to b. Any other format throws.
          - An upload is the buffer's texels, in order, in rows of kRowTexels
            (2048, the smallest GL_MAX_TEXTURE_SIZE ES 3.0 allows), the last
            row padded with zeros: texel i is at ( i % 2048, i / 2048 ).
            glTexImage2D, RGBA32F, NEAREST, so a texelFetch reads the float the
            plugin wrote, bit for bit.
          - glGetIntegerv answers GL_TEXTURE_BINDING_BUFFER and GL_TEXTURE_BUFFER
            from that record, GL_TEXTURE_BINDING_2D with the displaced texture
            while a buffer texture holds the unit (what desktop GL would say),
            and GL_MAX_TEXTURE_BUFFER_SIZE with what the rows can hold (2048 x
            GL_MAX_TEXTURE_SIZE): the number the plugin checks its table against.

        The shaders' half (samplerBuffer -> sampler2D, texelFetch( t, i ) ->
        knockoutBufferFetch( t, i ), which reads texel ( i % w, i / w )) is
        plugin.js's, applied after the check above, and the page lists it.
*/
#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// ES 3.2's names for the desktop GL 3.1 / 4.1 enums the plugin uses.
#ifndef GL_TEXTURE_BUFFER
	#define GL_TEXTURE_BUFFER 0x8C2A
#endif
#ifndef GL_MAX_TEXTURE_BUFFER_SIZE
	#define GL_MAX_TEXTURE_BUFFER_SIZE 0x8C2B
#endif
#ifndef GL_TEXTURE_BINDING_BUFFER
	#define GL_TEXTURE_BINDING_BUFFER 0x8C2C
#endif

extern "C"
{
void emscripten_glBindBuffer( GLenum target, GLuint buffer );
void emscripten_glBufferData( GLenum target, GLsizeiptr size, const void* data, GLenum usage );
void emscripten_glBindTexture( GLenum target, GLuint texture );
void emscripten_glGetIntegerv( GLenum pname, GLint* data );
void emscripten_glDeleteBuffers( GLsizei n, const GLuint* buffers );
void emscripten_glDeleteTextures( GLsizei n, const GLuint* textures );
void emscripten_glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type, const void* pixels );
void emscripten_glTexParameteri( GLenum target, GLenum pname, GLint param );
void emscripten_glActiveTexture( GLenum texture );
}

namespace
{
constexpr GLenum kProgramPointSize = 0x8642;///< GL_PROGRAM_POINT_SIZE, desktop GL 3.2
constexpr GLsizei kRowTexels       = 2048;  ///< ES 3.0's minimum GL_MAX_TEXTURE_SIZE
constexpr size_t kTexelBytes       = 16;    ///< RGBA32F

// clang-format off
EM_JS( void, knockoutShaderSource, ( GLuint shader, const char* source ), {
	const object = GL.shaders[ shader ];
	const stage  = GLctx.getShaderParameter( object, 0x8B4F /* GL_SHADER_TYPE */ ) === 0x8B31 ? 'vertex' : 'fragment';
	GLctx.shaderSource( object, Module[ 'knockoutShaderSource' ]( stage, UTF8ToString( source ) ) );
} );
EM_JS( void, knockoutEnable, ( GLenum cap ), { GLctx.enable( cap ); } );
EM_JS( void, knockoutDisable, ( GLenum cap ), { GLctx.disable( cap ); } );
EM_JS( int, knockoutIsEnabled, ( GLenum cap ), { return GLctx.isEnabled( cap ) ? 1 : 0; } );
// A use of a buffer texture this emulation cannot honour: thrown into the page,
// which stops drawing and shows it, rather than drawing something else.
EM_JS( void, knockoutRefuse, ( const char* message ), {
	throw new Error( UTF8ToString( message ) + ' (demo/wasm/gl_shim.cpp)' );
} );
// For the page's read-out: the shape of the last upload, per kind of table.
EM_JS( void, knockoutUploaded, ( GLuint texture, int texels, int width, int height ), {
	const uploads = Module[ 'knockoutUploads' ] || ( Module[ 'knockoutUploads' ] = {} );
	const count        = uploads[ texture ] ? uploads[ texture ].count + 1 : 1;
	uploads[ texture ] = { texels : texels, width : width, height : height, count : count };
} );
// clang-format on

/// What desktop GL keeps for buffer textures and WebGL2 has nowhere to put.
struct BufferTextures
{
	std::unordered_map< GLuint, std::vector< unsigned char > > contents;///< by buffer name
	std::unordered_map< GLuint, GLuint > viewing;                       ///< texture name -> buffer name
	GLuint boundBuffer = 0;                                              ///< the GL_TEXTURE_BUFFER buffer binding

	struct Hold
	{
		GLuint texture  = 0;///< the buffer texture bound on the unit
		GLint displaced = 0;///< the unit's TEXTURE_2D before it
	};
	std::unordered_map< GLint, Hold > units;///< by GL_TEXTUREi
} g;

GLint activeUnit()
{
	GLint unit = GL_TEXTURE0;
	emscripten_glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
	return unit;
}

void refuse( const std::string& message )
{
	knockoutRefuse( message.c_str() );
}

/// The buffer's bytes into `texture`, as rows of kRowTexels RGBA32F texels.
void upload( GLuint texture, const std::vector< unsigned char >& bytes )
{
	if( bytes.size() % kTexelBytes != 0 )
		refuse( "a texture buffer of " + std::to_string( bytes.size() ) + " bytes is not a whole number of RGBA32F texels" );
	const size_t texels  = bytes.size() / kTexelBytes;
	const GLsizei width  = static_cast< GLsizei >( std::clamp< size_t >( texels, 1, kRowTexels ) );
	const GLsizei height = static_cast< GLsizei >( std::max< size_t >( 1, ( texels + kRowTexels - 1 ) / kRowTexels ) );
	std::vector< float > rows( static_cast< size_t >( width ) * static_cast< size_t >( height ) * 4, 0.0f );
	if( !bytes.empty() )
		std::memcpy( rows.data(), bytes.data(), bytes.size() );

	GLint previous = 0;
	emscripten_glGetIntegerv( GL_TEXTURE_BINDING_2D, &previous );
	emscripten_glBindTexture( GL_TEXTURE_2D, texture );
	emscripten_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, rows.data() );
	emscripten_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	emscripten_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	emscripten_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	emscripten_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	emscripten_glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previous ) );
	knockoutUploaded( texture, static_cast< int >( texels ), width, height );
}

void uploadViewers( GLuint buffer )
{
	const auto found = g.contents.find( buffer );
	if( found == g.contents.end() )
		return;
	for( const auto& [ texture, viewed ] : g.viewing )
		if( viewed == buffer )
			upload( texture, found->second );
}
} // namespace

extern "C"
{
void glShaderSource( GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length )
{
	std::string source;
	for( GLsizei i = 0; i < count; ++i )
	{
		if( string[ i ] == nullptr )
			continue;
		const size_t n = ( length != nullptr && length[ i ] >= 0 ) ? static_cast< size_t >( length[ i ] ) : std::strlen( string[ i ] );
		source.append( string[ i ], n );
	}
	knockoutShaderSource( shader, source.c_str() );
}

void glEnable( GLenum cap )
{
	if( cap != kProgramPointSize )
		knockoutEnable( cap );
}

void glDisable( GLenum cap )
{
	if( cap != kProgramPointSize )
		knockoutDisable( cap );
}

GLboolean glIsEnabled( GLenum cap )
{
	if( cap == kProgramPointSize )
		return GL_FALSE;
	return knockoutIsEnabled( cap ) ? GL_TRUE : GL_FALSE;
}

//---------------------------------------------------------------------------
// Buffer textures.
//---------------------------------------------------------------------------
void glBindBuffer( GLenum target, GLuint buffer )
{
	if( target == GL_TEXTURE_BUFFER )
	{
		g.boundBuffer = buffer;
		return;
	}
	emscripten_glBindBuffer( target, buffer );
}

void glBufferData( GLenum target, GLsizeiptr size, const void* data, GLenum usage )
{
	if( target != GL_TEXTURE_BUFFER )
	{
		emscripten_glBufferData( target, size, data, usage );
		return;
	}
	if( g.boundBuffer == 0 )
		refuse( "glBufferData( GL_TEXTURE_BUFFER ) with no buffer bound" );
	std::vector< unsigned char >& bytes = g.contents[ g.boundBuffer ];
	bytes.assign( static_cast< size_t >( size ), 0 );
	if( data != nullptr && size > 0 )
		std::memcpy( bytes.data(), data, static_cast< size_t >( size ) );
	uploadViewers( g.boundBuffer );
}

void glBindTexture( GLenum target, GLuint texture )
{
	if( target == GL_TEXTURE_BUFFER )
	{
		const GLint unit = activeUnit();
		auto held        = g.units.find( unit );
		if( texture == 0 )
		{
			if( held != g.units.end() )
			{
				emscripten_glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( held->second.displaced ) );
				g.units.erase( held );
			}
			return;
		}
		if( held == g.units.end() )
		{
			BufferTextures::Hold hold;
			emscripten_glGetIntegerv( GL_TEXTURE_BINDING_2D, &hold.displaced );
			held = g.units.emplace( unit, hold ).first;
		}
		held->second.texture = texture;
		emscripten_glBindTexture( GL_TEXTURE_2D, texture );
		return;
	}
	if( target == GL_TEXTURE_2D && !g.units.empty() && g.units.count( activeUnit() ) != 0 )
		refuse( "glBindTexture( GL_TEXTURE_2D ) on a unit a buffer texture holds: WebGL2 has one binding where desktop GL has two" );
	emscripten_glBindTexture( target, texture );
}

void glTexBuffer( GLenum target, GLenum internalformat, GLuint buffer )
{
	if( target != GL_TEXTURE_BUFFER )
		refuse( "glTexBuffer on a target that is not GL_TEXTURE_BUFFER" );
	if( internalformat != GL_RGBA32F )
		refuse( "glTexBuffer with a format other than GL_RGBA32F" );
	const auto held = g.units.find( activeUnit() );
	if( held == g.units.end() )
		refuse( "glTexBuffer with no buffer texture bound" );
	const GLuint texture = held->second.texture;
	if( buffer == 0 )
	{
		g.viewing.erase( texture );
		return;
	}
	g.viewing[ texture ] = buffer;
	const auto found     = g.contents.find( buffer );
	upload( texture, found != g.contents.end() ? found->second : std::vector< unsigned char >() );
}

void glGetIntegerv( GLenum pname, GLint* data )
{
	switch( pname )
	{
	case GL_TEXTURE_BINDING_BUFFER:
	{
		const auto held = g.units.find( activeUnit() );
		*data           = held != g.units.end() ? static_cast< GLint >( held->second.texture ) : 0;
		return;
	}
	case GL_TEXTURE_BUFFER:
		*data = static_cast< GLint >( g.boundBuffer );
		return;
	case GL_MAX_TEXTURE_BUFFER_SIZE:
	{
		GLint side = 0;
		emscripten_glGetIntegerv( GL_MAX_TEXTURE_SIZE, &side );
		const long long texels = static_cast< long long >( kRowTexels ) * std::max( side, 1 );
		*data                  = static_cast< GLint >( std::min< long long >( texels, std::numeric_limits< GLint >::max() ) );
		return;
	}
	case GL_TEXTURE_BINDING_2D:
		if( !g.units.empty() )
		{
			const auto held = g.units.find( activeUnit() );
			if( held != g.units.end() )
			{
				*data = held->second.displaced;
				return;
			}
		}
		break;
	default: break;
	}
	emscripten_glGetIntegerv( pname, data );
}

void glDeleteBuffers( GLsizei n, const GLuint* buffers )
{
	for( GLsizei i = 0; i < n; ++i )
	{
		g.contents.erase( buffers[ i ] );
		if( g.boundBuffer == buffers[ i ] )
			g.boundBuffer = 0;
	}
	emscripten_glDeleteBuffers( n, buffers );
}

/// Deleting a texture unbinds it, as in GL: a buffer texture's unit gets its
/// displaced 2D texture back, and a displaced texture that is deleted is not
/// put back later.
void glDeleteTextures( GLsizei n, const GLuint* textures )
{
	for( GLsizei i = 0; i < n; ++i )
	{
		const GLuint name = textures[ i ];
		g.viewing.erase( name );
		for( auto it = g.units.begin(); it != g.units.end(); )
		{
			if( it->second.displaced == static_cast< GLint >( name ) )
				it->second.displaced = 0;
			if( it->second.texture != name )
			{
				++it;
				continue;
			}
			const GLint was = activeUnit();
			emscripten_glActiveTexture( static_cast< GLenum >( it->first ) );
			emscripten_glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( it->second.displaced ) );
			emscripten_glActiveTexture( static_cast< GLenum >( was ) );
			it = g.units.erase( it );
		}
	}
	emscripten_glDeleteTextures( n, textures );
}
} // extern "C"
