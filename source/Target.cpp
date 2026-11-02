#include "Target.h"

#include <algorithm>

namespace knockout
{
namespace
{
/// The upload format and type that go with an internal format.
void uploadFormatFor( GLint internalFormat, GLenum& format, GLenum& type )
{
	switch( internalFormat )
	{
	case GL_R16F:
	case GL_R32F:
		format = GL_RED;
		type   = GL_FLOAT;
		return;
	default:
		format = GL_RGBA;
		type   = GL_FLOAT;
		return;
	}
}

/// The bindings a target operation disturbs, put back on scope exit.
struct SavedState
{
	GLint framebuffer   = 0;
	GLint texture       = 0;
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	GLfloat clear[ 4 ]  = { 0, 0, 0, 0 };

	SavedState()
	{
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &framebuffer );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
	}
	~SavedState()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( framebuffer ) );
		glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( texture ) );
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glClearColor( clear[ 0 ], clear[ 1 ], clear[ 2 ], clear[ 3 ] );
	}
};

void applySampling( bool linear, bool mipmapped, bool wrapX )
{
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
	                 mipmapped ? GL_LINEAR_MIPMAP_LINEAR : ( linear ? GL_LINEAR : GL_NEAREST ) );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear || mipmapped ? GL_LINEAR : GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapX ? GL_REPEAT : GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
}
} // namespace

int TopMipLevel( int width, int height )
{
	int level = 0;
	for( int size = std::max( width, height ); size > 1; size /= 2 )
		++level;
	return level;
}

void SetHeightfieldSampling( GLuint texture, bool wrapX )
{
	GLint previous = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previous );
	glBindTexture( GL_TEXTURE_2D, texture );
	applySampling( true, true, wrapX );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previous ) );
}

bool Target::Ensure( int requestedWidth, int requestedHeight, const Spec& requested )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;
	if( framebuffer != 0 && width == requestedWidth && height == requestedHeight && spec == requested )
		return true;

	Destroy();
	SavedState saved;
	width    = requestedWidth;
	height   = requestedHeight;
	spec     = requested;
	topLevel = spec.mipmapped ? TopMipLevel( width, height ) : 0;

	GLenum uploadFormat = GL_RGBA, uploadType = GL_FLOAT;
	uploadFormatFor( spec.format, uploadFormat, uploadType );
	const int count = std::clamp( spec.attachments, 1, 2 );

	glGenFramebuffers( 1, &framebuffer );
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	GLenum buffers[ 2 ] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	for( int i = 0; i < count; ++i )
	{
		glGenTextures( 1, &textures[ i ] );
		glBindTexture( GL_TEXTURE_2D, textures[ i ] );
		glTexImage2D( GL_TEXTURE_2D, 0, spec.format, width, height, 0, uploadFormat, uploadType, nullptr );
		if( spec.mipmapped )
			glGenerateMipmap( GL_TEXTURE_2D );
		applySampling( spec.linear, spec.mipmapped, spec.wrapX );
		glFramebufferTexture2D( GL_FRAMEBUFFER, buffers[ i ], GL_TEXTURE_2D, textures[ i ], 0 );
	}
	glDrawBuffers( count, buffers );
	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	if( !complete )
	{
		Destroy();
		return false;
	}
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	return true;
}

void Target::Destroy()
{
	if( framebuffer )
		glDeleteFramebuffers( 1, &framebuffer );
	for( GLuint& texture : textures )
		if( texture )
		{
			glDeleteTextures( 1, &texture );
			texture = 0;
		}
	framebuffer = 0;
	width = height = topLevel = 0;
}

void Target::Bind() const
{
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glViewport( 0, 0, width, height );
}

void Target::GenerateMips() const
{
	if( !spec.mipmapped )
		return;
	GLint previous = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previous );
	glBindTexture( GL_TEXTURE_2D, textures[ 0 ] );
	glGenerateMipmap( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previous ) );
}

void Target::Clear() const
{
	SavedState saved;
	Bind();
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
}

} // namespace knockout
