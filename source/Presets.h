#pragma once

/**
    Factory presets: named situations an operator can reach in one gesture.

    Each is a room with a television in it and something magnetic nearby, not a
    collection of slider positions that happened to look good. The controls
    here are the parts of one physical arrangement, so a coherent look is a
    coherent story about that arrangement -- where the magnet is, how strong,
    whether it is moving, and whether anybody has degaussed the set lately.

    The values live in the same 0..1 parameter space both builds expose, so ONE
    table drives both and a preset looks identical in Resolume and Resolve.
    Plain data only; the application machinery lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this table:
    it means "the sliders are the truth".

    Presets deliberately do NOT cover Seed (which arrangement of the same
    layout is nobody's business but the operator's), the Degauss button
    (pressing it is an event, not a value), or Perspective and Zoom (framing --
    where the viewer sits is not part of the fault).
*/

namespace regauss
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kMode,
	kLayout,
	kMagnetisation,
	kWander,
	kInterference,
	kFrequency,
	kDeflection,
	kPurity,
	kConvergence,
	kOverscan,
	kAuto,
	kInterval,
	kDuration,
	kIntensity,
	kCoilSag,
	kRecovery,
	kMaskPattern,
	kMaskPitch,
	kMaskStrength,
	kScanlines,
	kLineCount,
	kBeamBloom,
	kPersistence,
	kHalation,
	kBrightness,
	kContrast,
	kCurvature,
	kCornerRadius,
	kVignette,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices. Mode 0 Full CRT / 1 Interference Only;
