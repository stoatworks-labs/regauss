#include "../Shaders.h"

namespace regauss::shaders
{
/// The pole uniforms, shared by the beam pass and the test probe.
const char* const kFieldUniforms = R"(
//The magnets: xy is position in square tube units, z is signed strength.
uniform vec3 Pole0;
uniform vec3 Pole1;
uniform vec3 Pole2;
uniform vec3 Pole3;
uniform float PoleHeight;
)";

/// The field itself.
///
/// This string is the GPU half of a pair. Its twin is `regauss::fieldAt()` in
/// Field.cpp, and the two have to agree or the OpenFX build and the browser
/// demo will quietly render a different magnet from the plugin.
///
/// The agreement is not left to a comment. `rgtest --field` assembles a probe
/// program around THIS string, renders the field into a float buffer, and
/// compares it against Field.cpp evaluated on the CPU at the same points. It
/// runs what the plugin runs, because it is the same characters.
const char* const kFieldFunction = R"(
//= mirrored: regauss::fieldAt() in Field.cpp. Checked by rgtest --field.
vec2 fieldAt( vec2 p )
{
	vec2 b = vec2( 0.0 );
	vec3 poles[ 4 ] = vec3[ 4 ]( Pole0, Pole1, Pole2, Pole3 );

	for( int i = 0; i < 4; ++i )
	{
		vec2 d = p - poles[ i ].xy;
		//The pole sits off the plane of the glass, so this is a real 3-D
		//distance and there is no singularity anywhere on the face. The
		//exponent is one rather than three halves -- this is the field
		//integrated along the beam's flight, not the field where it lands.
		//See Field.h.
		float r2 = dot( d, d ) + PoleHeight * PoleHeight;
		b += d * ( poles[ i ].z / r2 );
	}

	return b;
}
)";

/// The probe.
///
/// Writes the field at each point straight out as a colour, which is only
/// meaningful into a floating-point buffer -- the field is signed and its
/// magnitude runs past one near a pole, so an 8-bit target would clamp away
/// exactly the region worth checking. rgtest binds GL_RGBA32F for this.
///
/// The probe's coordinates are square tube units, the same space `fieldAt`
/// expects, with the aspect handed in so the harness can check a non-square
/// output too.
std::string FieldProbeSource()
{
	return std::string( "#version 410 core\n" ) + kFieldUniforms + R"(
uniform float Aspect;

in vec2 uv;
out vec4 fragColor;
)" + kFieldFunction + R"(
void main()
{
	vec2 pic = uv * 2.0 - 1.0;
	vec2 sq  = vec2( pic.x * Aspect, pic.y );
	fragColor = vec4( fieldAt( sq ), 0.0, 1.0 );
}
)";
}

} // namespace regauss::shaders
