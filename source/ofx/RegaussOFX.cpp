/// The OpenFX build of (re)gauss, for DaVinci Resolve, Vegas, Nuke, Natron and
/// other OFX hosts.
///
/// The model is not reimplemented here. `Field.cpp`, `Controls.cpp` and
/// `Masks.cpp` are linked straight from source, so the magnets, the coil, the
/// two sensitivities and the mask table are literally the same code the FFGL
/// build runs. What IS mirrored is the per-pixel half -- the beam pass and the
/// screen pass, constant for constant, from source/shaders/Beam.cpp and
/// source/shaders/Screen.cpp. Every mirrored line is marked `//= mirrored`.
/// Change a pass's GLSL, change the matching function here.
///
/// ------------------------------------------------- three deliberate departures
///
/// **There is no Degauss button.** OFX renders a timeline in whatever order it
/// likes, and a button press is an event at wall-clock time -- it cannot be
/// baked into a frame that might be rendered before the frame preceding it. So
/// the coil is scheduled instead: Auto on an interval, plus a "Degauss At"
/// offset that says where in the timeline the sequence starts. The FFGL build
/// does the identical arithmetic for its own Auto modes, so a composition set
/// up in Resolume and one graded in Resolve fire on the same instants.
///
/// **Beat and Bar need a tempo of their own.** FFGL gets bpm and bar phase
/// from the host every frame; OFX has no transport at all. Rather than drop
/// the two modes -- which would leave the preset table's element lists
/// disagreeing between the builds, and a preset would then select a mode that
/// does not exist -- there is a Tempo control here, and Beat and Bar work off
/// that.
///
/// **No Persistence and no Halation.** Both are feedback or blur over the
/// whole frame. The trail is only really visible during a degauss, and
/// reconstructing it through OFX temporal clip access -- which is what
/// old-cathode does for its phosphor -- is a great deal of machinery for it.
/// They are omitted rather than left in as dead controls.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Controls.h"
#include "../Field.h"
#include "../Masks.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.regauss";
constexpr const char* kPluginName       = "(re)gauss";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A magnet near a television. One vector field over the tube's face, read "
	"once per electron gun: the picture leans, every edge grows coloured "
	"fringes where the field has a gradient, and whole regions turn the wrong "
	"colour where the beam lands off its own phosphor stripe. None of the "
	"three is drawn -- they are consequences of the one field.\n\n"
	"The degauss coil applies a decaying alternating field and walks the "
	"mask's magnetisation down with it, so firing it genuinely clears what "
	"has built up -- and the picture swells and dims while the coil loads the "
	"HT, the way a real set does.\n\n"
	"https://stoatworks-labs.com";

//---------------------------------------------------------------------------
// Parameter names. Shared with nothing -- OFX names are this build's business
// -- but the ORDER of the preset binding below is shared, and asserted.
//---------------------------------------------------------------------------
const char* const kParamPreset       = "preset";
const char* const kParamMode         = "mode";
const char* const kParamLayout       = "layout";
const char* const kParamMagnetisation = "magnetisation";
const char* const kParamSeed         = "seed";
const char* const kParamWander       = "wander";
const char* const kParamInterference = "interference";
const char* const kParamFrequency    = "frequency";
const char* const kParamDeflection   = "deflection";
const char* const kParamPurity       = "purity";
const char* const kParamConvergence  = "convergence";
const char* const kParamOverscan     = "overscan";
const char* const kParamAuto         = "autoMode";
const char* const kParamTempo        = "tempo";
const char* const kParamDegaussAt    = "degaussAt";
const char* const kParamInterval     = "interval";
const char* const kParamDuration     = "duration";
const char* const kParamIntensity    = "intensity";
const char* const kParamCoilSag      = "coilSag";
const char* const kParamRecovery     = "recovery";
const char* const kParamMaskPattern  = "maskPattern";
const char* const kParamMaskPitch    = "maskPitch";
const char* const kParamMaskStrength = "maskStrength";
const char* const kParamScanlines    = "scanlines";
const char* const kParamLineCount    = "lineCount";
const char* const kParamBeamBloom    = "beamBloom";
const char* const kParamBrightness   = "brightness";
const char* const kParamContrast     = "contrast";
const char* const kParamCurvature    = "curvature";
const char* const kParamCornerRadius = "cornerRadius";
const char* const kParamPerspectiveX = "perspectiveX";
const char* const kParamPerspectiveY = "perspectiveY";
const char* const kParamZoom         = "zoom";
const char* const kParamVignette     = "vignette";

constexpr float TAU = 6.28318530718f;

//---------------------------------------------------------------------------
// GLSL built-ins, spelled out.
//---------------------------------------------------------------------------
float clampf( float v, float lo, float hi )
{
	return v < lo ? lo : ( v > hi ? hi : v );
}

