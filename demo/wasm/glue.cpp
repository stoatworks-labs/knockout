/*
    The page's way in to the plugin: what an FFGL host does to a plugin
    instance, as C functions JavaScript can call.

    Nothing here is the plugin, and nothing here does the plugin's work. The
    plugin is `source/Knockout.cpp` and everything it calls (Fonts, Outline,
    Layout, Target, Controls, Shaders, Diag), compiled UNMODIFIED into the same
    WebAssembly module as this file, with the FFGL SDK pieces it needs (see
    demo/tools/build-wasm.sh for the list). This file only plays the host:

      - it constructs a `KnockoutPlugin`, as the SDK's plugMain would through
        the CFFGLPluginInfo that EffectPlugin.cpp registers, and calls InitGL /
        DeInitGL;
      - it reads the parameter declarations back through the FFGL SDK's own
        host-facing getters (GetParamName, GetParamType, GetParamGroup,
        GetParamDefault, the element and file-extension getters -- the SDK
        compiled unmodified too), so the page's panel is built from the
        plugin's constructor rather than from a copy of it;
      - it forwards the host's calls: SetFloatParameter, SetTextParameter and
        ProcessOpenGL with one input texture, as tools/kotest's Rig does. The
        plugin keeps no clock, so there is no SetTime.

    Before the first instance, `ko_prepare` points the plugin's font scan
    (KNOCKOUT_FONT_DIR, read by Fonts.cpp) at the folder the page has written
    its bundled fonts into, and its log (KNOCKOUT_LOG_DIR, Diag.cpp) at another.
    The scan runs once, in the first constructor, as in a host.

    Two read-outs use the plugin's harness accessor (LastFrame) or its own
    conversions (Controls.cpp); they report and never change anything.

    GL calls go to the page's WebGL2 context through emscripten's GL library;
    demo/wasm/gl_shim.cpp replaces the entry points that cannot be passed
    straight through.
*/
#include "Controls.h"
#include "Diag.h"
#include "Knockout.h"

#include <emscripten/emscripten.h>

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

using namespace knockout;

namespace
{
/// One plugin instance and the host-side structs it is handed.
struct Instance
{
	KnockoutPlugin plugin;
	bool initialised = false;

	FFGLTextureStruct input {};
	FFGLTextureStruct* inputs[ 1 ] = { &input };
	ProcessOpenGLStruct process {};

