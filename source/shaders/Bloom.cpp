#include "../Shaders.h"

namespace regauss::shaders
{
/// Halation: the bright pass.
///
/// Light leaving a phosphor goes in every direction, including sideways into
/// the faceplate, where some of it bounces off the front surface and comes
/// back out somewhere else. So a highlight on a CRT has a halo that a flat
/// panel does not, and it is a property of the glass rather than of the
/// signal.
///
/// Quarter size throughout. The scattering length is millimetres of glass,
/// which at any sane viewing distance is far wider than the sampling error a
/// quarter-resolution blur introduces.
const char* const kBloomFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform vec2 SourceSize;
uniform float Threshold;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec3 c = texture( SourceTexture, uv ).rgb;

	//Only what is bright enough to scatter visibly. Subtracting the threshold
	//rather than testing against it keeps the halo continuous as a highlight
	//fades, instead of switching on the frame it crosses the line.
	vec3 bright = max( c - vec3( Threshold ), vec3( 0.0 ) );

	fragColor = vec4( bright, 1.0 );
}
)";

/// The separable blur, run once horizontally and once vertically.
///
/// A nine-tap Gaussian taken at five positions: the pairs either side are
/// fetched half way between two texels so the hardware's linear filter does
/// the summing. Standard, and worth the comment only because the offsets look
/// wrong to anyone expecting integers.
const char* const kBlurFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform vec2 Direction;

in vec2 uv;

out vec4 fragColor;

void main()
{
	const float offsets[ 3 ] = float[ 3 ]( 0.0, 1.3846153846, 3.2307692308 );
	const float weights[ 3 ] = float[ 3 ]( 0.2270270270, 0.3162162162, 0.0702702703 );

	vec3 sum = texture( SourceTexture, uv ).rgb * weights[ 0 ];
	for( int i = 1; i < 3; ++i )
	{
		vec2 d = Direction * offsets[ i ];
		sum += texture( SourceTexture, uv + d ).rgb * weights[ i ];
		sum += texture( SourceTexture, uv - d ).rgb * weights[ i ];
	}

	fragColor = vec4( sum, 1.0 );
}
)";
} // namespace regauss::shaders
