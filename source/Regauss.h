#pragma once

#include <FFGLSDK.h>

#include <chrono>

#include "Controls.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    (re)gauss -- a magnet near a television, and the coil that fixes it.

    A colour CRT paints its picture by steering three electron beams with a
    magnetic field. Put another magnetic field anywhere near it and the beams
    go somewhere else. That is the entire plugin: one vector field over the
    tube's face, read three times -- once per gun -- and the three things
    everybody recognises fall out of it rather than being drawn.

      * the picture leans and bulges                     (geometry error)
      * every edge grows coloured fringes                (convergence error,
                                                          from the field's
                                                          GRADIENT, not its
                                                          strength)
      * whole regions turn the wrong colour              (purity error, from
                                                          the field measured in
                                                          shadow-mask pitches)

    The Degauss button is not an animation over the top of that. It fires a
    decaying alternating field, and the mask's own magnetisation is walked down
    by the same envelope -- so pressing it genuinely clears the state the
    plugin has accumulated, the picture swells and dims while the coil loads
    the HT, and then the set is clean until it magnetises again.

    See Field.h for the model, Shaders.h for the passes, and AGENTS.md for the
    traps.
*/
class Regauss : public CFFGLPlugin
{
public:
	Regauss();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Display-only text still has to accept a write.
	///
	/// `instantiateGL` sets EVERY parameter's default on a fresh instance and
	/// deletes the instance if any set returns FF_FAIL (SDK b1afaf9, FFGL.cpp
	/// ~289), and the base class's SetTextParameter is a stub that returns
	/// FF_FAIL. So a plugin that declares the About text block without
	/// overriding this cannot be instantiated by any real host at all -- while
	/// remaining perfectly happy in every harness in this repo, because they
	/// drive the plugin class directly and never go through plugMain.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;

private:
	/// Everything the operator can reach, in the order Resolume shows them:
	/// what is in the room, what the beam does about it, the coil, the tube,
	/// and where you are sitting.
	enum ParamID : FFUInt32
	{
		//Mode
		PT_MODE,

		//Field
		PT_LAYOUT,
		PT_MAGNETISATION,
		PT_SEED,
		PT_WANDER,
		PT_INTERFERENCE,
		PT_FREQUENCY,

		//Beam
		PT_DEFLECTION,
		PT_PURITY,
		PT_CONVERGENCE,
		PT_OVERSCAN,

		//Degauss
		PT_DEGAUSS,
		PT_AUTO,
		PT_INTERVAL,
		PT_DURATION,
		PT_INTENSITY,
		PT_COIL_SAG,
		PT_RECOVERY,

		//Tube
		PT_MASK_PATTERN,
		PT_MASK_PITCH,
		PT_MASK_STRENGTH,
		PT_SCANLINES,
		PT_LINE_COUNT,
		PT_BEAM_BLOOM,
		PT_PERSISTENCE,
		PT_HALATION,
		PT_BRIGHTNESS,
		PT_CONTRAST,

		//Geometry
		PT_CURVATURE,
		PT_CORNER_RADIUS,
		PT_PERSPECTIVE_X,
		PT_PERSPECTIVE_Y,
		PT_ZOOM,
		PT_VIGNETTE,

		//Preset. Declared after the real controls so their IDs -- which a
		//saved composition refers to -- do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ regauss::presets::kParamCount ] = {
		PT_MODE, PT_LAYOUT, PT_MAGNETISATION, PT_WANDER, PT_INTERFERENCE, PT_FREQUENCY,
		PT_DEFLECTION, PT_PURITY, PT_CONVERGENCE, PT_OVERSCAN,
		PT_AUTO, PT_INTERVAL, PT_DURATION, PT_INTENSITY, PT_COIL_SAG, PT_RECOVERY,
		PT_MASK_PATTERN, PT_MASK_PITCH, PT_MASK_STRENGTH, PT_SCANLINES, PT_LINE_COUNT,
		PT_BEAM_BLOOM, PT_PERSISTENCE, PT_HALATION, PT_BRIGHTNESS, PT_CONTRAST,
		PT_CURVATURE, PT_CORNER_RADIUS, PT_VIGNETTE
	};

	void applyPreset( int presetIndex );

	bool compileShaders();
	void releaseBuffers();

	/// The host's clock, in seconds, whatever unit it arrived in.
	///
	/// Resolume sends `SetTime` in MILLISECONDS -- measured live in Arena
	/// 7.27.1 at 20.0 per frame at 50 fps. The FFGL header never says, this
	/// repo's own harness sends seconds, and the SDK's Particles sample
	/// quietly divides by a thousand. Getting it wrong here is not a subtle
	/// mis-tuning: a one-second degauss would last a millisecond and the beat
	/// grid would be off by three orders of magnitude. So the unit is decided
	/// from the first plausible frame delta and nothing consumes `hostTime`
	/// raw.
	double nowSeconds();

	/// When the coil last fired, in the same seconds, or negative for "not
	/// yet". The later of the manual button and whatever the Auto schedule
	/// says.
	float lastTrigger( double now ) const;

	ffglex::FFGLShader beamShader;
	ffglex::FFGLShader phosphorShader;
	ffglex::FFGLShader bloomShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader screenShader;
	ffglex::FFGLScreenQuad quad;

	regauss::PassBuffer beamBuffer;
	regauss::PassBuffer phosphorBuffer[ 2 ];
	regauss::PassBuffer bloomBuffer[ 3 ];

	int phosphorIndex = 0;//!< which half of the ping-pong this frame writes to

	//--- the clock ---------------------------------------------------------
	bool hostTimeSeen = false;
	std::chrono::steady_clock::time_point startTime;
	double clockScale  = 0.0; //!< 0 = undecided, 1 = seconds, 0.001 = milliseconds
	double lastRawTime = -1.0;
	double lastNow     = -1.0;
	int clockFrames    = 0;

	/// How long the beam takes to paint one raster, measured from the host's
	/// own frame delta rather than assumed.
	///
	/// It has to be real, because it is what decides whether mains
	/// interference stands still or rolls: a 50 Hz field on a set scanning at
	/// 50 Hz puts every line at the same phase and the bend does not move,
	/// and one hertz out it crawls. Assume 60 and that behaviour is simply
	/// wrong at any other rate.
	double scanPeriod = 1.0 / 60.0;

	/// Set by the Degauss button, on the rising edge only.
	double manualTrigger = -1.0;

	float params[ PT_COUNT ];

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
