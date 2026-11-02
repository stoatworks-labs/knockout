#pragma once

#include "Controls.h"
#include "Fonts.h"
#include "Layout.h"
#include "Target.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

namespace knockout
{
/**
    What a frame drew with, for the harness and the log. Everything a check
    needs to know where the plugin put the text, so it can measure the
    picture against it -- and, for the checks of where it put it, enough to
    show the harness's own prediction was compared with something.
*/
struct FrameState
{
	int width = 0, height = 0;
	int unitsPerEm = 1000;
	std::string family, style;
	int weight = 400;
	bool italic = false;
	bool fromFile = false;

	double fitScale = 0.0;///< pixels per font unit before Zoom
	double unitPx   = 0.0;///< pixels per font unit, Zoom included
	double rotation = 0.0;///< radians
	double zoom     = 1.0;
	double inkX0 = 0, inkY0 = 0, inkX1 = 0, inkY1 = 0;///< font units
	double offsetX = 0, offsetY = 0;                  ///< pixels, Position
	double zoomX = 0, zoomY = 0;                      ///< pixels, Zoom X/Y
	double rampPx = 1.0, growPx = 0.0, outlinePx = 0.0, reachPx = 0.0;

	int glyphs  = 0;///< glyphs with ink in the setting
	int drawn   = 0;///< instances drawn this frame
	int culled  = 0;///< off the screen, or not yet revealed
	int pieces  = 0;///< in the uploaded table
	int lines   = 0;
	std::vector< double > alphas;///< each glyph's reveal, in reading order

	/// The screen pixel (y up, the raster's) a point of the setting lands on.
	Vec2 ScreenFromLayout( Vec2 layout ) const;
};

/**
    The plugin: an effect, one input. The text is set on the CPU when it,
    the face or the layout changes; its outlines go to the GPU as a table
    of pieces once per face; each frame only the transform and one
    instance per glyph are worked out (in double, so a thousand-times zoom
    stays put) and the mask is drawn from the curves.
*/
class KnockoutPlugin : public CFFGLPlugin
{
public:
	KnockoutPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	/// The base class's stub fails, and a failed default deletes the instance
	/// -- so without this no real host can load the plugin.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	unsigned int ParamCount() const
	{
		return PT_COUNT;
	}

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these.
	//-------------------------------------------------------------------
	enum TestFlag : int
	{
		kTestEvenOdd    = 1, ///< the shader's (Shaders.h)
		kTestAllPieces  = 2, ///< the shader's
		kTestWideRamp   = 4, ///< the edge ramped over two pixels, not one
		kTestNoKerning  = 8, ///< the setting without the face's kerning
		kTestGrowInward = 16,///< Grow's sign flipped
		kTestOverlaps   = 32,///< outlines built without resolving overlaps
		kTestFloatCentre = 64,///< each glyph's centre worked out in float, as a shader would
	};
	void SetTestFlagsForTest( int flags );
	const FrameState& LastFrame() const
	{
		return frameState;
	}
	GLuint MaskTextureID() const
	{
		return mask.Texture();
	}
	const Typesetter& SetterForTest() const
	{
		return setter;
	}
	const Setting& SettingForTest() const
	{
		return setting;
	}
	std::string TextValue( unsigned int id );

private:
	void resolveFont();
	void relayout();
	void uploadPieces();
	void releaseBuffers();

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader glyphShader, compositeShader;
	ffglex::FFGLScreenQuad quad;
	Target mask;
	GLuint emptyVAO = 0;
	GLuint pieceBuffer = 0, pieceTexture = 0;
	GLuint instanceBuffer = 0, instanceTexture = 0;

	//The text fields, which a host may set from another thread than the one
	//it renders on.
	std::mutex textMutex;
	std::string text = "KNOCKOUT", fontName, fontFile;
	bool textDirty      = true;
	bool fontNameSet    = false;
	bool fontIndexMoved = false;
	std::string textReturn;

	Font font;
	Typesetter setter;
	Setting setting;
	std::u32string decoded;
	bool fontDirty   = true;
	bool layoutDirty = true;
	bool piecesDirty = true;
	std::vector< int > pieceFirst, pieceCount;
	int uploadedOutlines = 0;
	std::vector< float > instanceData;

	FrameState frameState;
	int testFlags = 0;
};

} // namespace knockout
