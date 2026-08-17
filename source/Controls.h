#pragma once

#include "Field.h"

namespace regauss
{
struct MaskSpec;

/**
    Host parameters in, physical quantities out.

    Every control a host shows is 0..1, because `CFFGLPluginManager::SetParamInfo`
    clamps a `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange` gets a
    chance to widen it, and there is no `SetParamDefault` to work round it (SDK
    b1afaf9). So a parameter declared in seconds cannot declare a default in
    seconds, and the fleet's answer is to keep every slider 0..1 and do the
    mapping here.

    Which makes this file the one place the plugin says what it means by any of
    its numbers -- and, more usefully, the only place the FFGL build and the
    OpenFX build can disagree. Both call `drive()`. Neither computes a
    sensitivity of its own.
*/
namespace controls
{
//---------------------------------------------------------------------------
// The two sensitivities.
//
// These are the constants that decide what the plugin feels like, and they are
// separate for a physical reason rather than a convenient one.
//
// How far a given field bends the beam is set by the deflection yoke and the
// anode voltage. How much of a colour error that bend causes is set by the
// shadow-mask pitch and the distance from the gun to the mask. They are
// different pieces of hardware, and their ratio on a real tube is such that
// the interesting region is nearly all at one end: a magnet weak enough to
// leave the geometry visibly untouched will still rotate the colours
// completely. That is the commonest real purity fault there is, and a plugin
// that derived one control from the other could not reach it.
//---------------------------------------------------------------------------

/// Picture units of displacement per unit of field, at Deflection = 1.
///
/// Measured against the default Speaker Left layout, whose field peaks at
/// about 0.44 at the near edge: 0.5 there is a bend of some eleven per cent of
/// the screen width at full magnetisation -- far more than any real magnet,
/// and the right place for the top of a slider on an effect.
///
/// The useful consequence of leaving this well above what the static field
/// needs: the coil's field is several times larger than the mask's own, so one
/// setting gives a barely-there lean while the set sits there and a proper
/// throw when the button is pressed.
inline constexpr float kDeflectScale = 0.5f;

/// Phosphor widths of landing error per unit of field, at Purity = 1 and the
/// reference mask pitch. Calibrated the same way: about 1.1 phosphors at the
/// near edge of the default layout, so Purity = 1 rotates the colours through
/// rather more than a whole stripe. Past two or three it stops reading as a
/// stain and starts reading as rainbow banding -- a real thing a real magnet
/// does, but not one worth spending the top half of a slider on.
inline constexpr float kPurityScale = 4.0f;

/// The mask pitch, in output pixels, that kPurityScale is calibrated against.
/// A finer mask stains worse under the same field, which is why the pitch is
/// in this calculation at all and why a fine-pitch monitor is ruined by a
/// magnet a coarse television shrugs off.
inline constexpr float kReferencePitch = 4.0f;

//---------------------------------------------------------------------------
// Mappings. Each is the physical range the fleet's 0..1 slider covers.
//---------------------------------------------------------------------------
float Deflection( float p );

/// Output pixels per phosphor stripe.
float MaskPitchPixels( float p );

/// Scan lines across the picture. There is no broadcast standard here to take
/// it from -- this plugin runs at the composition's resolution -- so the
/// operator says what sort of set it was.
float LineCount( float p );

/// How far apart the three cathodes sit, in square tube units. Zero puts all
/// three beams on the same path, which removes convergence error entirely
/// while leaving geometry and purity error untouched: the field is still
/// there, the three guns just all see the same one.
float GunSeparation( float p );

/// Seconds between automatic firings, in Interval mode.
float Interval( float p );

/// Seconds for the coil's field to fall to one per cent of its peak.
float Duration( float p );

/// Peak field the coil applies, relative to the ambient interference scale.
float Intensity( float p );

/// Seconds for the mask to take its magnetisation back on after a degauss.
/// Never infinite: a Magnetisation slider that does nothing because the
/// operator degaussed a minute ago reads as a broken plugin, however defensible
/// "the mask is clean" is as a model.
float Recovery( float p );

/// How much wider than its own face the set scans. A real television
/// overscans by a few per cent so the blanking edges hide behind the bezel.
float Overscan( float p );

/// Seconds for the audio-driven field to fall back after a peak.
float AudioRelease( float p );

/// Field per unit of audio, at Audio Drive = 1.
///
/// One, so that a full-scale signal at full drive is worth the same as
/// Magnetisation at maximum. The two are the same quantity arriving by
/// different routes -- what the mask is holding, and what the speaker is
/// putting out this instant.
inline constexpr float kAudioScale = 1.0f;

//---------------------------------------------------------------------------
/// The 0..1 controls that feed the field and the beam. Named rather than an
/// array so that the OpenFX build, which has its own parameter handles and its
/// own ordering, cannot fill them in the wrong order.
struct Settings
{
	int layout        = 0;
	float magnetisation = 0.0f;
	float seed        = 0.0f;
	float wander      = 0.0f;
	float interference = 0.0f;
	float frequency   = 0.0f;

	float deflection  = 0.0f;
	float purity      = 0.0f;
	float convergence = 0.0f;
	float overscan    = 0.0f;

	float duration    = 0.0f;
	float intensity   = 0.0f;
	float coilSag     = 0.0f;
	float recovery    = 0.0f;

	float maskPitch   = 0.0f;
	int maskPattern   = 0;

	/// The speaker's own field, this instant.
	///
	/// Already band-selected and smoothed by the caller -- `drive()` is a pure
	/// function and the smoothing is a filter with memory, so it cannot live
	/// here. Zero by default, which is what makes the OpenFX build's ignorance
	/// of audio cost nothing: it never sets these, and the term falls out.
	float audioLevel = 0.0f;
	float audioDrive = 0.0f;
};

/// Everything the beam pass needs, worked out once per frame on the CPU.
///
/// It is all here rather than in the shader because it is all per-frame rather
/// than per-pixel, because the OpenFX build needs exactly the same numbers,
/// and because a uniform that turns out to be wrong is far easier to print
/// than a temporary halfway down a fragment shader.
struct Drive
{
	PoleSet poles;

	float staticAmp = 0.0f;//!< what the mask is holding, scaled
	float acAmp     = 0.0f;//!< ambient interference plus the coil
	float frequency = 0.0f;//!< Hz

	float deflection  = 0.0f;
	float purityGainX = 0.0f;
	float purityGainY = 0.0f;
	float gunSeparation = 0.0f;
	float gunTriangular = 0.0f;

	float swell = 0.0f;//!< the picture grows while the HT is down
	float sag   = 0.0f;//!< ...and dims
	float overscan = 1.0f;
};

/// Work out the frame.
///
/// `lastTrigger` is when the coil last fired, in the same seconds as `now`, or
/// negative for "not yet". Everything else is a parameter or a fact about the
/// output. No state is kept and no clock is read: hand this the same arguments
/// twice and it returns the same answer, which is what lets the OpenFX build
/// render a timeline out of order.
Drive drive( const Settings& s,
             const MaskSpec& mask,
             float now,
             float lastTrigger,
             float outputWidth,
             float outputHeight );

} // namespace controls
} // namespace regauss