float mixf( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

float smoothstepf( float e0, float e1, float x )
{
	const float t = clampf( ( x - e0 ) / ( e1 - e0 ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

float fractf( float v )
{
	return v - std::floor( v );
}

/// GLSL `mod`, which is not fmod for negatives.
float modf_( float x, float y )
{
	return x - y * std::floor( x / y );
}

//---------------------------------------------------------------------------
/// A float RGBA plane, which is what both passes read.
struct Plane
{
	std::vector< float > data;
	int w = 0;
	int h = 0;

	void allocate( int width, int height )
	{
		w = width;
		h = height;
		data.assign( static_cast< size_t >( w ) * h * 4, 0.0f );
	}

	float* at( int x, int y )
	{
		return data.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
	}

	const float* at( int x, int y ) const
	{
		return data.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
	}
};

/// Bilinear fetch with clamping, matching GL_LINEAR + GL_CLAMP_TO_EDGE.
void bilinear4( const Plane& p, float u, float v, float out[ 4 ] )
{
	const float x = clampf( u * p.w - 0.5f, 0.0f, static_cast< float >( p.w - 1 ) );
	const float y = clampf( v * p.h - 0.5f, 0.0f, static_cast< float >( p.h - 1 ) );

	const int x0 = static_cast< int >( x );
	const int y0 = static_cast< int >( y );
	const int x1 = std::min( x0 + 1, p.w - 1 );
	const int y1 = std::min( y0 + 1, p.h - 1 );
	const float fx = x - x0;
	const float fy = y - y0;

	const float* a = p.at( x0, y0 );
	const float* b = p.at( x1, y0 );
	const float* c = p.at( x0, y1 );
	const float* d = p.at( x1, y1 );

	for( int i = 0; i < 4; ++i )
		out[ i ] = mixf( mixf( a[ i ], b[ i ], fx ), mixf( c[ i ], d[ i ], fx ), fy );
}

//---------------------------------------------------------------------------
/// Everything both passes need, worked out once per frame.
struct FrameSetup
{
	regauss::controls::Drive drive;

	float outW = 1.0f;
	float outH = 1.0f;
	float aspect = 1.0f;
	float time = 0.0f;
	float scanPeriod = 1.0f / 25.0f;

	bool tubeEnabled = true;

	int maskPattern     = 1;
	float maskPitch     = 5.6f;
	float maskStrength  = 0.6f;
	float maskSpill     = 0.35f;
	float maskGain      = 2.235f;

	float scanlines    = 0.5f;
	float lineCount    = 480.0f;
	float beamBloom    = 0.5f;
	float brightness   = 1.0f;
	float contrast     = 1.0f;

	float curvature    = 0.168f;
	float cornerRadius = 0.056f;
	float perspectiveX = 0.0f;
	float perspectiveY = 0.0f;
	float zoom         = 1.0f;
	float vignette     = 0.35f;

	Plane source;//the incoming picture, premultiplied
	Plane beam;  //what the beam pass produced
};

//===========================================================================
// The beam pass.
//
//= mirrored: source/shaders/Beam.cpp. Change one, change both.
//===========================================================================
void beamAt( const FrameSetup& s, float u, float v, float out[ 4 ] )
{
	const float aspect = s.aspect;

	const float picX = u * 2.0f - 1.0f;
	const float picY = v * 2.0f - 1.0f;
	const float sqX  = picX * aspect;
	const float sqY  = picY;

	//= mirrored: the scan-position term. The beam paints top-down over one
	//frame, so the bottom of the picture is drawn a frame later than the top
	//and an alternating field has moved on by then.
	const float ac  = std::sin( TAU * s.drive.frequency * ( s.time + ( 1.0f - v ) * s.scanPeriod ) );
	const float amp = s.drive.staticAmp + s.drive.acAmp * ac;

	//= mirrored: three cathodes, on a triangle or in a row.
	float gunX[ 3 ], gunY[ 3 ];
	const float sep = s.drive.gunSeparation;
	if( s.drive.gunTriangular > 0.5f )
	{
		gunX[ 0 ] = 0.0f * sep;      gunY[ 0 ] = 1.0f * sep;
		gunX[ 1 ] = -0.866f * sep;   gunY[ 1 ] = -0.5f * sep;
		gunX[ 2 ] = 0.866f * sep;    gunY[ 2 ] = -0.5f * sep;
	}
	else
	{
		gunX[ 0 ] = -sep; gunY[ 0 ] = 0.0f;
		gunX[ 1 ] = 0.0f; gunY[ 1 ] = 0.0f;
		gunX[ 2 ] = sep;  gunY[ 2 ] = 0.0f;
	}

	float sampled[ 3 ] = { 0.0f, 0.0f, 0.0f };
	float landing[ 3 ] = { 0.0f, 0.0f, 0.0f };
	float alpha = 0.0f;
	float inside = 0.0f;

	for( int c = 0; c < 3; ++c )
	{
		float bx = 0.0f, by = 0.0f;
		regauss::fieldAt( s.drive.poles, sqX + gunX[ c ], sqY + gunY[ c ], bx, by );
		bx *= amp;
		by *= amp;

		const float unitX = bx / aspect;
		const float unitY = by;

		const float dX = unitX * s.drive.deflection;
		const float dY = unitY * s.drive.deflection;

		const float scale = std::max( s.drive.overscan * ( 1.0f + s.drive.swell ), 0.05f );
		const float srcX  = ( picX - dX ) / scale;
		const float srcY  = ( picY - dY ) / scale;
		const float stX   = srcX * 0.5f + 0.5f;
		const float stY   = srcY * 0.5f + 0.5f;

		//= mirrored: clamp half a texel inside, in picture space.
		const float halfTexelX = 0.5f / std::max( static_cast< float >( s.source.w ), 1.0f );
		const float halfTexelY = 0.5f / std::max( static_cast< float >( s.source.h ), 1.0f );

		float texel[ 4 ];
		bilinear4( s.source,
		           clampf( stX, halfTexelX, 1.0f - halfTexelX ),
		           clampf( stY, halfTexelY, 1.0f - halfTexelY ),
		           texel );

		sampled[ c ] = texel[ c ];
		landing[ c ] = unitX * s.drive.purityGainX + unitY * s.drive.purityGainY;

		if( c == 1 )
		{
			alpha = texel[ 3 ];
			const float overX = std::max( -stX, stX - 1.0f );
			const float overY = std::max( -stY, stY - 1.0f );
			inside = 1.0f - clampf( std::max( overX, overY ) * 40.0f, 0.0f, 1.0f );
		}
	}

	//= mirrored: which phosphor each gun actually hit. A tent exactly one
	//phosphor wide, so this is the identity matrix at zero field.
	float colour[ 3 ] = { 0.0f, 0.0f, 0.0f };
	for( int c = 0; c < 3; ++c )
	{
		for( int k = 0; k < 3; ++k )
		{
			float d = static_cast< float >( c ) + landing[ c ] - static_cast< float >( k );
			d -= 3.0f * std::round( d / 3.0f );
			colour[ k ] += sampled[ c ] * std::max( 0.0f, 1.0f - std::fabs( d ) );
		}
	}

	const float dim = 1.0f - s.drive.sag * 0.55f;
	for( int k = 0; k < 3; ++k )
		out[ k ] = colour[ k ] * dim * inside;
	out[ 3 ] = alpha * inside;
}

//===========================================================================
// The screen pass.
//
//= mirrored: source/shaders/Screen.cpp.
//===========================================================================

/// Where on the tube's face this output pixel is looking, and the quantities
/// the anti-aliasing needs derivatives of.
struct Geometry
{
	float tubeX, tubeY;
	float screenU, screenV;
	float lineF;
	float maskX, maskY;
	float sd;
	float infront;
};

Geometry geometryAt( float u, float v, const FrameSetup& s,
                     float cosRX, float sinRX, float cosRY, float sinRY )
{
	Geometry g {};

	const float aspect = s.aspect;
	float pX = ( u * 2.0f - 1.0f ) * aspect;
	float pY = v * 2.0f - 1.0f;

	//The orientation matrix is rotationY(px) * rotationX(py); applied to
	//(0,0,1) it gives the face normal, and its transpose takes a world point
	//into tube coordinates. Written out rather than carrying a matrix class.
	const float FOCAL = 2.4f;

	const float dirX = pX / std::max( s.zoom, 0.05f );
	const float dirY = pY / std::max( s.zoom, 0.05f );
	const float dirZ = FOCAL;

	//normal = rotationY(rx) * rotationX(ry) * (0,0,1)
	const float nX = -sinRY * cosRX;
	const float nY = sinRX;
	const float nZ = cosRY * cosRX;

	float denom = nX * dirX + nY * dirY + nZ * dirZ;
	denom = denom >= 0.0f ? std::max( denom, 1e-4f ) : std::min( denom, -1e-4f );
	const float t = ( nZ * FOCAL ) / denom;

	const float wX = t * dirX;
	const float wY = t * dirY;
	const float wZ = t * dirZ - FOCAL;

	//transpose(orientation) * w
	const float localX = cosRY * wX + sinRY * wZ;
	const float localY = sinRY * sinRX * wX + cosRX * wY - cosRY * sinRX * wZ;

	g.tubeX   = localX / aspect;
	g.tubeY   = localY;
	g.infront = t > 1e-4f ? 1.0f : 0.0f;

	const float cornerExpansion = 1.0f + s.curvature;
	const float rr = g.tubeX * g.tubeX + g.tubeY * g.tubeY;
	const float curvedX = g.tubeX * ( 1.0f + s.curvature * 0.5f * rr ) / cornerExpansion;
	const float curvedY = g.tubeY * ( 1.0f + s.curvature * 0.5f * rr ) / cornerExpansion;

	g.screenU = curvedX * 0.5f + 0.5f;
	g.screenV = curvedY * 0.5f + 0.5f;

	g.lineF = g.screenV * std::max( s.lineCount, 1.0f ) - 0.5f;

	const float pitch = std::max( s.maskPitch, 1.0f );
	g.maskX = ( g.tubeX * 0.5f + 0.5f ) * s.outW / pitch;
	g.maskY = ( g.tubeY * 0.5f + 0.5f ) * s.outH / pitch;

	const float radius = std::max( s.cornerRadius, 0.001f );
	const float qX = std::fabs( g.tubeX ) - ( 1.0f - radius );
	const float qY = std::fabs( g.tubeY ) - ( 1.0f - radius );
	const float mx = std::max( qX, 0.0f );
	const float my = std::max( qY, 0.0f );
	g.sd = std::sqrt( mx * mx + my * my ) + std::min( std::max( qX, qY ), 0.0f ) - radius;

	return g;
}

//= mirrored: dotMask() in Screen.cpp.
void dotMask( float mcx, float mcy, int pattern, float out[ 3 ] )
{
	out[ 0 ] = out[ 1 ] = out[ 2 ] = 1.0f;

	if( pattern == 1 )
	{
		const float rowHeight = 0.866f;
		const float row = std::floor( mcy / rowHeight );
		const float x   = mcx + modf_( row, 2.0f ) * 0.5f;
		const float idx = modf_( std::floor( x ), 3.0f );
		const float cx  = fractf( x ) - 0.5f;
		const float cy  = ( fractf( mcy / rowHeight ) - 0.5f ) * rowHeight;
		const float spot = 1.0f - smoothstepf( 0.24f, 0.46f, std::sqrt( cx * cx + cy * cy ) );
		out[ 0 ] = idx < 0.5f ? spot : 0.0f;
		out[ 1 ] = ( idx >= 0.5f && idx < 1.5f ) ? spot : 0.0f;
		out[ 2 ] = idx >= 1.5f ? spot : 0.0f;
		return;
	}

	if( pattern == 2 )
	{
		const float idx = modf_( std::floor( mcx ), 3.0f );
		const float stripe = 1.0f - smoothstepf( 0.26f, 0.50f, std::fabs( fractf( mcx ) - 0.5f ) );
		out[ 0 ] = idx < 0.5f ? stripe : 0.0f;
		out[ 1 ] = ( idx >= 0.5f && idx < 1.5f ) ? stripe : 0.0f;
		out[ 2 ] = idx >= 1.5f ? stripe : 0.0f;
		return;
	}

	if( pattern == 3 )
	{
		const float slotHeight = 2.0f;
		const float idx = modf_( std::floor( mcx ), 3.0f );
		const float stagger = modf_( std::floor( mcx / 3.0f ), 2.0f ) * 0.5f;
		const float sy = fractf( mcy / slotHeight + stagger );
		const float stripe = 1.0f - smoothstepf( 0.28f, 0.50f, std::fabs( fractf( mcx ) - 0.5f ) );
		const float slot   = 1.0f - smoothstepf( 0.38f, 0.50f, std::fabs( sy - 0.5f ) );
		const float value  = stripe * slot;
		out[ 0 ] = idx < 0.5f ? value : 0.0f;
		out[ 1 ] = ( idx >= 0.5f && idx < 1.5f ) ? value : 0.0f;
		out[ 2 ] = idx >= 1.5f ? value : 0.0f;
		return;
	}

	if( pattern == 4 )
	{
		const float idx = modf_( std::floor( mcx ), 3.0f );
		out[ 0 ] = idx < 0.5f ? 1.0f : 0.0f;
		out[ 1 ] = ( idx >= 0.5f && idx < 1.5f ) ? 1.0f : 0.0f;
		out[ 2 ] = idx >= 1.5f ? 1.0f : 0.0f;
	}
}

//---------------------------------------------------------------------------
/// `OFX::ChoiceParam` has only the out-parameter accessors.
///
/// `IntParam` and `DoubleParam` each carry a convenience overload that returns
/// the value, and `ChoiceParam` -- which is an int param in all but name --
/// does not. Reaching for the same spelling gives "cannot initialize a
/// variable of type const int with an rvalue of type void", which is a
/// perfectly clear message about a thoroughly unclear asymmetry.
int choice( OFX::ChoiceParam* p, double t )
{
	int v = 0;
	p->getValueAtTime( t, v );
	return v;
}

int choice( OFX::ChoiceParam* p )
{
	int v = 0;
	p->getValue( v );
	return v;
}

//---------------------------------------------------------------------------
class RegaussProcessorBase : public OFX::ImageProcessor
{
public:
	explicit RegaussProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( const FrameSetup* v, bool premultipliedValue )
	{
		setup         = v;
		premultiplied = premultipliedValue;
	}

protected:
	const FrameSetup* setup = nullptr;
	bool premultiplied      = false;
};

template< class PIX, int nComponents, int maxValue >
class RegaussProcessor : public RegaussProcessorBase
{
public:
	explicit RegaussProcessor( OFX::ImageEffect& effect ) :
		RegaussProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI bounds = _dstImg->getBounds();
		const FrameSetup& s   = *setup;

		const float cosRX = std::cos( s.perspectiveY ), sinRX = std::sin( s.perspectiveY );
		const float cosRY = std::cos( s.perspectiveX ), sinRY = std::sin( s.perspectiveX );

		const float du = 1.0f / s.outW;
		const float dv = 1.0f / s.outH;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix   = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			const float v = ( y - bounds.y1 + 0.5f ) / s.outH;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float u = ( x - bounds.x1 + 0.5f ) / s.outW;

				float rgba[ 4 ];
				if( !s.tubeEnabled )
				{
					//= mirrored: the Interference Only branch. Hand back what
					//the beam pass produced.
					bilinear4( s.beam, u, v, rgba );
				}
				else
				{
					screen( s, u, v, du, dv, cosRX, sinRX, cosRY, sinRY, rgba );
				}

				write( dstPix, rgba );
			}
		}
	}

private:
	static void screen( const FrameSetup& s, float u, float v, float du, float dv,
	                    float cosRX, float sinRX, float cosRY, float sinRY, float out[ 4 ] )
	{
		const Geometry g  = geometryAt( u, v, s, cosRX, sinRX, cosRY, sinRY );
		const Geometry gx = geometryAt( u + du, v, s, cosRX, sinRX, cosRY, sinRY );
		const Geometry gy = geometryAt( u, v + dv, s, cosRX, sinRX, cosRY, sinRY );

		//fwidth(): |ddx| + |ddy|, exactly as GLSL defines it.
		const float fwLineF = std::fabs( gx.lineF - g.lineF ) + std::fabs( gy.lineF - g.lineF );
		const float fwMaskX = std::fabs( gx.maskX - g.maskX ) + std::fabs( gy.maskX - g.maskX );
		const float fwMaskY = std::fabs( gx.maskY - g.maskY ) + std::fabs( gy.maskY - g.maskY );
		const float fwSd    = std::fabs( gx.sd - g.sd ) + std::fabs( gy.sd - g.sd );

		//--- scan ----------------------------------------------------------
		const float lines = std::max( s.lineCount, 1.0f );
		const float base  = std::floor( g.lineF );
		const float scanAA = smoothstepf( 1.2f, 2.0f, 1.0f / std::max( fwLineF, 1e-5f ) );

		float beamSum[ 3 ] = { 0.0f, 0.0f, 0.0f };
		float alphaSum = 0.0f, weightSum = 0.0f, weightFlat = 0.0f;

		for( int i = -1; i <= 1; ++i )
		{
			const float li = base + static_cast< float >( i );
			float c[ 4 ];
			bilinear4( s.beam, g.screenU, ( li + 0.5f ) / lines, c );

			const float luma = clampf( 0.299f * std::fabs( c[ 0 ] ) + 0.587f * std::fabs( c[ 1 ] )
			                               + 0.114f * std::fabs( c[ 2 ] ),
			                           0.0f, 1.0f );
			const float sigma = mixf( 0.26f, mixf( 0.34f, 0.95f, s.beamBloom ), luma );

			const float d = g.lineF - li;
			const float w = std::exp( -0.5f * d * d / ( sigma * sigma ) );
			const float wFlat = std::exp( -0.5f * static_cast< float >( i * i ) / ( sigma * sigma ) );

			for( int k = 0; k < 3; ++k )
				beamSum[ k ] += c[ k ] * w;
			alphaSum   += c[ 3 ] * w;
			weightSum  += w;
			weightFlat += wFlat;
		}

		float colour[ 3 ];
		for( int k = 0; k < 3; ++k )
			colour[ k ] = beamSum[ k ] / std::max( weightSum, 1e-4f );
		float alpha = alphaSum / std::max( weightSum, 1e-4f );

		const float scanMod = weightSum / std::max( weightFlat, 1e-4f );
		const float scan    = mixf( 1.0f, std::min( scanMod, 1.0f ), s.scanlines * scanAA );
		for( int k = 0; k < 3; ++k )
			colour[ k ] *= scan;

		//No halation here -- see the note at the top of this file.
		for( int k = 0; k < 3; ++k )
		{
			colour[ k ] = ( colour[ k ] - 0.5f * alpha ) * s.contrast + 0.5f * alpha;
			colour[ k ] *= s.brightness;
		}

		//--- mask ----------------------------------------------------------
		const float maskAA = 1.0f - smoothstepf( 0.4f, 0.8f, std::max( fwMaskX, fwMaskY ) );
		float shape[ 3 ];
		dotMask( g.maskX, g.maskY, s.maskPattern, shape );

		const float strength = s.maskStrength * maskAA;
		for( int k = 0; k < 3; ++k )
		{
			const float t = mixf( s.maskSpill, 1.0f, shape[ k ] );
			colour[ k ] *= mixf( 1.0f, t * s.maskGain, strength );
		}

		if( s.maskPattern == 2 )
		{
			float wire = 0.0f;
			wire += 1.0f - smoothstepf( 0.0f, 3.2f / s.outH, std::fabs( g.tubeY - 0.36f ) );
			wire += 1.0f - smoothstepf( 0.0f, 3.2f / s.outH, std::fabs( g.tubeY + 0.36f ) );
			const float cut = 1.0f - clampf( wire, 0.0f, 1.0f ) * 0.28f * strength;
			for( int k = 0; k < 3; ++k )
				colour[ k ] *= cut;
		}

		//--- the edge of the glass ------------------------------------------
		const float vigR = std::sqrt( g.tubeX * 0.92f * g.tubeX * 0.92f + g.tubeY * g.tubeY );
		const float vig  = 1.0f - s.vignette * smoothstepf( 0.25f, 1.5f, vigR );
		for( int k = 0; k < 3; ++k )
			colour[ k ] *= vig;

		const float aa = std::max( fwSd, 1e-4f );
		float face = 1.0f - smoothstepf( -aa, aa, g.sd );

		const float outsideX = ( g.screenU < 0.0f ? 1.0f : 0.0f ) + ( g.screenU >= 1.0f ? 1.0f : 0.0f );
		const float outsideY = ( g.screenV < 0.0f ? 1.0f : 0.0f ) + ( g.screenV >= 1.0f ? 1.0f : 0.0f );
		face *= ( 1.0f - clampf( outsideX + outsideY, 0.0f, 1.0f ) ) * g.infront;

		for( int k = 0; k < 3; ++k )
			out[ k ] = clampf( colour[ k ], 0.0f, 1.0f ) * face;
		out[ 3 ] = clampf( alpha, 0.0f, 1.0f ) * face;
	}

	void write( PIX* dstPix, const float rgba[ 4 ] ) const
	{
		//Everything in the two passes is premultiplied. A host that wants
		//straight alpha gets the divide here rather than a second code path
		//through the model.
		float outv[ 4 ] = { rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ] };
		if( !premultiplied && rgba[ 3 ] > 1e-6f )
		{
			for( int k = 0; k < 3; ++k )
				outv[ k ] = clampf( rgba[ k ] / rgba[ 3 ], 0.0f, 1.0f );
		}

		for( int k = 0; k < nComponents; ++k )
		{
			const float value = k < 4 ? outv[ k ] : 1.0f;
			dstPix[ k ] = static_cast< PIX >( clampf( value, 0.0f, 1.0f ) * maxValue
			                                  + ( maxValue == 1 ? 0.0f : 0.5f ) );
		}
	}
};

