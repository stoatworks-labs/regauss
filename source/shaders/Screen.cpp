#include "../Shaders.h"

namespace regauss::shaders
{
/// The glass.
///
/// Where the light coming off the phosphor stops being a signal and becomes a
/// television you are looking at from somewhere. The order is the physical
/// one: undo the view, undo the curvature, scan, then mask -- because the
/// phosphors are painted on the glass, so the mask lives in the tube's own
/// coordinates. It turns with the tube when you look at the set from an angle,
/// and it does not slide about when the picture moves.
///
/// ------------------------------------------------------- Interference Only
///
/// The whole pass is bypassed by a uniform branch when the operator has asked
/// for the interference without the television. That is a real use: (re)gauss
/// stacked on old-cathode, or on somebody's own CRT footage, wants the magnet
/// and nothing else -- two shadow masks in a chain is a moire generator, and
/// two sets of curvature is a fisheye.
///
/// The branch is on a uniform, so every pixel of every quad takes the same
/// side of it and the early return cannot leave a derivative undefined for a
/// neighbour that went the other way.
///
/// -------------------------------------------------------- premultiplication
///
/// Everything here is premultiplied. Anything that changes how much light
/// comes off the screen -- the mask, the scan, brightness -- scales the colour
/// alone, because it does not change what the picture covers. Anything that
/// changes coverage -- the bezel, the crop outside the raster -- scales colour
/// and alpha together. Contrast is written about a pivot of `0.5 * alpha`
/// rather than 0.5 for the same reason: half grey in premultiplied form is
/// half of the alpha, and pivoting on a bare 0.5 would tint every partly
/// transparent pixel.
const char* const kScreenFragment = R"(#version 410 core
uniform sampler2D ScreenTexture;
uniform sampler2D BloomTexture;
uniform vec2 OutputSize;

uniform float TubeEnabled;

uniform float MaskPattern;
uniform float MaskPitch;//output pixels per phosphor stripe
uniform float MaskStrength;
uniform float MaskSpill;
uniform float MaskGain;

uniform float Scanlines;
uniform float LineCount;
uniform float BeamBloom;
uniform float Halation;
uniform float Brightness;
uniform float Contrast;

uniform float Curvature;
uniform float CornerRadius;
uniform float PerspectiveX;
uniform float PerspectiveY;
uniform float Zoom;
uniform float Vignette;

in vec2 uv;

out vec4 fragColor;

const float FOCAL = 2.4;

mat3 rotationX( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( 1.0, 0.0, 0.0,
	             0.0, c, s,
	             0.0, -s, c );
}

mat3 rotationY( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( c, 0.0, -s,
	             0.0, 1.0, 0.0,
	             s, 0.0, c );
}

//---------------------------------------------------------------------------
// The phosphor layout, as the transmission of each of the three phosphors at
// this point. Coordinates are in phosphor widths, so one unit is one stripe in
// both axes whatever the pitch is set to -- which is the same unit the beam
// pass measured its landing error in, and they have to agree or the purity
// stain will not line up with the mask that is supposed to be causing it.
//---------------------------------------------------------------------------
vec3 dotMask( vec2 mc, int pattern )
{
	if( pattern == 1 )
	{
		//Delta shadow mask: round dots on a hexagonal lattice, every other row
		//offset by half a triad. The consumer television mask.
		const float rowHeight = 0.866;//sqrt(3)/2 -- what makes the lattice regular
		float row  = floor( mc.y / rowHeight );
		float x    = mc.x + mod( row, 2.0 ) * 0.5;
		float idx  = mod( floor( x ), 3.0 );
		vec2 cell  = vec2( fract( x ) - 0.5, ( fract( mc.y / rowHeight ) - 0.5 ) * rowHeight );
		float spot = 1.0 - smoothstep( 0.24, 0.46, length( cell ) );
		vec3 phos  = idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		return phos * spot;
	}

	if( pattern == 2 )
	{
		//Aperture grille: continuous vertical stripes held apart by tensioned
		//wires. No vertical structure at all -- which is exactly why the beam
		//pass leaves MaskStaggers at zero for this one, and why a Trinitron
		//shrugs off a vertical purity error that would stain a shadow mask.
		float idx    = mod( floor( mc.x ), 3.0 );
		float stripe = 1.0 - smoothstep( 0.26, 0.50, abs( fract( mc.x ) - 0.5 ) );
		vec3 phos    = idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		return phos * stripe;
	}

	if( pattern == 3 )
	{
		//Slot mask: stripes broken into slots so the sheet keeps its rigidity,
		//alternate triads staggered vertically. What most later sets used.
		const float slotHeight = 2.0;
		float idx     = mod( floor( mc.x ), 3.0 );
		float stagger = mod( floor( mc.x / 3.0 ), 2.0 ) * 0.5;
		float sy      = fract( mc.y / slotHeight + stagger );
		float stripe  = 1.0 - smoothstep( 0.28, 0.50, abs( fract( mc.x ) - 0.5 ) );
		float slot    = 1.0 - smoothstep( 0.38, 0.50, abs( sy - 0.5 ) );
		vec3 phos     = idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		return phos * stripe * slot;
	}

	if( pattern == 4 )
	{
		//Hard RGB stripe: no gap, no vertical structure. Not a mask any tube
		//ever had, but it is what a coarse subpixel grid looks like and it
		//stays legible at pitches where the others have turned to mush.
		float idx = mod( floor( mc.x ), 3.0 );
		return idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
	}

	return vec3( 1.0 );
}

void main()
{
	//----------------------------------------------------------------------
	// Interference Only: hand back what the beam and the phosphor produced.
	//----------------------------------------------------------------------
	if( TubeEnabled < 0.5 )
	{
		fragColor = texture( ScreenTexture, uv );
		return;
	}

	float aspect = OutputSize.x / max( OutputSize.y, 1.0 );

	//----------------------------------------------------------------------
	// 1. Undo the view: which point on the tube's face is this output pixel
	//    looking at? Everything after this is in the tube's own coordinates.
	//----------------------------------------------------------------------
	vec2 p = uv * 2.0 - 1.0;
	p.x *= aspect;//square units, so a rotation is a rotation

	mat3 orientation = rotationY( PerspectiveX ) * rotationX( PerspectiveY );
	vec3 dir     = vec3( p / max( Zoom, 0.05 ), FOCAL );
	vec3 normal  = orientation * vec3( 0.0, 0.0, 1.0 );
	vec3 centre  = vec3( 0.0, 0.0, FOCAL );

	//Guarded rather than branched: an early return here would leave the
	//derivatives below undefined for the whole quad, and both the scanline and
	//the mask anti-aliasing depend on them.
	float denom = dot( normal, dir );
	denom = denom >= 0.0 ? max( denom, 1e-4 ) : min( denom, -1e-4 );
	float t = dot( normal, centre ) / denom;

	vec3 local    = transpose( orientation ) * ( t * dir - centre );
	vec2 tube     = vec2( local.x / aspect, local.y );
	float infront = step( 1e-4, t );//the face is behind the eye at absurd angles

	//----------------------------------------------------------------------
	// 2. Undo the curvature, divided through by the expansion at the corner.
	//    Without that the distortion pulls the picture's own corners inside
	//    the glass and shows black beyond them, which is the one thing a
	//    correctly set-up television never does.
	//----------------------------------------------------------------------
	float cornerExpansion = 1.0 + Curvature;
	vec2 curved   = tube * ( 1.0 + Curvature * 0.5 * dot( tube, tube ) ) / cornerExpansion;
	vec2 screenUV = curved * 0.5 + 0.5;

	//----------------------------------------------------------------------
	// 3. Scan. The beam is a spot with a profile, not a row of squares, and it
	//    is fatter where it is brighter because more current defocuses it.
	//
	//    There is no authentic raster to take the line count from -- this
	//    plugin runs at the composition's resolution and models a magnet, not
	//    a broadcast standard -- so the operator says how many lines the set
	//    had, and the sampling follows from that.
	//----------------------------------------------------------------------
	float lines = max( LineCount, 1.0 );
	float lineF = screenUV.y * lines - 0.5;//integer at line centres
	float base  = floor( lineF );

	//Below roughly a pixel and a bit per line there is nothing left to draw
	//and the modulation is pure aliasing, so it fades out.
	float pixelsPerLine = 1.0 / max( fwidth( lineF ), 1e-5 );
	float scanAA = smoothstep( 1.2, 2.0, pixelsPerLine );

	vec3 beamSum   = vec3( 0.0 );
	float alphaSum = 0.0;
	float weightSum = 0.0;
	float weightFlat = 0.0;

	for( int i = -1; i <= 1; ++i )
	{
		float li = base + float( i );
		vec4 c   = texture( ScreenTexture, vec2( screenUV.x, ( li + 0.5 ) / lines ) );

		//A brighter line is a fatter line: more beam current means a bigger
		//spot, which is why highlights on a CRT swell into the gaps between
		//scan lines and shadows do not.
		float luma  = clamp( dot( abs( c.rgb ), vec3( 0.299, 0.587, 0.114 ) ), 0.0, 1.0 );
		float sigma = mix( 0.26, mix( 0.34, 0.95, BeamBloom ), luma );

		float d = lineF - li;
		float w = exp( -0.5 * d * d / ( sigma * sigma ) );
		//The same profile sampled dead-on: what the line would be worth if the
		//beam were centred here. Their ratio is the scan modulation and is
		//independent of the picture, so strength 0 returns the picture intact.
		float wFlat = exp( -0.5 * float( i ) * float( i ) / ( sigma * sigma ) );

		beamSum    += c.rgb * w;
		alphaSum   += c.a * w;
		weightSum  += w;
		weightFlat += wFlat;
	}

	vec3 color    = beamSum / max( weightSum, 1e-4 );
	float alpha   = alphaSum / max( weightSum, 1e-4 );
	float scanMod = weightSum / max( weightFlat, 1e-4 );
	color *= mix( 1.0, min( scanMod, 1.0 ), Scanlines * scanAA );

	//----------------------------------------------------------------------
	// Halation, then the picture controls, in that order: the front panel
	// adjusts the beam, and the glass scatters whatever the beam produced.
	//----------------------------------------------------------------------
	color += texture( BloomTexture, screenUV ).rgb * Halation;
	color = ( color - 0.5 * alpha ) * Contrast + 0.5 * alpha;
	color *= Brightness;

	//----------------------------------------------------------------------
	// 4. Mask, in tube coordinates.
	//----------------------------------------------------------------------
	int pattern = int( MaskPattern + 0.5 );
	vec2 maskCoord = ( tube * 0.5 + 0.5 ) * OutputSize / max( MaskPitch, 1.0 );

	//Once a phosphor is smaller than a pixel the mask is no longer a mask, it
	//is a moire generator. Fade it on the derivative rather than on a
	//resolution check, so it also does the right thing when perspective
	//shrinks the tube. The worst axis, not the length of the pair: the two are
	//independent and combining them would overstate the rate by root two.
	vec2 maskRate = fwidth( maskCoord );
	float maskAA = 1.0 - smoothstep( 0.4, 0.8, max( maskRate.x, maskRate.y ) );

	//The spot is wider than one phosphor, so its neighbours are always partly
	//lit. That floor is why a real mask reads as a texture over the picture
	//rather than as three separated primaries.
	vec3 shape     = mix( vec3( MaskSpill ), vec3( 1.0 ), dotMask( maskCoord, pattern ) );
	float strength = MaskStrength * maskAA;
	color *= mix( vec3( 1.0 ), shape * MaskGain, strength );

	//Damper wires. Only on a grille -- a perforated sheet holds itself up.
	//They are the giveaway that you are looking at a Trinitron.
	if( pattern == 2 )
	{
		float wire = 0.0;
		wire += 1.0 - smoothstep( 0.0, 3.2 / OutputSize.y, abs( tube.y - 0.36 ) );
		wire += 1.0 - smoothstep( 0.0, 3.2 / OutputSize.y, abs( tube.y + 0.36 ) );
		color *= 1.0 - clamp( wire, 0.0, 1.0 ) * 0.28 * strength;
	}

	//----------------------------------------------------------------------
	// The edge of the glass.
	//----------------------------------------------------------------------
	color *= 1.0 - Vignette * smoothstep( 0.25, 1.5, length( tube * vec2( 0.92, 1.0 ) ) );

	//A rounded rectangle in the tube's own coordinates, so the bezel keeps its
	//shape when the set is turned away from you.
	float radius = max( CornerRadius, 0.001 );
	vec2 q  = abs( tube ) - vec2( 1.0 - radius );
	float sd = length( max( q, vec2( 0.0 ) ) ) + min( max( q.x, q.y ), 0.0 ) - radius;
	float aa = max( fwidth( sd ), 1e-4 );
	float face = 1.0 - smoothstep( -aa, aa, sd );

	//Curvature can push the sample outside the raster before the face mask
	//cuts it off; there is no picture out there.
	vec2 outside = step( vec2( 0.0 ), -screenUV ) + step( vec2( 1.0 ), screenUV );
	face *= ( 1.0 - clamp( outside.x + outside.y, 0.0, 1.0 ) ) * infront;

	//Coverage, so both together.
	fragColor = vec4( clamp( color, vec3( 0.0 ), vec3( 1.0 ) ) * face, clamp( alpha, 0.0, 1.0 ) * face );
}
)";
} // namespace regauss::shaders
