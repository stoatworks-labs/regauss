#pragma once

/**
    The magnetic field, and what the coil does to it.

    Everything (re)gauss puts on screen comes from one vector field over the
    tube's face. The field displaces the beam; that displacement is the
    geometry error, its *gradient* is the convergence error, and its size
    measured in shadow-mask pitches is the purity error. Three symptoms, one
    cause. If a symptom needs its own control to look right, the field is wrong
    somewhere -- adding a "colour fringe" slider here would be the same mistake
    as drawing dot crawl directly in old-cathode.

    This header is host-agnostic on purpose. It is the scalar half of the model:
    where the magnets are and what the degauss coil is doing this instant. The
    per-pixel half lives in the GLSL (Screen.cpp), is mirrored on the CPU by the
    OpenFX build, and is mirrored again in the browser demo -- so `fieldAt()`
    below is the reference the other two are checked against, and its GLSL twin
    is marked `//= mirrored`.

    Nothing in here touches OpenGL, and nothing in here keeps state. The field
    at time t is a pure function of (t, parameters, when the coil last fired),
    which is what lets the OpenFX build render a timeline out of order and get
    the same picture the FFGL build shows live.
*/

namespace regauss
{
//---------------------------------------------------------------------------
// The magnets.
//---------------------------------------------------------------------------

/// One magnet, somewhere near the tube.
///
/// Position is in tube coordinates: the face spans -1..1 in both axes, so a
/// pole at x = -1.3 is just off the left-hand edge of the glass, which is
/// where a bookshelf speaker actually sits.
struct Pole
{
	float x;
	float y;
	float strength;//!< signed -- a north pole and a south pole push opposite ways
};

/// Four is enough to be lumpy and few enough to be free.
///
/// The point of several poles rather than one global gradient is that a real
/// purity fault is *patchy*: a stain in one corner, nothing in the middle, a
/// different stain on the opposite edge. A single dipole gives a field that
/// varies monotonically across the face, which reads as a lens rather than as
/// a magnetised mask.
inline constexpr int kPoleCount = 4;

struct PoleSet
{
	Pole p[ kPoleCount ];
};

/// Where the magnets are, as named situations rather than eight coordinates.
///
/// Positions below are written in HALF-WIDTHS: x = -1.2 means "a fifth of the
/// way off the left-hand edge of the glass", whatever shape the glass is.
/// `poles()` multiplies them by the aspect ratio to get the square units the
/// field is evaluated in. Without that step every layout is calibrated for a
/// square screen, and on a 16:9 composition a magnet meant to sit beside the
/// set ends up two thirds of the way into the picture.
enum Layout
{
	kSpeakerLeft = 0,//!< one big driver beside the set, the classic domestic fault
	kCornerMagnet,   //!< something small held against a corner of the glass
	kRingMagnet,     //!< four poles around the rim: a badly seated purity ring
	kWandering,      //!< the poles orbit, so the stain drifts across the face
	kEarthField,     //!< a single very weak, very broad tilt -- the set was turned round
	kLayoutCount
};

/// The poles for a layout, at a moment.
///
/// `seed` reshuffles the positions within the layout's character; `wander`
/// (0..1) is how far they drift, and `time` is what they drift against. A
/// layout with wander at zero is stationary, which matters: a stain that
/// crawls when the operator did not ask it to reads as a bug in the plugin
/// rather than as a magnet in the room.
///
/// `aspect` is the composition's width over its height. Only the layout's own
/// x positions are scaled by it -- the seed jitter and the drift are physical
/// displacements of a real magnet, which are the same size in both axes.
PoleSet poles( int layout, float seed, float wander, float time, float aspect );

/// The field at a point on the face, in tube coordinates.
///
/// Each pole sits a little way *off* the plane of the screen, which is both
/// true and the reason there is a softening term: with the pole in the plane
/// the field goes to infinity where the operator put it, and the picture
/// develops a single blown pixel that no amount of clamping makes look
/// magnetic.
///
///     b = sum over poles of  strength * d / (|d|^2 + soft^2)
///
/// ------------------------------------------------------------- the exponent
///
/// That denominator is NOT squared, and a point magnet's field says it should
/// be. The exponent is one because what bends the beam is not the field where
/// the beam lands -- it is the field integrated along the whole path the beam
/// flies, from the gun at the back of the neck to the glass at the front. A
/// line integral through a point source's field comes out a full power gentler
/// than the field itself, and that is the difference between a stain that
/// covers a third of the screen, which is what a speaker beside a television
/// actually does, and a bright sliver two centimetres wide at the very edge,
/// which is what the un-integrated law gives and which was the first thing
/// this plugin rendered.
///
/// Mirrored in GLSL. Change one, change both, and run `rgtest --field`.
void fieldAt( const PoleSet& set, float px, float py, float& bx, float& by );

/// How far off the plane the poles sit. Not a taste knob: it sets the width of
/// a stain relative to the face, and every mask-pitch figure below was chosen
/// against this value.
inline constexpr float kPoleHeight = 0.55f;

//---------------------------------------------------------------------------
// The coil.
//---------------------------------------------------------------------------

/// What the degauss coil is doing, at one instant.
struct Coil
{
	/// Amplitude of the alternating field the coil is applying, 0 when idle.
	/// This is what shakes the picture.
	float ac;

	/// What is left of the mask's own magnetisation, 0..1. Starts at 1, and
	/// the coil walks it down to 0 -- which is the entire purpose of degaussing
	/// and the reason the button is not just an animation.
	float retained;

	/// How far the coil has pulled the HT down, 0..1. A degauss coil is a
	/// serious load, and on an ordinary set the picture dims and *swells* while
	/// it runs: less anode voltage means a slower beam, and a slower beam is
	/// deflected further by the same yoke current.
	float sag;
};

/// The coil's state `secondsSinceTrigger` after it fired.
///
/// A real automatic degausser is a coil in series with a PTC thermistor: at
/// switch-on the thermistor is cold and passes a large mains-frequency
/// current, the current heats it, its resistance climbs, and the field dies
/// away over a second or two. So the envelope is a decaying exponential
/// carrying a mains-frequency oscillation, and the *decay*, not the
/// oscillation, is what actually demagnetises the mask -- the field has to
/// pass through every amplitude on its way down so that every domain gets
/// flipped one last time and then left where it lies.
///
/// `duration` is the decay time constant in seconds; `intensity` scales the
/// peak. Before the trigger (a negative argument) the coil is idle and
/// `retained` is 1.
Coil coil( float secondsSinceTrigger, float duration, float intensity );

/// The mains frequency the coil hums at, and the AC interference with it.
/// Not an option list: 50 and 60 are the two answers, but a plugin that only
/// offers those cannot beat the field rate against anything else, and the
/// slow roll you get at 48 or 72 is half of why this is worth having.
float acFrequency( float param );

//---------------------------------------------------------------------------
// Scheduling.
//---------------------------------------------------------------------------

enum AutoMode
{
	kAutoOff = 0,//!< only the button fires the coil
	kAutoInterval,
	kAutoBeat,
	kAutoBar,
	kAutoCount
};

/// The most recent scheduled trigger at or before `now`, or a negative number
/// if the schedule has not fired yet.
///
/// `gridSeconds` is the interval between firings -- literal seconds in
/// Interval mode, and the length of a beat or a bar worked out from the host's
/// tempo in the other two. Deliberately arithmetic rather than a counter: the
/// OpenFX build renders a timeline in whatever order it likes, and a counter
/// would give a different answer on a scrub than on a playthrough.
float scheduledTrigger( float now, float gridSeconds );

} // namespace regauss