//---------------------------------------------------------------------------
class RegaussPlugin : public OFX::ImageEffect
{
public:
	explicit RegaussPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset       = fetchChoiceParam( kParamPreset );
		mode         = fetchChoiceParam( kParamMode );
		layout       = fetchChoiceParam( kParamLayout );
		autoMode     = fetchChoiceParam( kParamAuto );
		maskPattern  = fetchChoiceParam( kParamMaskPattern );

		tempo     = fetchDoubleParam( kParamTempo );
		degaussAt = fetchDoubleParam( kParamDegaussAt );

		for( const auto& entry : sliderNames() )
			sliders[ entry.first ] = fetchDoubleParam( entry.second );
	}

	void render( const OFX::RenderArguments& args ) override;
	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& name ) override;

private:
	/// Every 0..1 slider, keyed by the presets::Param it corresponds to. The
	/// preset table is host-agnostic; this is the OFX binding of it, and it is
	/// the direct counterpart of Regauss.h's kPresetParamIDs.
	static const std::vector< std::pair< int, const char* > >& sliderNames()
	{
		using namespace regauss::presets;
		static const std::vector< std::pair< int, const char* > > list = {
			{ kMagnetisation, kParamMagnetisation }, { kWander, kParamWander },
			{ kInterference, kParamInterference }, { kFrequency, kParamFrequency },
			{ kDeflection, kParamDeflection }, { kPurity, kParamPurity },
			{ kConvergence, kParamConvergence }, { kOverscan, kParamOverscan },
			{ kInterval, kParamInterval }, { kDuration, kParamDuration },
			{ kIntensity, kParamIntensity }, { kCoilSag, kParamCoilSag },
			{ kRecovery, kParamRecovery },
			{ kMaskPitch, kParamMaskPitch }, { kMaskStrength, kParamMaskStrength },
			{ kScanlines, kParamScanlines }, { kLineCount, kParamLineCount },
			{ kBeamBloom, kParamBeamBloom },
			{ kBrightness, kParamBrightness }, { kContrast, kParamContrast },
			{ kCurvature, kParamCurvature }, { kCornerRadius, kParamCornerRadius },
			{ kVignette, kParamVignette },
		};
		return list;
	}

	float slider( int presetParam, double time ) const
	{
		const auto it = sliders.find( presetParam );
		return it == sliders.end() ? 0.0f : static_cast< float >( it->second->getValueAtTime( time ) );
	}

	void applyPreset( int index, double time );

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset = nullptr;
	OFX::ChoiceParam* mode = nullptr;
	OFX::ChoiceParam* layout = nullptr;
	OFX::ChoiceParam* autoMode = nullptr;
	OFX::ChoiceParam* maskPattern = nullptr;

	OFX::DoubleParam* tempo = nullptr;
	OFX::DoubleParam* degaussAt = nullptr;

	std::map< int, OFX::DoubleParam* > sliders;

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