// Layout 0 Speaker Left / 1 Corner Magnet / 2 Ring Magnet / 3 Wandering /
// 4 Earth's Field; Auto 0 Off / 1 Interval / 2 Beat / 3 Bar; Mask 0 None /
// 1 Shadow Mask / 2 Aperture Grille / 3 Slot Mask / 4 RGB Stripe.
//
// Frequency is 10..120 Hz linear, so 50 Hz is 0.364 and 60 Hz is 0.455.
// Line Count is 120..960 linear, so 480 lines is 0.429 and 576 is 0.543.
// Interval, Duration and Recovery are geometric -- see Controls.cpp.
// Brightness and Contrast sit at unity on 0.5.
inline constexpr Preset kPresets[] = {
	// The fault everybody has actually seen: an unshielded speaker left
	// against the side of the set for months. Almost no geometry error -- a
	// magnet this weak does not visibly bend the raster -- and a large soft
	// stain down one side, because purity is the sensitive one.
	{ "Speaker Beside It",
	  { /*Mode*/ 0, /*Layout*/ 0, /*Magn*/ 0.55f, /*Wander*/ 0.0f, /*Intf*/ 0.0f, /*Freq*/ 0.364f,
	    /*Defl*/ 0.06f, /*Purity*/ 0.55f, /*Conv*/ 0.30f, /*Over*/ 0.25f,
	    /*Auto*/ 0, /*Ivl*/ 0.58f, /*Dur*/ 0.62f, /*Int*/ 0.7f, /*Sag*/ 0.6f, /*Recov*/ 0.65f,
	    /*Mask*/ 1, /*Pitch*/ 0.30f, /*MaskStr*/ 0.6f, /*Scan*/ 0.5f, /*Lines*/ 0.429f,
	    /*Bloom*/ 0.5f, /*Persist*/ 0.15f, /*Halation*/ 0.25f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f,
	    /*Curve*/ 0.28f, /*Corner*/ 0.16f, /*Vig*/ 0.35f } },

	// Something small held against one corner of the glass. A tight pair of
	// poles means a steep gradient, and a steep gradient is convergence error,
	// so this is the one that fringes: coloured edges on everything in that
	// corner, and a hard-edged stain rather than a soft one.
	{ "Corner Stain",
	  { /*Mode*/ 0, /*Layout*/ 1, /*Magn*/ 0.80f, /*Wander*/ 0.0f, /*Intf*/ 0.0f, /*Freq*/ 0.364f,
	    /*Defl*/ 0.14f, /*Purity*/ 0.75f, /*Conv*/ 0.55f, /*Over*/ 0.30f,
	    /*Auto*/ 0, /*Ivl*/ 0.58f, /*Dur*/ 0.55f, /*Int*/ 0.8f, /*Sag*/ 0.6f, /*Recov*/ 0.72f,
	    /*Mask*/ 1, /*Pitch*/ 0.26f, /*MaskStr*/ 0.65f, /*Scan*/ 0.5f, /*Lines*/ 0.429f,
	    /*Bloom*/ 0.5f, /*Persist*/ 0.15f, /*Halation*/ 0.28f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f,
	    /*Curve*/ 0.28f, /*Corner*/ 0.16f, /*Vig*/ 0.35f } },

	// A transformer on the other side of the wall. Nothing retained at all --
	// the mask is clean -- but the field alternates at mains frequency, and
	// because the set scans at very nearly the same rate the bend crawls
	// slowly up the picture instead of flickering in place.
	{ "Mains Hum",
	  { /*Mode*/ 0, /*Layout*/ 2, /*Magn*/ 0.05f, /*Wander*/ 0.0f, /*Intf*/ 0.35f, /*Freq*/ 0.364f,
	    /*Defl*/ 0.30f, /*Purity*/ 0.35f, /*Conv*/ 0.35f, /*Over*/ 0.35f,
	    /*Auto*/ 0, /*Ivl*/ 0.58f, /*Dur*/ 0.62f, /*Int*/ 0.7f, /*Sag*/ 0.6f, /*Recov*/ 0.65f,
	    /*Mask*/ 1, /*Pitch*/ 0.32f, /*MaskStr*/ 0.6f, /*Scan*/ 0.55f, /*Lines*/ 0.543f,
	    /*Bloom*/ 0.5f, /*Persist*/ 0.18f, /*Halation*/ 0.25f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f,
	    /*Curve*/ 0.30f, /*Corner*/ 0.18f, /*Vig*/ 0.38f } },

	// The performance setting: the coil fires itself on every bar. Big
	// intensity and a long-ish decay, so each bar line throws the picture
	// about and it settles just in time for the next one.
	{ "Degauss on the Bar",
	  { /*Mode*/ 0, /*Layout*/ 0, /*Magn*/ 0.70f, /*Wander*/ 0.15f, /*Intf*/ 0.05f, /*Freq*/ 0.364f,
	    /*Defl*/ 0.35f, /*Purity*/ 0.6f, /*Conv*/ 0.45f, /*Over*/ 0.35f,
	    /*Auto*/ 3, /*Ivl*/ 0.58f, /*Dur*/ 0.60f, /*Int*/ 0.85f, /*Sag*/ 0.8f, /*Recov*/ 0.42f,
	    /*Mask*/ 1, /*Pitch*/ 0.30f, /*MaskStr*/ 0.6f, /*Scan*/ 0.5f, /*Lines*/ 0.429f,
	    /*Bloom*/ 0.6f, /*Persist*/ 0.35f, /*Halation*/ 0.35f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f,
	    /*Curve*/ 0.28f, /*Corner*/ 0.16f, /*Vig*/ 0.35f } },

	// Nothing is bolted down. The poles orbit, so the stain drifts across the
	// face and the geometry breathes with it. Persistence up, because the
	// smear along the path is most of what sells it.
	{ "Nothing Bolted Down",
	  { /*Mode*/ 0, /*Layout*/ 3, /*Magn*/ 0.60f, /*Wander*/ 0.75f, /*Intf*/ 0.10f, /*Freq*/ 0.30f,
	    /*Defl*/ 0.40f, /*Purity*/ 0.65f, /*Conv*/ 0.50f, /*Over*/ 0.40f,
	    /*Auto*/ 0, /*Ivl*/ 0.58f, /*Dur*/ 0.62f, /*Int*/ 0.7f, /*Sag*/ 0.6f, /*Recov*/ 0.55f,
	    /*Mask*/ 3, /*Pitch*/ 0.34f, /*MaskStr*/ 0.6f, /*Scan*/ 0.5f, /*Lines*/ 0.429f,
	    /*Bloom*/ 0.6f, /*Persist*/ 0.45f, /*Halation*/ 0.35f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f,
	    /*Curve*/ 0.32f, /*Corner*/ 0.18f, /*Vig*/ 0.40f } },

	// A Trinitron with the same magnet on it. The stripes run the full height
	// of the tube, so a vertical landing error slides the beam along its own
	// stripe and costs nothing -- the stain is narrower and the picture is
	// brighter, which is what the design was for. Fine pitch, so what purity
	// error is left bites harder.
	{ "Trinitron, Magnetised",
	  { /*Mode*/ 0, /*Layout*/ 0, /*Magn*/ 0.65f, /*Wander*/ 0.0f, /*Intf*/ 0.0f, /*Freq*/ 0.364f,
	    /*Defl*/ 0.08f, /*Purity*/ 0.6f, /*Conv*/ 0.25f, /*Over*/ 0.20f,
	    /*Auto*/ 0, /*Ivl*/ 0.58f, /*Dur*/ 0.62f, /*Int*/ 0.7f, /*Sag*/ 0.6f, /*Recov*/ 0.65f,
	    /*Mask*/ 2, /*Pitch*/ 0.20f, /*MaskStr*/ 0.55f, /*Scan*/ 0.45f, /*Lines*/ 0.62f,
	    /*Bloom*/ 0.4f, /*Persist*/ 0.10f, /*Halation*/ 0.20f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f,
	    /*Curve*/ 0.08f, /*Corner*/ 0.10f, /*Vig*/ 0.18f } },

	// A yoke on its way out: heavy deflection, a coil that pulls the HT down
	// hard, and a mask that takes its magnetisation straight back on. Fires
	// itself every few seconds and never quite recovers.
	{ "Failing Yoke",
	  { /*Mode*/ 0, /*Layout*/ 2, /*Magn*/ 0.85f, /*Wander*/ 0.35f, /*Intf*/ 0.30f, /*Freq*/ 0.41f,
	    /*Defl*/ 0.55f, /*Purity*/ 0.7f, /*Conv*/ 0.65f, /*Over*/ 0.45f,
	    /*Auto*/ 1, /*Ivl*/ 0.50f, /*Dur*/ 0.70f, /*Int*/ 1.0f, /*Sag*/ 1.0f, /*Recov*/ 0.25f,
	    /*Mask*/ 1, /*Pitch*/ 0.38f, /*MaskStr*/ 0.65f, /*Scan*/ 0.55f, /*Lines*/ 0.38f,
	    /*Bloom*/ 0.8f, /*Persist*/ 0.5f, /*Halation*/ 0.55f, /*Bright*/ 0.45f, /*Contrast*/ 0.45f,
	    /*Curve*/ 0.38f, /*Corner*/ 0.20f, /*Vig*/ 0.5f } },

	// For stacking on old-cathode, or on somebody's own CRT footage: the
	// magnet and the coil, and no television of our own. Purity still works --
	// it uses the mask pitch as an implied scale rather than a drawn one --
	// so the colour stain is there without a second shadow mask in the chain.
	{ "Interference Only",
	  { /*Mode*/ 1, /*Layout*/ 0, /*Magn*/ 0.55f, /*Wander*/ 0.10f, /*Intf*/ 0.15f, /*Freq*/ 0.364f,
	    /*Defl*/ 0.20f, /*Purity*/ 0.5f, /*Conv*/ 0.35f, /*Over*/ 0.10f,
	    /*Auto*/ 0, /*Ivl*/ 0.58f, /*Dur*/ 0.62f, /*Int*/ 0.8f, /*Sag*/ 0.5f, /*Recov*/ 0.6f,
	    /*Mask*/ 1, /*Pitch*/ 0.30f, /*MaskStr*/ 0.0f, /*Scan*/ 0.0f, /*Lines*/ 0.429f,
	    /*Bloom*/ 0.3f, /*Persist*/ 0.20f, /*Halation*/ 0.0f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f,
	    /*Curve*/ 0.0f, /*Corner*/ 0.0f, /*Vig*/ 0.0f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace regauss
