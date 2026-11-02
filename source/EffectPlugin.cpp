#include "Knockout.h"

/**
    The registration, in a file of its own: CFFGLPluginInfo registers from a
    file-scope constructor, so this is listed in the MODULE target, never in
    the core library, where nothing would reference it and the linker could
    drop it (CMakeLists.txt says more).
*/
static CFFGLPluginInfo PluginInfo(
	PluginFactory< knockout::KnockoutPlugin >,// Create method
	"KO01",                                   // Plugin unique ID of maximum length 4
	"SW Knockout",                            // Plugin name
	2,                                        // API major version number
	1,                                        // API minor version number
	0,                                        // Plugin major version number
	1,                                        // Plugin minor version number
	FF_EFFECT,                                // Plugin type
	"Your words, in any installed font, as a mask over the clip: the clip shows through the letters, or the letters "
	"are cut out of it. Cut from the font's own curves every frame, so a word stays sharp from across the frame to a "
	"thousand-times zoom into one stroke. Grow, Feather, Outline and a letter-by-letter Reveal; Mix brings the clip "
	"back.",
	"Knockout FFGL effect"// About
);

extern "C" const char* KnockoutBuildStamp()
{
	return "knockout " KNOCKOUT_VERSION ", built " __DATE__ " " __TIME__;
}