//---------------------------------------------------------------------------
void RegaussPlugin::render( const OFX::RenderArguments& args )
{
	std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
	std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );
	if( !dst || !src )
		return;

	const OfxRectI dstBounds = dst->getBounds();
	const int outW = dstBounds.x2 - dstBounds.x1;
	const int outH = dstBounds.y2 - dstBounds.y1;
	if( outW <= 0 || outH <= 0 )
		return;

	const double t = args.time;

	//------------------------------------------------------------------
	// Seconds, from the timeline.
	//
	// FFGL is handed a clock; here the frame number and the frame rate are
	// the clock, which is better -- a frame renders identically however the
	// host reached it, so a scrub and a playthrough agree.
	//------------------------------------------------------------------
	const double fps     = std::max( 1e-3, static_cast< double >( dstClip->getFrameRate() ) );
	const double seconds = t / fps;

	FrameSetup setup;
	setup.outW   = static_cast< float >( outW );
	setup.outH   = static_cast< float >( outH );
	setup.aspect = setup.outW / std::max( setup.outH, 1.0f );
	setup.time   = static_cast< float >( seconds );
	setup.scanPeriod = static_cast< float >( 1.0 / fps );

	setup.tubeEnabled = choice( mode, t ) == 0;

	const int patternIndex = choice( maskPattern, t );
	const regauss::MaskSpec& spec = regauss::mask( patternIndex );

	regauss::controls::Settings s;
	s.layout        = choice( layout, t );
	s.magnetisation = slider( regauss::presets::kMagnetisation, t );
	s.seed          = static_cast< float >( fetchDoubleParam( kParamSeed )->getValueAtTime( t ) );
	s.wander        = slider( regauss::presets::kWander, t );
	s.interference  = slider( regauss::presets::kInterference, t );
	s.frequency     = slider( regauss::presets::kFrequency, t );
	s.deflection    = slider( regauss::presets::kDeflection, t );
	s.purity        = slider( regauss::presets::kPurity, t );
	s.convergence   = slider( regauss::presets::kConvergence, t );
	s.overscan      = slider( regauss::presets::kOverscan, t );
	s.duration      = slider( regauss::presets::kDuration, t );
	s.intensity     = slider( regauss::presets::kIntensity, t );
	s.coilSag       = slider( regauss::presets::kCoilSag, t );
	s.recovery      = slider( regauss::presets::kRecovery, t );
	s.maskPitch     = slider( regauss::presets::kMaskPitch, t );
	s.maskPattern   = patternIndex;

	//------------------------------------------------------------------
	// When the coil last fired.
	//
	// Scheduled, never pressed -- see the note at the top of this file. The
	// grid is literal seconds in Interval mode and comes off the Tempo control
	// in the other two, because OFX gives no transport of any kind.
	//------------------------------------------------------------------
	const double start = degaussAt->getValueAtTime( t );
	float lastTrigger  = -1.0f;

	const int autoIndex = choice( autoMode, t );
	if( autoIndex != regauss::kAutoOff && seconds >= start )
	{
		const double bpm        = std::max( 1.0, tempo->getValueAtTime( t ) );
		const double barSeconds = 240.0 / bpm;

		double grid = 0.0;
		switch( autoIndex )
		{
			case regauss::kAutoInterval:
				grid = regauss::controls::Interval( slider( regauss::presets::kInterval, t ) );
				break;
			case regauss::kAutoBeat: grid = barSeconds / 4.0; break;
			case regauss::kAutoBar:  grid = barSeconds; break;
			default: break;
		}

		const float scheduled = regauss::scheduledTrigger( static_cast< float >( seconds - start ),
		                                                   static_cast< float >( grid ) );
		if( scheduled >= 0.0f )
			lastTrigger = scheduled + static_cast< float >( start );
	}

	setup.drive = regauss::controls::drive( s, spec, setup.time, lastTrigger, setup.outW, setup.outH );

	setup.maskPattern  = patternIndex;
	setup.maskPitch    = regauss::controls::MaskPitchPixels( s.maskPitch );
	setup.maskStrength = slider( regauss::presets::kMaskStrength, t );
	setup.maskSpill    = spec.spill;
	setup.maskGain     = spec.gain;

	setup.scanlines  = slider( regauss::presets::kScanlines, t );
	setup.lineCount  = regauss::controls::LineCount( slider( regauss::presets::kLineCount, t ) );
	setup.beamBloom  = slider( regauss::presets::kBeamBloom, t );
	setup.brightness = slider( regauss::presets::kBrightness, t ) * 2.0f;
	setup.contrast   = slider( regauss::presets::kContrast, t ) * 2.0f;

	setup.curvature    = slider( regauss::presets::kCurvature, t ) * 0.6f;
	setup.cornerRadius = slider( regauss::presets::kCornerRadius, t ) * 0.35f;
	setup.perspectiveX = ( static_cast< float >( fetchDoubleParam( kParamPerspectiveX )->getValueAtTime( t ) ) - 0.5f ) * 1.8f;
	setup.perspectiveY = ( static_cast< float >( fetchDoubleParam( kParamPerspectiveY )->getValueAtTime( t ) ) - 0.5f ) * 1.8f;
	setup.zoom         = mixf( 0.5f, 1.5f, static_cast< float >( fetchDoubleParam( kParamZoom )->getValueAtTime( t ) ) );
	setup.vignette     = slider( regauss::presets::kVignette, t );

	//------------------------------------------------------------------
	// The source, as a premultiplied float plane.
	//------------------------------------------------------------------
	const bool premultiplied = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;
	const OfxRectI srcBounds = src->getBounds();
	const int srcW = srcBounds.x2 - srcBounds.x1;
	const int srcH = srcBounds.y2 - srcBounds.y1;
	setup.source.allocate( std::max( srcW, 1 ), std::max( srcH, 1 ) );

	{
		const OFX::BitDepthEnum depth = src->getPixelDepth();
		const int comps = src->getPixelComponentCount();

		for( int y = 0; y < srcH; ++y )
		{
			for( int x = 0; x < srcW; ++x )
			{
				float px[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
				const void* p = src->getPixelAddress( srcBounds.x1 + x, srcBounds.y1 + y );
				if( p != nullptr )
				{
					for( int k = 0; k < std::min( comps, 4 ); ++k )
					{
						switch( depth )
						{
							case OFX::eBitDepthUByte:
								px[ k ] = static_cast< const unsigned char* >( p )[ k ] / 255.0f;
								break;
							case OFX::eBitDepthUShort:
								px[ k ] = static_cast< const unsigned short* >( p )[ k ] / 65535.0f;
								break;
							default:
								px[ k ] = static_cast< const float* >( p )[ k ];
								break;
						}
					}
					if( comps < 4 )
						px[ 3 ] = 1.0f;
				}

				//The model is premultiplied throughout, so a straight-alpha
				//host is converted once on the way in rather than everywhere.
				if( !premultiplied )
				{
					for( int k = 0; k < 3; ++k )
						px[ k ] *= px[ 3 ];
				}

				std::memcpy( setup.source.at( x, y ), px, sizeof( px ) );
			}
		}
	}

	//------------------------------------------------------------------
	// The beam pass, into a plane of its own.
	//
	// It cannot be folded into the tiled processor below: the screen pass
	// samples the beam's output wherever the curvature and the scan send it,
	// which is routinely outside the tile being written.
	//------------------------------------------------------------------
	setup.beam.allocate( outW, outH );
	{
		const unsigned hardware = std::max( 1u, std::thread::hardware_concurrency() );
		const int threads = static_cast< int >( std::min< unsigned >( hardware, 16u ) );
		std::vector< std::thread > pool;
		pool.reserve( threads );

		for( int ti = 0; ti < threads; ++ti )
		{
			pool.emplace_back( [ & , ti ] {
				for( int y = ti; y < outH; y += threads )
				{
					const float v = ( y + 0.5f ) / setup.outH;
					for( int x = 0; x < outW; ++x )
					{
						const float u = ( x + 0.5f ) / setup.outW;
						beamAt( setup, u, v, setup.beam.at( x, y ) );
					}
				}
			} );
		}
		for( auto& th : pool )
			th.join();
	}

	//------------------------------------------------------------------
	// The screen pass, through the SDK's tiled multi-threading.
	//------------------------------------------------------------------
	const OFX::BitDepthEnum depth = dst->getPixelDepth();
	const OFX::PixelComponentEnum comps = dst->getPixelComponents();

	auto run = [ & ]( RegaussProcessorBase& processor ) {
		processor.setDstImg( dst.get() );
		processor.setRenderWindow( args.renderWindow );
		processor.setSetup( &setup, premultiplied );
		processor.process();
	};

	if( comps == OFX::ePixelComponentRGBA )
	{
		if( depth == OFX::eBitDepthUByte )       { RegaussProcessor< unsigned char, 4, 255 > p( *this ); run( p ); }
		else if( depth == OFX::eBitDepthUShort ) { RegaussProcessor< unsigned short, 4, 65535 > p( *this ); run( p ); }
		else                                     { RegaussProcessor< float, 4, 1 > p( *this ); run( p ); }
	}
	else
	{
		if( depth == OFX::eBitDepthUByte )       { RegaussProcessor< unsigned char, 3, 255 > p( *this ); run( p ); }
		else if( depth == OFX::eBitDepthUShort ) { RegaussProcessor< unsigned short, 3, 65535 > p( *this ); run( p ); }
		else                                     { RegaussProcessor< float, 3, 1 > p( *this ); run( p ); }
	}
}