	std::string scratch;///< a string handed back to JS lives here until the next call
};

float asFloat( FFMixed mixed )
{
	float value = 0.0f;
	static_assert( sizeof( value ) == sizeof( mixed.UIntValue ), "FFMixed carries a float in its bits" );
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

const char* hold( Instance* instance, std::string text )
{
	instance->scratch = std::move( text );
	return instance->scratch.c_str();
}

constexpr double kNaN = std::numeric_limits< double >::quiet_NaN();
} // namespace

extern "C"
{
//---------------------------------------------------------------------------
// Before the first ko_new: where the fonts are (the page writes them there
// first) and where the log goes. Diag.cpp makes its folder with `mkdir -p`
// through system(), which a browser does not have, so it is made here.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE void ko_prepare( const char* fontDirectory )
{
	setenv( "KNOCKOUT_FONT_DIR", fontDirectory, 1 );
	setenv( "KNOCKOUT_LOG_DIR", "/knockout/logs", 1 );
	mkdir( "/knockout", 0755 );
	mkdir( "/knockout/logs", 0755 );
}

EMSCRIPTEN_KEEPALIVE const char* ko_log_path()
{
	static std::string path;
	path = diag::logPath();
	return path.c_str();
}

//---------------------------------------------------------------------------
// Lifetime.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE Instance* ko_new()
{
	return new Instance();
}

EMSCRIPTEN_KEEPALIVE void ko_delete( Instance* instance )
{
	if( instance == nullptr )
		return;
	if( instance->initialised )
		instance->plugin.DeInitGL();
	delete instance;
}

/// InitGL, with the viewport a host would pass. 1 on success.
EMSCRIPTEN_KEEPALIVE int ko_init_gl( Instance* instance, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.width        = static_cast< GLuint >( width );
	viewport.height       = static_cast< GLuint >( height );
	instance->initialised = instance->plugin.InitGL( &viewport ) == FF_SUCCESS;
	return instance->initialised ? 1 : 0;
}

//---------------------------------------------------------------------------
// The declarations, read as a host reads them.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int ko_param_count( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetNumParams() );
}

EMSCRIPTEN_KEEPALIVE const char* ko_param_name( Instance* instance, int index )
{
	const char* name = instance->plugin.GetParamName( static_cast< unsigned int >( index ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE int ko_param_type( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamType( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* ko_param_group( Instance* instance, int index )
{
	return hold( instance, instance->plugin.GetParamGroup( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a numeric parameter (FF_TYPE_STANDARD already
/// clamped into 0..1 by the SDK, as a host sees it).
EMSCRIPTEN_KEEPALIVE float ko_param_default( Instance* instance, int index )
{
	return asFloat( instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a text or file parameter.
EMSCRIPTEN_KEEPALIVE const char* ko_param_default_text( Instance* instance, int index )
{
	const FFMixed mixed = instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) );
	return mixed.PointerValue ? static_cast< const char* >( mixed.PointerValue ) : "";
}

EMSCRIPTEN_KEEPALIVE int ko_param_element_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumParamElements( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* ko_param_element_name( Instance* instance, int index, int element )
{
	const char* name = instance->plugin.GetParamElementName( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE float ko_param_element_value( Instance* instance, int index, int element )
{
	return asFloat( instance->plugin.GetParamElementDefault( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) ) );
}

EMSCRIPTEN_KEEPALIVE int ko_param_extension_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumFileParamExtensions( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* ko_param_extension( Instance* instance, int index, int extension )
{
	const char* text = instance->plugin.GetFileParamExtension( static_cast< unsigned int >( index ), static_cast< unsigned int >( extension ) );
	return text ? text : "";
}

EMSCRIPTEN_KEEPALIVE int ko_max_inputs( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetMaxInputs() );
}

//---------------------------------------------------------------------------
// The host's calls.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int ko_set_float( Instance* instance, int index, float value )
{
	return instance->plugin.SetFloatParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float ko_get_float( Instance* instance, int index )
{
	return instance->plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
}

EMSCRIPTEN_KEEPALIVE int ko_set_text( Instance* instance, int index, const char* value )
{
	return instance->plugin.SetTextParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* ko_get_text( Instance* instance, int index )
{
	const char* text = instance->plugin.GetTextParameter( static_cast< unsigned int >( index ) );
	return text ? text : "";
}

/// One frame: ProcessOpenGL with one input of clipWidth x clipHeight whose GL
/// name is `clip` (a texture the page registered with emscripten's GL
/// tables), drawing into whatever framebuffer and viewport the page has bound,
/// as a host's. 1 on success.
EMSCRIPTEN_KEEPALIVE int ko_process( Instance* instance, int clip, int clipWidth, int clipHeight )
{
	instance->input.Width = instance->input.HardwareWidth = static_cast< FFUInt32 >( clipWidth );
	instance->input.Height = instance->input.HardwareHeight = static_cast< FFUInt32 >( clipHeight );
	instance->input.Handle                                 = static_cast< GLuint >( clip );
	instance->process.numInputTextures                     = 1;
	instance->process.inputTextures                        = instance->inputs;
	instance->process.HostFBO                              = 0;
	return instance->plugin.ProcessOpenGL( &instance->process ) == FF_SUCCESS ? 1 : 0;
}

//---------------------------------------------------------------------------
// For the panel's read-outs: Controls.cpp's conversion for a parameter, the
// one Knockout.cpp's ProcessOpenGL applies to it. NaN where the plugin uses
// the host's 0..1 as it is (Zoom X/Y, Reveal, Reveal Spread, Mix).
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double ko_convert( int index, float v )
{
	switch( index )
	{
	case PT_SIZE: return SizeFromParam( v );
	case PT_MARGIN: return MarginFromParam( v );
	case PT_TRACKING: return TrackingFromParam( v );
	case PT_LINE_SPACING: return LineSpacingFromParam( v );
	case PT_POSITION_X:
	case PT_POSITION_Y: return PositionFromParam( v );
	case PT_ROTATION: return RotationFromParam( v );
	case PT_ZOOM: return ZoomFromParam( v );
	case PT_GROW: return GrowFromParam( v );
	case PT_FEATHER: return FeatherFromParam( v );
	case PT_OUTLINE: return OutlineFromParam( v );
	default: return kNaN;
	}
}

//---------------------------------------------------------------------------
// What the last frame drew with (LastFrame, the harness's accessor), for the
// status line. Numbers: 0 weight, 1 italic, 2 from Font File, 3 glyphs with
// ink, 4 instances drawn, 5 culled, 6 pieces in the table, 7 lines, 8 pixels
// per font unit, 9 zoom, 10 units per em. Text: 0 family, 1 style.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double ko_frame_number( Instance* instance, int what )
{
	const FrameState& f = instance->plugin.LastFrame();
	switch( what )
	{
	case 0: return f.weight;
	case 1: return f.italic ? 1.0 : 0.0;
	case 2: return f.fromFile ? 1.0 : 0.0;
	case 3: return f.glyphs;
	case 4: return f.drawn;
	case 5: return f.culled;
	case 6: return f.pieces;
	case 7: return f.lines;
	case 8: return f.unitPx;
	case 9: return f.zoom;
	case 10: return f.unitsPerEm;
	default: return kNaN;
	}
}

EMSCRIPTEN_KEEPALIVE const char* ko_frame_text( Instance* instance, int what )
{
	const FrameState& f = instance->plugin.LastFrame();
	return hold( instance, what == 0 ? f.family : what == 1 ? f.style : std::string() );
}

} // extern "C"
