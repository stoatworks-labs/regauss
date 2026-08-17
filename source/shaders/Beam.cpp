#include "../Shaders.h"

namespace regauss::shaders
{
/// Where the beam actually lands.
///
/// This pass is the plugin. Everything else is a television built around it.
///
/// Three beams leave the gun, and a magnetic field bends each of them. Because
/// the three cathodes are not in the same place, the three beams do not travel
/// the same path, so they do not see quite the same field -- and every symptom
/// follows from that one sentence:
///
///   * All three displaced together is a **geometry error**: the picture leans,
///     bulges or shifts.
///   * The three displaced by *different* amounts is a **convergence error**:
///     coloured fringes on every edge. It is driven by the field's GRADIENT,
///     not its strength, which is why a fridge magnet on the glass fringes
///     savagely and the Earth's field does not fringe at all.
///   * Any displacement at all, measured in shadow-mask pitches, is a **purity
///     error**: the beam meant for the red stripe lands partly on the green
///     one. It is driven by the field's strength relative to the MASK PITCH,
///     which is why the same magnet ruins a fine-pitch monitor and barely
///     marks a coarse television.
///
/// None of the three is drawn. They are read out of one field.
namespace
{
const char* const kBeamBody = R"(
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 OutputSize;

//The two sensitivities. See Controls.h -- they are separate on purpose,
//because a real tube's two sensitivities are set by different hardware.
uniform float Deflection;
uniform float PurityGainX;
uniform float PurityGainY;

uniform float GunSeparation;
uniform float GunTriangular;//delta gun (1) or in-line gun (0)

uniform float StaticAmp;    //what the mask itself is holding
uniform float AcAmp;        //ambient interference plus whatever the coil is doing
uniform float Frequency;    //Hz
uniform float Time;         //seconds
uniform float ScanPeriod;   //seconds to paint one raster, top to bottom

uniform float Swell;        //HT sag pushes the picture outwards
uniform float Sag;          //...and dims it
uniform float Overscan;

in vec2 uv;

out vec4 fragColor;
)";

const char* const kBeamMain = R"(
void main()
{
	float aspect = OutputSize.x / max( OutputSize.y, 1.0 );

	//Picture coordinates, -1..1 on both axes, and the same point in SQUARE
	//units. The field lives in square units so that a round magnet makes a
	//round stain on a 16:9 composition; the picture lives in picture units
	//because that is what the texture is indexed by.
	vec2 pic = uv * 2.0 - 1.0;
	vec2 sq  = vec2( pic.x * aspect, pic.y );

	//----------------------------------------------------------------------
	// How strong the field is, right here, right now.
	//
	// The scan-position term is the one worth understanding. The beam paints
	// the raster from the top down over one frame period, so the bottom of the
	// picture is drawn a whole frame LATER than the top -- and if the field is
	// alternating, it has moved on by then. That is the entire mechanism
	// behind the slow vertical roll of mains interference: at exactly the
	// field rate every line sees the same phase and the bend is stationary,
	// and a hertz or two either side it crawls up or down the screen. Take
	// this term out and the picture flickers in place instead, which is what
	// interference has never once looked like.
	//----------------------------------------------------------------------
	float ac  = sin( 6.2831853 * Frequency * ( Time + ( 1.0 - uv.y ) * ScanPeriod ) );
	float amp = StaticAmp + AcAmp * ac;

	//----------------------------------------------------------------------
	// Three guns, three paths, three fields.
	//
	// A delta gun has its three cathodes on a triangle and an in-line gun has
	// them in a row, which is not a detail: it decides whether the fringing
	// comes out as a vertical red/blue split or as a three-way spray. The
	// choice follows the mask, because the tube was built as one thing -- a
	// delta shadow mask is fed by a delta gun and a grille by an in-line gun.
	//----------------------------------------------------------------------
	vec2 gun[ 3 ];
	if( GunTriangular > 0.5 )
	{
		gun[ 0 ] = vec2( 0.0, 1.0 ) * GunSeparation;
		gun[ 1 ] = vec2( -0.866, -0.5 ) * GunSeparation;
		gun[ 2 ] = vec2( 0.866, -0.5 ) * GunSeparation;
	}
	else
	{
		gun[ 0 ] = vec2( -1.0, 0.0 ) * GunSeparation;
		gun[ 1 ] = vec2( 0.0, 0.0 );
		gun[ 2 ] = vec2( 1.0, 0.0 ) * GunSeparation;
	}

	vec2 halfTexel = 0.5 / max( InputSize, vec2( 1.0 ) );

	vec3 sampled = vec3( 0.0 );
	vec3 landing = vec3( 0.0 );//each gun's error, in phosphor widths
	float alpha  = 0.0;
	float inside = 0.0;

	for( int c = 0; c < 3; ++c )
	{
		//The field this gun's beam flies through, which is NOT the field at
		//the point it is aimed at. In picture units, undoing the aspect
		//stretch the field was evaluated in.
		vec2 b     = fieldAt( sq + gun[ c ] ) * amp;
		vec2 unit  = vec2( b.x / aspect, b.y );

		//Geometry error and purity error read the SAME field through two
		//different sensitivities, which is the physical arrangement: how far a
		//given field bends the beam is set by the yoke and the anode voltage,
		//and how much of a colour error that bend causes is set by the mask
		//pitch and how far the gun sits from the mask. A weak magnet gives a
		//large purity error and no measurable geometry error at all, and that
		//is the commonest real fault there is -- so the two cannot be one
		//control with a fixed ratio between them.
		vec2 d = unit * Deflection;

		//The light at this point on the glass came from the beam that was
		//aimed a displacement back up the line. Overscan and the HT sag both
		//scale the raster: a set deliberately scans wider than its own tube so
		//the blanking edges hide behind the bezel, and a coil pulling the HT
		//down slows the electrons, which makes the same yoke current bend them
		//further. The picture visibly swells while it degausses.
		vec2 src = ( pic - d ) / max( Overscan * ( 1.0 + Swell ), 0.05 );
		vec2 st  = src * 0.5 + 0.5;

		//Clamp in PICTURE space, then scale by MaxUV at the fetch. The host's
		//texture can be larger than the picture drawn into it, and a warp that
		//samples a scaled coordinate walks into that undrawn padding.
		vec2 fetch = clamp( st, halfTexel, 1.0 - halfTexel ) * MaxUV;
		vec4 texel = texture( InputTexture, fetch );

		sampled[ c ] = texel[ c ];

		//How far off its own stripe this gun landed, in phosphor widths. The
		//vertical term is zero for a mask with no vertical structure -- an
		//aperture grille's stripes run the full height of the tube, so a
		//vertical landing error moves the beam along its own stripe and costs
		//nothing. PurityGainY carries that, and it is why a Trinitron survives
		//a magnet that stains a shadow-mask set.
		landing[ c ] = unit.x * PurityGainX + unit.y * PurityGainY;

		if( c == 1 )
		{
			//Alpha and the edge test come from the centre gun. There is no
			//picture outside the raster, and stretching the edge texel across
			//the gap reads as a smear rather than as an overscanned set.
			alpha = texel.a;

			vec2 over = max( vec2( 0.0 ) - st, st - vec2( 1.0 ) );
			inside    = 1.0 - clamp( max( over.x, over.y ) * 40.0, 0.0, 1.0 );
		}
	}

	//----------------------------------------------------------------------
	// Purity: which phosphor each gun actually hit.
	//
	// A gun is aimed at its own stripe. Displaced by one whole phosphor width
	// it lands squarely on its neighbour's, and the picture's colours rotate:
	// this is why a magnetised corner goes green rather than merely dim. In
	// between it lands across two, and the two share the light in proportion.
	//
	// The tent is exactly one phosphor wide, which makes this the identity
	// matrix at zero field. Any wider and the plugin would desaturate the
	// picture while claiming to do nothing, which is the sort of thing nobody
	// notices until it is in a show.
	//----------------------------------------------------------------------
	vec3 color = vec3( 0.0 );
	for( int c = 0; c < 3; ++c )
	{
		for( int k = 0; k < 3; ++k )
		{
			float d = float( c ) + landing[ c ] - float( k );
			d -= 3.0 * round( d / 3.0 );//the stripes repeat every triad
			color[ k ] += sampled[ c ] * max( 0.0, 1.0 - abs( d ) );
		}
	}

	//A coil that big pulls the HT down and the whole picture with it.
	color *= 1.0 - Sag * 0.55;

	color *= inside;
	alpha *= inside;

	fragColor = vec4( color, alpha );
}
)";
} // namespace

/// Assembled from the same `kFieldUniforms` and `kFieldFunction` the test
/// probe uses, so `rgtest --field` is checking the field this pass actually
/// evaluates rather than a copy of it that has been allowed to drift.
std::string BeamFragmentSource()
{
	return std::string( "#version 410 core\n" ) + kFieldUniforms + kBeamBody + kFieldFunction + kBeamMain;
}

} // namespace regauss::shaders