//---------------------------------------------------------------------------
void RegaussPlugin::applyPreset( int index, double time )
{
	if( index <= 0 || index > regauss::presets::kCount )
		return;

	const regauss::presets::Preset& p = regauss::presets::kPresets[ index - 1 ];

	applyingPreset = true;
	beginEditBlock( "applyPreset" );

	mode->setValue( static_cast< int >( std::lround( p.v[ regauss::presets::kMode ] ) ) );
	layout->setValue( static_cast< int >( std::lround( p.v[ regauss::presets::kLayout ] ) ) );
	autoMode->setValue( static_cast< int >( std::lround( p.v[ regauss::presets::kAuto ] ) ) );
	maskPattern->setValue( static_cast< int >( std::lround( p.v[ regauss::presets::kMaskPattern ] ) ) );

	for( const auto& entry : sliderNames() )
		sliders[ entry.first ]->setValue( p.v[ entry.first ] );

	endEditBlock();
	applyingPreset = false;

	(void)time;
}

void RegaussPlugin::changedParam( const OFX::InstanceChangedArgs& args, const std::string& name )
{
	if( applyingPreset )
		return;

	if( name == kParamPreset )
	{
		applyPreset( choice( preset ), args.time );
		return;
	}

	//An edit to anything a preset covers means the operator has taken over.
	//Judged by NAME, not by change reason: hosts differ on what they report
	//for a value the plugin itself wrote, and this callback is already
	//guarded against our own writes by applyingPreset.
	if( choice( preset ) == 0 )
		return;

	static const std::vector< std::string > covered = [] {
		std::vector< std::string > out = { kParamMode, kParamLayout, kParamAuto, kParamMaskPattern };
		for( const auto& entry : sliderNames() )
			out.emplace_back( entry.second );
		return out;
	}();

	if( std::find( covered.begin(), covered.end(), name ) != covered.end() )
		preset->setValue( 0 );
}

