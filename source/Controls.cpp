#include "Controls.h"

#include "Masks.h"

#include <algorithm>
#include <cmath>

namespace regauss::controls
{
namespace
{
float lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

float clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// Geometric rather than linear, for anything measured in time.
///
/// A linear 0.2..60 second slider spends its first three per cent on
/// everything under two seconds, which is the whole of the useful range for a
/// degauss and most of it for a recovery. Geometric gives equal resolution per
/// octave, which is how the ear and the eye both judge duration anyway.
float geometric( float p, float lo, float hi )
{
	return lo * std::pow( hi / lo, clamp01( p ) );
}
} // namespace

float Deflection( float p )
{
	return clamp01( p ) * kDeflectScale;
}

float MaskPitchPixels( float p )
{
	return lerp( 2.0f, 14.0f, clamp01( p ) );
}

float LineCount( float p )
{
	//120 lines is a very early set, 960 is a high-end late-nineties monitor,
	//and 480/576 -- the two everybody actually means -- land at 0.43 and 0.54.
	return lerp( 120.0f, 960.0f, clamp01( p ) );
}

float GunSeparation( float p )
{
	return clamp01( p ) * 0.30f;
}

float Interval( float p )
{
	return geometric( p, 0.25f, 30.0f );
}

float Duration( float p )
{
	return geometric( p, 0.15f, 6.0f );
}

float Intensity( float p )
{
	//Up to three times the ambient interference scale. A degauss coil is a far
	//bigger field than anything that leaks in from the room, and if the
	//button's effect is not obviously larger than the Interference slider's
	//then the button is not doing its job.
	return clamp01( p ) * 3.0f;
}

float Recovery( float p )
{
	return geometric( p, 0.2f, 60.0f );
}

float Overscan( float p )
{
	return lerp( 1.0f, 1.15f, clamp01( p ) );
}

//---------------------------------------------------------------------------
Drive drive( const Settings& s,
             const MaskSpec& mask,
             float now,
             float lastTrigger,
             float outputWidth,
             float outputHeight )
{
	Drive d;

	const float sinceTrigger = lastTrigger >= 0.0f ? ( now - lastTrigger ) : -1.0f;
	const Coil c = coil( sinceTrigger, Duration( s.duration ), Intensity( s.intensity ) );

	//------------------------------------------------------------------
	// Where the magnets are. The poles drift against `now` rather than
	// against a frame counter, so the same moment of a timeline gives the
	// same stain however the host got there.
	//------------------------------------------------------------------
	const float aspect = outputWidth / std::max( outputHeight, 1.0f );
	d.poles = poles( s.layout, s.seed, clamp01( s.wander ), now, aspect );

	//------------------------------------------------------------------
	// How magnetised the mask is.
	//
	// Two things are competing: the coil is walking the magnetisation down,
	// and time is letting it creep back on. `max` rather than a sum, so the
	// handover is monotonic and the total can never exceed what the operator
	// asked for -- during the transient the coil's falling envelope is the
	// larger, and once it has died the recovery curve takes over.
	//------------------------------------------------------------------
	const float recovered = sinceTrigger < 0.0f
	                            ? 1.0f
	                            : 1.0f - std::exp( -sinceTrigger / Recovery( s.recovery ) );

	d.staticAmp = clamp01( s.magnetisation ) * std::max( c.retained, recovered );

	//The coil's own field is alternating, and so is whatever leaks in from the
	//room, so they add here and share one oscillator in the shader.
	d.acAmp     = clamp01( s.interference ) + c.ac;
	d.frequency = acFrequency( s.frequency );

	//------------------------------------------------------------------
	// The two sensitivities.
	//------------------------------------------------------------------
	d.deflection = Deflection( s.deflection );

	const float pitch = MaskPitchPixels( s.maskPitch );
	d.purityGainX = clamp01( s.purity ) * kPurityScale * ( kReferencePitch / pitch );

	//A vertical landing error only costs colour on a mask whose triads are
	//staggered row to row. An aperture grille's stripes run the full height of
	//the tube, so moving the beam up or down slides it along its own stripe
	//and changes nothing -- which is a genuine advantage of the design and
	//falls out here rather than being asserted.
	//
	//1.732 is 1.5 / 0.866: one row step on a hexagonal lattice is 0.866 of a
	//phosphor width vertically and staggers the triad by half of one, so a
	//vertical error is worth that much more than a horizontal one.
	d.purityGainY = mask.staggers
	                    ? d.purityGainX * ( outputHeight / std::max( outputWidth, 1.0f ) ) * 1.7320508f
	                    : 0.0f;

	//A mask with no phosphor structure has nothing for the beam to fall
	//between, so there is no purity error to have. That is a monochrome tube,
	//and it is the honest answer rather than a special case: index 0 is "None".
	if( s.maskPattern <= 0 )
	{
		d.purityGainX = 0.0f;
		d.purityGainY = 0.0f;
	}

	d.gunSeparation = GunSeparation( s.convergence );
	d.gunTriangular = mask.triangularGun ? 1.0f : 0.0f;

	//------------------------------------------------------------------
	// What the coil costs the rest of the set.
	//------------------------------------------------------------------
	const float sag = clamp01( c.sag ) * clamp01( s.coilSag );
	d.sag   = sag;
	//Less anode voltage means slower electrons, and slower electrons are bent
	//further by the same yoke current. The picture swells while it degausses,
	//which is the detail that makes the button read as a coil rather than as
	//an animation.
	d.swell = sag * 0.06f;

	d.overscan = Overscan( s.overscan );

	return d;
}

} // namespace regauss::controls
