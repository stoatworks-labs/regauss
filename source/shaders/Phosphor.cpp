#include "../Shaders.h"

namespace regauss::shaders
{
/// Emissive decay.
///
/// It sits after the beam pass rather than before it, and that ordering is the
/// only interesting thing about this shader. The phosphor is a coating on the
/// inside of the glass: it glows where the beam hit it, not where the beam was
/// aimed. So a picture being shaken by a magnet leaves its trail along the
/// path the beam was actually dragged through, which is what makes a violent
/// degauss read as the picture being physically thrown about rather than as a
/// still frame with a wobble applied to it.
///
/// Per-channel decay because the three phosphors are three different
/// compounds. Blue goes out first and green hangs on longest, so a white
/// object dragged across a tube leaves a faintly green wake.
const char* const kPhosphorFragment = R"(#version 410 core
uniform sampler2D CurrentTexture;
uniform sampler2D HistoryTexture;
uniform vec3 Decay;

in vec2 uv;

out vec4 fragColor;

void main()
{
	//Both textures are ours, so both are exactly the size of the picture and
	//neither needs MaxUV.
	vec4 current = texture( CurrentTexture, uv );
	vec4 history = texture( HistoryTexture, uv );

	//Emission adds; it does not blend. A phosphor that is still glowing from
	//the last frame does not stop the new beam lighting it further, and a mix()
	//here would dim every moving highlight instead of trailing it.
	vec3 glow = max( current.rgb, history.rgb * Decay );

	fragColor = vec4( glow, max( current.a, history.a * Decay.g ) );
}
)";
} // namespace regauss::shaders