//---------------------------------------------------------------------------
OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( RegaussPluginFactory, {}, {} );

void RegaussPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	//A tile cannot render alone: the beam pass warps, so a pixel inside the
	//tile routinely reads a source pixel outside it, and the screen pass then
	//samples the beam plane wherever the curvature sends it.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void RegaussPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named situations -- a room with a television in it and something "
	                      "magnetic nearby. Picking one sets the covered controls; editing any "
	                      "of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < regauss::presets::kCount; ++i )
		presetParam->appendOption( regauss::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::ChoiceParamDescriptor* modeParam = desc.defineChoiceParam( kParamMode );
	modeParam->setLabels( "Render", "Render", "Render" );
	modeParam->setHint( "Full CRT draws the television. Interference Only applies the magnet "
	                    "and nothing else, for stacking on footage that is already a CRT." );
	modeParam->appendOption( "Full CRT" );
	modeParam->appendOption( "Interference Only" );
	modeParam->setDefault( 0 );
	modeParam->setAnimates( false );
	page->addChild( *modeParam );

	//--- Field -------------------------------------------------------------
	OFX::GroupParamDescriptor* fieldGroup = desc.defineGroupParam( "Field" );
	fieldGroup->setLabels( "Field", "Field", "Field" );
	page->addChild( *fieldGroup );

	OFX::ChoiceParamDescriptor* layoutParam = desc.defineChoiceParam( kParamLayout );
	layoutParam->setLabels( "Layout", "Layout", "Layout" );
	layoutParam->setHint( "Where the magnets are. A tight pair gives a small hard stain with "
	                      "heavy colour fringing; a distant one gives a broad soft "
	                      "discolouration with none." );
	layoutParam->appendOption( "Speaker Left" );
	layoutParam->appendOption( "Corner Magnet" );
	layoutParam->appendOption( "Ring Magnet" );
	layoutParam->appendOption( "Wandering" );
	layoutParam->appendOption( "Earth's Field" );
	layoutParam->setDefault( 0 );
	layoutParam->setParent( *fieldGroup );
	page->addChild( *layoutParam );

	defineSlider( desc, page, kParamMagnetisation, "Magnetisation",
	              "How much field the shadow mask is holding. The degauss coil walks this down.",
	              0.70 )->setParent( *fieldGroup );
	defineSlider( desc, page, kParamSeed, "Seed",
	              "Reshuffles the poles within the layout's character.", 0.0 )->setParent( *fieldGroup );
	defineSlider( desc, page, kParamWander, "Wander",
	              "How far the magnets drift. At zero they are bolted down.", 0.0 )->setParent( *fieldGroup );
	defineSlider( desc, page, kParamInterference, "Interference",
	              "An alternating field leaking in from the room.", 0.0 )->setParent( *fieldGroup );
	defineSlider( desc, page, kParamFrequency, "Frequency",
	              "10 to 120 Hz. Near the frame rate the bend stands still; a hertz either "
	              "side and it crawls up or down the picture.", 0.364 )->setParent( *fieldGroup );

	//--- Beam --------------------------------------------------------------
	OFX::GroupParamDescriptor* beamGroup = desc.defineGroupParam( "Beam" );
	beamGroup->setLabels( "Beam", "Beam", "Beam" );
	page->addChild( *beamGroup );

	defineSlider( desc, page, kParamDeflection, "Deflection",
	              "How far the field bends the beam: the geometry error.", 0.25 )->setParent( *beamGroup );
	defineSlider( desc, page, kParamPurity, "Purity",
	              "How much of a colour error that bend causes. Separate from Deflection "
	              "because a real tube's two sensitivities are set by different hardware.",
	              0.70 )->setParent( *beamGroup );
	defineSlider( desc, page, kParamConvergence, "Convergence",
	              "How far apart the three cathodes sit. At zero all three beams take the "
	              "same path and there is no fringing.", 0.30 )->setParent( *beamGroup );
	defineSlider( desc, page, kParamOverscan, "Overscan",
	              "How much wider than its own face the set scans.", 0.25 )->setParent( *beamGroup );

	//--- Degauss -----------------------------------------------------------
	OFX::GroupParamDescriptor* coilGroup = desc.defineGroupParam( "Degauss" );
	coilGroup->setLabels( "Degauss", "Degauss", "Degauss" );
	page->addChild( *coilGroup );

	OFX::ChoiceParamDescriptor* autoParam = desc.defineChoiceParam( kParamAuto );
	autoParam->setLabels( "Auto", "Auto", "Auto" );
	autoParam->setHint( "When the coil fires. There is no button in an OFX host: a press is an "
	                    "event at wall-clock time and this plugin's frames render in any order, "
	                    "so the coil is scheduled instead." );
	autoParam->appendOption( "Off" );
	autoParam->appendOption( "Interval" );
	autoParam->appendOption( "Beat" );
	autoParam->appendOption( "Bar" );
	autoParam->setDefault( 0 );
	autoParam->setParent( *coilGroup );
	page->addChild( *autoParam );

	OFX::DoubleParamDescriptor* tempoParam = desc.defineDoubleParam( kParamTempo );
	tempoParam->setLabels( "Tempo", "Tempo", "Tempo" );
	tempoParam->setHint( "Beats per minute, for the Beat and Bar modes. The FFGL build takes "
	                     "this from the host's transport; OFX has none." );
	tempoParam->setRange( 20.0, 300.0 );
	tempoParam->setDisplayRange( 60.0, 200.0 );
	tempoParam->setDefault( 120.0 );
	tempoParam->setParent( *coilGroup );
	page->addChild( *tempoParam );

	OFX::DoubleParamDescriptor* atParam = desc.defineDoubleParam( kParamDegaussAt );
	atParam->setLabels( "Degauss At", "Degauss At", "Degauss At" );
	atParam->setHint( "Seconds into the timeline at which the schedule starts, so a firing can "
	                  "be placed on a cut." );
	atParam->setRange( 0.0, 3600.0 );
	atParam->setDisplayRange( 0.0, 60.0 );
	atParam->setDefault( 0.0 );
	atParam->setParent( *coilGroup );
	page->addChild( *atParam );

	defineSlider( desc, page, kParamInterval, "Interval",
	              "Seconds between firings, 0.25 to 30, geometric.", 0.58 )->setParent( *coilGroup );
	defineSlider( desc, page, kParamDuration, "Duration",
	              "How long the coil takes to die away, 0.15 to 6 seconds.", 0.62 )->setParent( *coilGroup );
	defineSlider( desc, page, kParamIntensity, "Intensity",
	              "Peak field the coil applies.", 0.70 )->setParent( *coilGroup );
	defineSlider( desc, page, kParamCoilSag, "Coil Sag",
	              "How far the coil pulls the HT down. The picture dims and swells while it runs.",
	              0.60 )->setParent( *coilGroup );
	defineSlider( desc, page, kParamRecovery, "Recovery",
	              "How long the mask takes to magnetise again, 0.2 to 60 seconds.",
	              0.65 )->setParent( *coilGroup );

	//--- Tube --------------------------------------------------------------
	OFX::GroupParamDescriptor* tubeGroup = desc.defineGroupParam( "Tube" );
	tubeGroup->setLabels( "Tube", "Tube", "Tube" );
	page->addChild( *tubeGroup );

	OFX::ChoiceParamDescriptor* maskParam = desc.defineChoiceParam( kParamMaskPattern );
	maskParam->setLabels( "Mask Pattern", "Mask Pattern", "Mask Pattern" );
	maskParam->setHint( "The phosphor layout. It decides how a landing error becomes a colour "
	                    "error: an aperture grille has no vertical structure, so a vertical "
	                    "error costs it nothing." );
	for( int i = 0; i < regauss::maskCount(); ++i )
		maskParam->appendOption( regauss::mask( i ).name );
	maskParam->setDefault( 1 );
	maskParam->setParent( *tubeGroup );
	page->addChild( *maskParam );

	defineSlider( desc, page, kParamMaskPitch, "Mask Pitch", "Output pixels per phosphor stripe.", 0.30 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamMaskStrength, "Mask Strength", "", 0.60 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamScanlines, "Scanlines", "", 0.50 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamLineCount, "Line Count", "120 to 960 lines. 480 is 0.43, 576 is 0.54.", 0.429 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamBeamBloom, "Beam Bloom", "A brighter line is a fatter line.", 0.50 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamBrightness, "Brightness", "0.5 is unity.", 0.50 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamContrast, "Contrast", "0.5 is unity.", 0.50 )->setParent( *tubeGroup );

	//--- Geometry ----------------------------------------------------------
	OFX::GroupParamDescriptor* geometryGroup = desc.defineGroupParam( "Geometry" );
	geometryGroup->setLabels( "Geometry", "Geometry", "Geometry" );
	page->addChild( *geometryGroup );

	defineSlider( desc, page, kParamCurvature, "Curvature", "", 0.28 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamCornerRadius, "Corner Radius", "", 0.16 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamPerspectiveX, "Perspective X", "0.5 is straight on.", 0.50 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamPerspectiveY, "Perspective Y", "0.5 is straight on.", 0.50 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamZoom, "Zoom", "0.5 is 1:1.", 0.50 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamVignette, "Vignette", "", 0.35 )->setParent( *geometryGroup );
}

OFX::ImageEffect* RegaussPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new RegaussPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static RegaussPluginFactory* factory =
		new RegaussPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
