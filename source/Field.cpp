#include "Field.h"

#include <cmath>

namespace regauss
{
namespace
{
/// An integer hash, not `fract( sin( x ) * 43758.5453 )`.
///
/// The fleet's rule, and it earns itself here: the seed has to give the same
/// pole positions in the plugin, in the OpenFX build and in the browser demo,
/// and the trigonometric hash is the one construct whose answer genuinely
/// differs between an ARM CPU, an Intel CPU and three GPU vendors.
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float hash01( uint32_t v )
{
	return static_cast< float >( pcg( v ) ) * ( 1.0f / 4294967296.0f );
}

/// Signed, centred on zero.
float hash11( uint32_t v )
{
	return hash01( v ) * 2.0f - 1.0f;
}
} // namespace

//---------------------------------------------------------------------------
PoleSet poles( int layout, float seed, float wander, float time, float aspect )
{
	//------------------------------------------------------------------
	// The layouts.
	//
	// Each is a situation somebody has actually had, and the *spacing* of the
	// poles within it is the part that carries the character:
	//
	//   Two poles close together give a field that dies away quickly, so the
	//   stain is small, tight and strongly curved -- and because it dies away
	//   quickly it has a steep gradient, which is convergence error, which is
	//   colour fringing. That is a fridge magnet on the glass.
	//
	//   Two poles far apart give a broad, slowly varying field: a large soft
	//   discolouration with almost no fringing. That is the Earth's field
	//   after somebody turned the set to face a different wall.
	//
	// So the layouts are not eight numbers each; they are near-pairs and
	// far-pairs, and the plugin's whole vocabulary of stains follows from
	// which it is.
	//------------------------------------------------------------------
	PoleSet set {};

	switch( layout )
	{
		default:
		case kSpeakerLeft:
			//A bookshelf speaker beside the set: a big ring magnet, so a strong
			//near-pair just off the left edge, and its unshielded partner on the
			//other side of the room contributing almost nothing.
			set.p[ 0 ] = { -1.20f, 0.20f, 1.00f };
			set.p[ 1 ] = { -1.34f, -0.12f, -0.85f };
			set.p[ 2 ] = { 1.48f, 0.06f, 0.16f };
			set.p[ 3 ] = { 1.60f, -0.18f, -0.13f };
			break;

		case kCornerMagnet:
			//Something small held against the top-right corner. Tight pair,
			//fast falloff, vicious gradient: this is the layout that fringes.
			set.p[ 0 ] = { 0.82f, 0.78f, 0.95f };
			set.p[ 1 ] = { 1.02f, 0.94f, -0.88f };
			set.p[ 2 ] = { -0.90f, -0.86f, 0.10f };
			set.p[ 3 ] = { -1.02f, -0.96f, -0.08f };
			break;

		case kRingMagnet:
			//A purity ring that has been knocked round on the neck, so the
			//correction it is supposed to apply is now applied in the wrong
			//direction: four poles on the rim, alternating.
			set.p[ 0 ] = { 0.00f, 1.14f, 0.80f };
			set.p[ 1 ] = { 1.14f, 0.00f, -0.80f };
			set.p[ 2 ] = { 0.00f, -1.14f, 0.80f };
			set.p[ 3 ] = { -1.14f, 0.00f, -0.80f };
			break;

		case kWandering:
			//Nothing static at all: four moderate poles that orbit. Wants
			//Wander turned up; at zero it is simply a lopsided Ring Magnet,
			//which is the honest answer rather than a special case.
			for( int i = 0; i < kPoleCount; ++i )
			{
				const float a = 1.5707963f * static_cast< float >( i ) + 0.4f;
				set.p[ i ]    = { 1.05f * std::cos( a ), 1.05f * std::sin( a ), ( i % 2 == 0 ) ? 0.70f : -0.70f };
			}
			break;

		case kEarthField:
			//Far away and correspondingly strong, which is the only way to get
			//a field that is near enough uniform across the face. Uniform means
			//the whole picture leans and *nothing* fringes -- geometry error
			//with no convergence error, which is exactly what the Earth's field
			//does to a colour set and why it needs no degaussing to look right,
			//only to look straight.
			set.p[ 0 ] = { -4.20f, 2.60f, 7.00f };
			set.p[ 1 ] = { 4.20f, -2.60f, -7.00f };
			set.p[ 2 ] = { -4.60f, -2.90f, 1.20f };
			set.p[ 3 ] = { 4.60f, 2.90f, -1.20f };
			break;
	}

	//------------------------------------------------------------------
	// Into square units.
	//
	// The positions above are in half-widths, so that "just off the left-hand
	// edge" stays just off the left-hand edge on any composition. The field is
	// evaluated in square units -- where the half-HEIGHT is 1 -- so a round
	// magnet makes a round stain rather than an elliptical one, and getting
	// from one to the other is this multiply.
	//
	// Only the layout's own x. The jitter and the drift below are physical
	// movements of a real object, and a real object does not travel further
	// sideways on a wider television.
	//------------------------------------------------------------------
	const float safeAspect = aspect > 0.01f ? aspect : 1.0f;
	for( int i = 0; i < kPoleCount; ++i )
		set.p[ i ].x *= safeAspect;

	//------------------------------------------------------------------
	// Seed and drift.
	//
	// The seed nudges each pole rather than replacing it, so a layout still
	// looks like itself at every seed -- the operator picked "speaker on the
	// left" and is entitled to keep it.
	//------------------------------------------------------------------
	const uint32_t base = static_cast< uint32_t >( seed * 4096.0f ) * 2654435761u
	                      + static_cast< uint32_t >( layout ) * 40503u;

	//Wandering orbits an order of magnitude wider than the others: there the
	//drift is the whole point, elsewhere it is a magnet somebody keeps
	//brushing past.
	const float reach = ( layout == kWandering ? 0.85f : 0.22f ) * wander;

	for( int i = 0; i < kPoleCount; ++i )
	{
		const uint32_t h = base + static_cast< uint32_t >( i ) * 0x9E3779B9u;

		set.p[ i ].x += hash11( h ) * 0.18f;
		set.p[ i ].y += hash11( h + 1u ) * 0.18f;
		set.p[ i ].strength *= 0.80f + hash01( h + 2u ) * 0.40f;

		//Each pole gets its own rate and phase, so they never line up into a
		//single visible rotation. Rates are deliberately not harmonically
		//related: two poles at 0.10 and 0.20 Hz repeat every ten seconds and
		//the eye finds the loop immediately.
		const float rate  = 0.043f + hash01( h + 3u ) * 0.081f;
		const float phase = hash01( h + 4u ) * 6.2831853f;

		set.p[ i ].x += reach * std::cos( time * rate * 6.2831853f + phase );
		set.p[ i ].y += reach * std::sin( time * rate * 4.6831853f + phase * 1.7f );
	}

	return set;
}

//---------------------------------------------------------------------------
void fieldAt( const PoleSet& set, float px, float py, float& bx, float& by )
{
	bx = 0.0f;
	by = 0.0f;

	for( int i = 0; i < kPoleCount; ++i )
	{
		const float dx = px - set.p[ i ].x;
		const float dy = py - set.p[ i ].y;

		//The pole sits kPoleHeight off the plane of the glass, so this is the
		//real 3-D distance to it and there is no singularity anywhere on the
		//face to guard against.
		//
		//No sqrt: the exponent is one, not three halves, because this stands
		//for the field integrated along the beam's flight rather than the
		//field at the point it lands. See Field.h.
		const float r2  = dx * dx + dy * dy + kPoleHeight * kPoleHeight;
		const float inv = set.p[ i ].strength / r2;

		bx += dx * inv;
		by += dy * inv;
	}
}

//---------------------------------------------------------------------------
Coil coil( float secondsSinceTrigger, float duration, float intensity )
{
	Coil out { 0.0f, 1.0f, 0.0f };

	if( secondsSinceTrigger < 0.0f )
		return out;//the coil has not fired; the mask keeps whatever it holds

	//4.6 is ln(100): the envelope is down to one per cent of its peak after
	//exactly `duration` seconds, so the control means what its label says
	//rather than being a time constant the operator has to multiply by four in
	//their head.
	const float k        = 4.6f / ( duration > 0.05f ? duration : 0.05f );
	const float envelope = std::exp( -secondsSinceTrigger * k );

	out.ac = envelope * intensity;

	//The same envelope walks the retained magnetisation down. This is not a
	//convenience -- it is the physics. Demagnetising works because the applied
	//field passes through every amplitude between "enough to saturate the
	//steel" and zero, flipping the domains once more at each and leaving them
	//with progressively less to align to. A coil that switched off abruptly
	//would leave the mask magnetised at whatever the last half-cycle said, and
	//degaussing a television by pulling the plug out is exactly as effective
	//as that sounds.
	out.retained = envelope;

	//The HT recovers faster than the field dies, because the thermistor's
	//resistance is climbing the whole time: the current is falling faster than
	//the field it produces suggests. Hence a shorter constant rather than a
	//scaled copy of the same curve.
	out.sag = std::exp( -secondsSinceTrigger * k * 1.9f ) * intensity;

	return out;
}

//---------------------------------------------------------------------------
float acFrequency( float param )
{
	//10 to 120 Hz. 50 lands at 0.364 and 60 at 0.455, which are not round
	//numbers on the slider, and that is the right trade: the interesting
	//territory is the beat between this and the host's frame rate, and a
	//two-element 50/60 dropdown cannot reach it. The presets carry the two
	//mains frequencies for anyone who wants them exactly.
	const float t = param < 0.0f ? 0.0f : ( param > 1.0f ? 1.0f : param );
	return 10.0f + t * 110.0f;
}

//---------------------------------------------------------------------------
float scheduledTrigger( float now, float gridSeconds )
{
	if( gridSeconds <= 0.0f || now < 0.0f )
		return -1.0f;

	return std::floor( now / gridSeconds ) * gridSeconds;
}

} // namespace regauss
