#include "Regauss.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Masks.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace regauss;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Regauss >,                                    // Create method
	"RG01",                                                      // Plugin unique ID of maximum length 4.
	"(re)gauss",                                                 // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	1,                                                           // Plugin major version number
	0,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"Magnetic interference on a CRT, and the coil that clears it",// Plugin description
	"(re)gauss FFGL effect"                                      // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

const char* const kModeNames[]   = { "Full CRT", "Interference Only" };
const char* const kLayoutNames[] = { "Speaker Left", "Corner Magnet", "Ring Magnet", "Wandering", "Earth's Field" };
const char* const kAutoNames[]   = { "Off", "Interval", "Beat", "Bar" };

/// Which driver in the cabinet is leaking. A woofer's field follows the bass
/// and a tweeter's the treble, so this is a band selector wearing the clothes
/// of the thing it actually models.
const char* const kBandNames[] = { "Full Range", "Woofer", "Mid", "Tweeter" };

constexpr int kModeCount = 2;
constexpr int kBandCount = 4;

/// First and last bin of each band, over the 64 the host delivers. Boundaries
/// are the usual crossover points of a three-way cabinet rather than equal
/// thirds: a woofer is done by a few hundred hertz and a tweeter does not
/// start until a few kHz, so the bass gets far fewer bins than the treble.
constexpr int kBandRange[ kBandCount ][ 2 ] = {
	{ 0, 63 }, //Full Range
	{ 0, 7 },  //Woofer
	{ 8, 27 }, //Mid
	{ 28, 63 },//Tweeter
};

float lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

//---------------------------------------------------------------------------
Regauss::Regauss() :
	startTime( std::chrono::steady_clock::now() )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The field alternates, the poles drift and the coil decays, so the effect
	//needs a clock. Asking the host for one means a re-render of the same
	//composition produces the same wobble rather than whatever the wall clock
	//happened to say when it was rendered.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults.
	//
	// SetParamInfof reads each one back out of GetFloatParameter, so these
	// assignments are what the host is told the defaults are.
	//
	// They are set to a television with a speaker beside it, not to nothing.
	// The default has a visible stain and almost no geometry error, which is
	// what the fault actually looks like -- and an effect that does nothing
	// until six sliders are moved is an effect nobody finds out is any good.
	//---------------------------------------------------------------------
	params[ PT_MODE ]           = 0.0f;

	params[ PT_LAYOUT ]         = 0.0f;//speaker on the left
	params[ PT_MAGNETISATION ]  = 0.70f;
	params[ PT_SEED ]           = 0.0f;
	params[ PT_WANDER ]         = 0.0f;
	params[ PT_INTERFERENCE ]   = 0.0f;
	params[ PT_FREQUENCY ]      = 0.364f;//50 Hz

	params[ PT_DEFLECTION ]     = 0.25f; //barely a lean while it sits there...
	params[ PT_PURITY ]         = 0.70f; //...but stains it plainly, and throws it on a degauss.
	params[ PT_CONVERGENCE ]    = 0.30f;
	params[ PT_OVERSCAN ]       = 0.25f;

	params[ PT_DEGAUSS ]        = 0.0f;
	params[ PT_AUTO ]           = 0.0f;
	params[ PT_INTERVAL ]       = 0.58f; //about 4 seconds
	params[ PT_DURATION ]       = 0.62f; //about 1.5 seconds
	params[ PT_INTENSITY ]      = 0.70f;
	params[ PT_COIL_SAG ]       = 0.60f;
	params[ PT_RECOVERY ]       = 0.65f; //about 8 seconds

	params[ PT_MASK_PATTERN ]   = 1.0f;  //shadow mask
	params[ PT_MASK_PITCH ]     = 0.30f;
	params[ PT_MASK_STRENGTH ]  = 0.60f;
	params[ PT_SCANLINES ]      = 0.50f;
	params[ PT_LINE_COUNT ]     = 0.429f;//480 lines
	params[ PT_BEAM_BLOOM ]     = 0.50f;
	params[ PT_PERSISTENCE ]    = 0.15f;
	params[ PT_HALATION ]       = 0.25f;
	params[ PT_BRIGHTNESS ]     = 0.50f;//0.5 is unity
	params[ PT_CONTRAST ]       = 0.50f;//0.5 is unity

	params[ PT_CURVATURE ]      = 0.28f;
	params[ PT_CORNER_RADIUS ]  = 0.16f;
	params[ PT_PERSPECTIVE_X ]  = 0.50f;//0.5 is straight on
	params[ PT_PERSPECTIVE_Y ]  = 0.50f;
	params[ PT_ZOOM ]           = 0.50f;//0.5 is 1:1
	params[ PT_VIGNETTE ]       = 0.35f;

	params[ PT_AUDIO_DRIVE ]     = 0.0f;//off until somebody routes audio to it
	params[ PT_AUDIO_BAND ]      = 1.0f;//woofer -- the driver with the big magnet
	params[ PT_AUDIO_RELEASE ]   = 0.42f;//about 150 ms
	params[ PT_AUDIO_TRIGGER ]   = 0.0f;
	params[ PT_AUDIO_THRESHOLD ] = 0.55f;

	params[ PT_PRESET ]         = 0.0f; //Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration. The groups matter: this is thirty-odd parameters, and an
	// ungrouped list of thirty in somebody else's inspector is unusable.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_MODE, "Render", kModeCount, params[ PT_MODE ] );
	for( int i = 0; i < kModeCount; ++i )
		SetParamElementInfo( PT_MODE, i, kModeNames[ i ], static_cast< float >( i ) );

	SetOptionParamInfo( PT_LAYOUT, "Layout", kLayoutCount, params[ PT_LAYOUT ] );
	for( int i = 0; i < kLayoutCount; ++i )
		SetParamElementInfo( PT_LAYOUT, i, kLayoutNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_MAGNETISATION, "Magnetisation", FF_TYPE_STANDARD );
	SetParamInfof( PT_SEED, "Seed", FF_TYPE_STANDARD );
	SetParamInfof( PT_WANDER, "Wander", FF_TYPE_STANDARD );
	SetParamInfof( PT_INTERFERENCE, "Interference", FF_TYPE_STANDARD );
	SetParamInfof( PT_FREQUENCY, "Frequency", FF_TYPE_STANDARD );

	SetParamInfof( PT_DEFLECTION, "Deflection", FF_TYPE_STANDARD );
	SetParamInfof( PT_PURITY, "Purity", FF_TYPE_STANDARD );
	SetParamInfof( PT_CONVERGENCE, "Convergence", FF_TYPE_STANDARD );
	SetParamInfof( PT_OVERSCAN, "Overscan", FF_TYPE_STANDARD );

	//An event, which the host draws as a button. It is the only control here
	//that is an instruction rather than a value.
	SetParamInfo( PT_DEGAUSS, "Degauss", FF_TYPE_EVENT, false );

	SetOptionParamInfo( PT_AUTO, "Auto", kAutoCount, params[ PT_AUTO ] );
	for( int i = 0; i < kAutoCount; ++i )
		SetParamElementInfo( PT_AUTO, i, kAutoNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_INTERVAL, "Interval", FF_TYPE_STANDARD );
	SetParamInfof( PT_DURATION, "Duration", FF_TYPE_STANDARD );
	SetParamInfof( PT_INTENSITY, "Intensity", FF_TYPE_STANDARD );
	SetParamInfof( PT_COIL_SAG, "Coil Sag", FF_TYPE_STANDARD );
	SetParamInfof( PT_RECOVERY, "Recovery", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_MASK_PATTERN, "Mask Pattern", maskCount(), params[ PT_MASK_PATTERN ] );
	for( int i = 0; i < maskCount(); ++i )
		SetParamElementInfo( PT_MASK_PATTERN, i, mask( i ).name, static_cast< float >( i ) );

	SetParamInfof( PT_MASK_PITCH, "Mask Pitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_MASK_STRENGTH, "Mask Strength", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCANLINES, "Scanlines", FF_TYPE_STANDARD );
	SetParamInfof( PT_LINE_COUNT, "Line Count", FF_TYPE_STANDARD );
	SetParamInfof( PT_BEAM_BLOOM, "Beam Bloom", FF_TYPE_STANDARD );
	SetParamInfof( PT_PERSISTENCE, "Persistence", FF_TYPE_STANDARD );
	SetParamInfof( PT_HALATION, "Halation", FF_TYPE_STANDARD );
	SetParamInfof( PT_BRIGHTNESS, "Brightness", FF_TYPE_STANDARD );
	SetParamInfof( PT_CONTRAST, "Contrast", FF_TYPE_STANDARD );

	SetParamInfof( PT_CURVATURE, "Curvature", FF_TYPE_STANDARD );
	SetParamInfof( PT_CORNER_RADIUS, "Corner Radius", FF_TYPE_STANDARD );
	SetParamInfof( PT_PERSPECTIVE_X, "Perspective X", FF_TYPE_STANDARD );
	SetParamInfof( PT_PERSPECTIVE_Y, "Perspective Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ZOOM, "Zoom", FF_TYPE_STANDARD );
	SetParamInfof( PT_VIGNETTE, "Vignette", FF_TYPE_STANDARD );

	// The spectrum, and what to do with it.
	//
	// Declared with a real element list so the host knows how many bins to
	// fill. Audio Drive defaults to zero: with no audio routed, the controls
	// sit there doing nothing rather than the picture twitching to a phantom
	// signal the operator never asked for.
	SetBufferParamInfo( PT_AUDIO_FFT, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO_FFT, i, "", 0.0f );

	SetParamInfof( PT_AUDIO_DRIVE, "Audio Drive", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_AUDIO_BAND, "Band", kBandCount, params[ PT_AUDIO_BAND ] );
	for( int i = 0; i < kBandCount; ++i )
		SetParamElementInfo( PT_AUDIO_BAND, i, kBandNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_AUDIO_RELEASE, "Release", FF_TYPE_STANDARD );
	SetParamInfo( PT_AUDIO_TRIGGER, "Trigger Coil", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_AUDIO_THRESHOLD, "Threshold", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	SetParamGroup( PT_MODE, "Mode" );
	for( FFUInt32 i = PT_LAYOUT; i <= PT_FREQUENCY; ++i )
		SetParamGroup( i, "Field" );
	for( FFUInt32 i = PT_DEFLECTION; i <= PT_OVERSCAN; ++i )
		SetParamGroup( i, "Beam" );
	for( FFUInt32 i = PT_DEGAUSS; i <= PT_RECOVERY; ++i )
		SetParamGroup( i, "Degauss" );
	for( FFUInt32 i = PT_MASK_PATTERN; i <= PT_CONTRAST; ++i )
		SetParamGroup( i, "Tube" );
	for( FFUInt32 i = PT_CURVATURE; i <= PT_VIGNETTE; ++i )
		SetParamGroup( i, "Geometry" );
	for( FFUInt32 i = PT_AUDIO_FFT; i <= PT_AUDIO_THRESHOLD; ++i )
		SetParamGroup( i, "Audio" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created (re)gauss effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Regauss::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

double Regauss::nowSeconds()
{
	double raw;
	if( hostTimeSeen && hostTime >= 0.0 )
	{
		raw = hostTime;
	}
	else
	{
		//No host clock at all. The wall clock is already in seconds, so the
		//unit question does not arise and the scale must not be applied to it.
		const auto elapsed = std::chrono::steady_clock::now() - startTime;
		return std::chrono::duration< double >( elapsed ).count();
	}

	//Decide the unit by measuring the host's clock against a real one: the
	//ratio is ~1 for a seconds host and ~1000 for a milliseconds host, and
	//nothing plausible sits between. This replaced a guess made from the
	//magnitude of one frame delta, which decided nothing between 0.5 and 2.0,
	//could lock to "seconds" off a burst of sub-0.5 ms frames at load, and
	//assumed seconds while undecided -- precisely the millisecond host's wrong
	//answer.
	const double wallNow =
	    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			//Several frames rather than one, so a single odd frame cannot
			//decide it alone.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow;
}

float Regauss::lastTrigger( double now ) const
{
	double scheduled = -1.0;

	const int mode = static_cast< int >( std::lround( params[ PT_AUTO ] ) );

	//The tempo the host is running at. It always sends something -- Resolume
	//calls SetBeatInfo unconditionally and the SDK defaults to 120/0 -- but a
	//host that never does would leave bpm at zero and make barSeconds
	//infinite, so it is guarded.
	const double tempo      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
	const double barSeconds = 240.0 / tempo;//four beats to the bar
	const double within     = std::clamp( static_cast< double >( barPhase ), 0.0, 1.0 );

	switch( mode )
	{
		case kAutoInterval:
			scheduled = scheduledTrigger( static_cast< float >( now ),
			                              controls::Interval( params[ PT_INTERVAL ] ) );
			break;

		case kAutoBar:
			//The host hands over the position WITHIN the current bar, so the
			//most recent bar line is simply that far back. Exact, and it uses
			//the host's own grid rather than an arithmetic one of ours that
			//would drift off it -- which is the whole point of a Bar mode.
			scheduled = now - within * barSeconds;
			break;

		case kAutoBeat:
		{
			const double beats  = within * 4.0;
			const double inBeat = beats - std::floor( beats );
			scheduled = now - inBeat * ( barSeconds / 4.0 );
			break;
		}

		case kAutoOff:
		default:
			break;
	}

	//Whichever happened more recently. A manual press during an automatic
	//sequence has to win, or the button would appear dead in exactly the
	//situation somebody is most likely to reach for it.
	return static_cast< float >( std::max( scheduled, manualTrigger ) );
}

//---------------------------------------------------------------------------
void Regauss::updateAudio( double now )
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO_FFT );
	if( info == nullptr )
		return;

	//The release filter's own clock, off the same normalised-to-seconds one
	//everything else runs on -- so the milliseconds-or-seconds question is
	//already settled by the time it gets here. A clock that has not moved
	//snaps rather than filtering, which is what the first frame needs.
	const double dt = ( audioClock >= 0.0 && now > audioClock ) ? now - audioClock : 0.0;
	audioClock      = now;

	const float releaseSeconds = controls::AudioRelease( params[ PT_AUDIO_RELEASE ] );

	//Fast up, slow down. A field that arrives a frame late reads as broken;
	//one that takes a moment to die away reads as a room with a speaker in it.
	//Symmetric smoothing trades the transient for lag, which is worse.
	const float release = dt > 0.0
	                          ? 1.0f - std::exp( static_cast< float >( -dt / releaseSeconds ) )
	                          : 1.0f;

	const size_t bins = std::min< size_t >( info->elements.size(), kAudioBins );
	for( size_t i = 0; i < bins; ++i )
	{
		//sqrt because bin magnitudes bunch hard against zero: a spectrum used
		//raw moves the picture for the kick drum and for nothing else.
		const float raw = std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );

		if( raw >= audioBins[ i ] )
			audioBins[ i ] = raw;
		else
			audioBins[ i ] += ( raw - audioBins[ i ] ) * release;
	}

	//Fold the chosen band down to one number. The mean rather than the peak:
	//a peak follows whichever bin happens to be loudest and jumps between
	//them, and the field a speaker leaks is its whole output at once.
	const int band = std::clamp( static_cast< int >( std::lround( params[ PT_AUDIO_BAND ] ) ), 0, kBandCount - 1 );
	const int from = kBandRange[ band ][ 0 ];
	const int to   = std::min( kBandRange[ band ][ 1 ], static_cast< int >( bins ) - 1 );

	float sum = 0.0f;
	int counted = 0;
	for( int i = from; i <= to; ++i )
	{
		sum += audioBins[ i ];
		++counted;
	}

	audioPrevious = audioLevel;
	audioLevel    = counted > 0 ? std::clamp( sum / static_cast< float >( counted ), 0.0f, 1.0f ) : 0.0f;

	//------------------------------------------------------------------
	// Fire the coil on a transient.
	//
	// A rising crossing of the threshold, not a derivative: a derivative
	// re-fires all the way up a swell, and the lockout that would be needed to
	// stop it is doing the same job as the hysteresis this gets for free by
	// requiring the level to fall back under the line first.
	//
	// The lockout is still there, and it is short -- it only stops a signal
	// sitting exactly on the threshold from firing every frame it dithers
	// across it.
	//------------------------------------------------------------------
	if( params[ PT_AUDIO_TRIGGER ] > 0.5f )
	{
		const float threshold = params[ PT_AUDIO_THRESHOLD ];
		const bool crossed    = audioLevel > threshold && audioPrevious <= threshold;
		const bool armed      = lastAudioTrigger < 0.0 || ( now - lastAudioTrigger ) > 0.12;

		if( crossed && armed )
		{
			manualTrigger    = now;
			lastAudioTrigger = now;
		}
	}
}

//---------------------------------------------------------------------------
bool Regauss::compileShaders()
{
	struct Pass
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};

	//The beam pass is assembled at runtime from the shared field strings, so
	//it is a std::string and the rest are promoted to match. A shader built
	//from several pieces has one consequence worth remembering: a compile
	//error's reported line number is in a file that does not exist.
	const Pass passes[] = {
		{ &beamShader, shaders::BeamFragmentSource(), "beam" },
		{ &phosphorShader, shaders::kPhosphorFragment, "phosphor" },
		{ &bloomShader, shaders::kBloomFragment, "bloom" },
		{ &blurShader, shaders::kBlurFragment, "blur" },
		{ &screenShader, shaders::kScreenFragment, "screen" },
	};

	for( const Pass& pass : passes )
	{
		if( !pass.shader->Compile( shaders::kVertex, pass.fragment.c_str() ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which pass it was.
			diag::error( std::string( "the " ) + pass.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "(re)gauss: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult Regauss::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "(re)gauss: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	phosphorIndex = 0;
	clockFrames   = 0;

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Regauss::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin. Read before
	//anything of ours touches it -- ScopedFBOBinding restores the framebuffer
	//binding and NOT the viewport, so every pass's ResizeViewPort() leaks into
	//the next one and the final pass, which draws into the host's own
	//framebuffer, has nothing of its own to size itself from.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const int outputW = std::max( 1, static_cast< int >( hostViewport[ 2 ] ) );
	const int outputH = std::max( 1, static_cast< int >( hostViewport[ 3 ] ) );

	//---------------------------------------------------------------------
	// The clock.
	//---------------------------------------------------------------------
	const double now = nowSeconds();

	//How long the beam takes to paint one raster. Measured rather than
	//assumed, because it decides whether mains interference stands still or
	//rolls: a 50 Hz field on a set scanning at 50 Hz puts every line at the
	//same phase and the bend does not move at all.
	if( lastNow >= 0.0 && now > lastNow )
	{
		const double delta = std::clamp( now - lastNow, 1.0 / 240.0, 1.0 / 10.0 );
		//Smoothed, because a single long frame should not make the roll jump.
		scanPeriod += ( delta - scanPeriod ) * 0.15;
	}

	//A manual press is recorded here rather than in SetFloatParameter,
	//because that runs outside the render and may well arrive before the
	//plugin has ever been told what time it is.
	if( manualTrigger == -2.0 )
		manualTrigger = now;

	lastNow = now;

	//Before lastTrigger() below, because a transient in the audio can fire the
	//coil and that has to be visible on this frame rather than the next.
	updateAudio( now );

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now )
		            + " scanPeriod=" + std::to_string( scanPeriod )
		            + " bpm=" + std::to_string( bpm )
		            + " barPhase=" + std::to_string( barPhase ) );

	//---------------------------------------------------------------------
	// What the field is doing.
	//---------------------------------------------------------------------
	const int maskIndex          = static_cast< int >( std::lround( params[ PT_MASK_PATTERN ] ) );
	const MaskSpec& maskSpec     = mask( maskIndex );
	const bool interferenceOnly  = std::lround( params[ PT_MODE ] ) == 1;

	controls::Settings settings;
	settings.layout        = static_cast< int >( std::lround( params[ PT_LAYOUT ] ) );
	settings.magnetisation = params[ PT_MAGNETISATION ];
	settings.seed          = params[ PT_SEED ];
	settings.wander        = params[ PT_WANDER ];
	settings.interference  = params[ PT_INTERFERENCE ];
	settings.frequency     = params[ PT_FREQUENCY ];
	settings.deflection    = params[ PT_DEFLECTION ];
	settings.purity        = params[ PT_PURITY ];
	settings.convergence   = params[ PT_CONVERGENCE ];
	settings.overscan      = params[ PT_OVERSCAN ];
	settings.duration      = params[ PT_DURATION ];
	settings.intensity     = params[ PT_INTENSITY ];
	settings.coilSag       = params[ PT_COIL_SAG ];
	settings.recovery      = params[ PT_RECOVERY ];
	settings.maskPitch     = params[ PT_MASK_PITCH ];
	settings.maskPattern   = maskIndex;
	settings.audioLevel    = audioLevel;
	settings.audioDrive    = params[ PT_AUDIO_DRIVE ];

	const controls::Drive drive = controls::drive( settings,
	                                               maskSpec,
	                                               static_cast< float >( now ),
	                                               lastTrigger( now ),
	                                               static_cast< float >( outputW ),
	                                               static_cast< float >( outputH ) );

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens BEFORE any texture is bound. FFGLFBO::Initialise
	// sizes its new colour texture inside a ScopedTextureBinding, and every
	// Scoped* binding in the SDK CLEARS to 0 on scope exit rather than
	// restoring -- so allocating a buffer silently unbinds whatever was on the
	// active unit. The symptom is the dangerous part: correct on every frame
	// except the one that allocates.
	//---------------------------------------------------------------------
	const int bloomW = std::max( 1, outputW / 4 );
	const int bloomH = std::max( 1, outputH / 4 );

	const bool usePhosphor = params[ PT_PERSISTENCE ] > 0.001f;
	const bool useHalation = !interferenceOnly && params[ PT_HALATION ] > 0.001f;

	//16-bit float rather than 8-bit: the persistence pass reads its own output
	//back every frame, and quantising a decaying trail to 256 levels bands it
	//into visible steps within a second.
	if( !beamBuffer.Ensure( outputW, outputH, GL_RGBA16F )
	    || !phosphorBuffer[ 0 ].Ensure( outputW, outputH, GL_RGBA16F )
	    || !phosphorBuffer[ 1 ].Ensure( outputW, outputH, GL_RGBA16F )
	    || !bloomBuffer[ 0 ].Ensure( bloomW, bloomH, GL_RGBA16F )
	    || !bloomBuffer[ 1 ].Ensure( bloomW, bloomH, GL_RGBA16F )
	    || !bloomBuffer[ 2 ].Ensure( bloomW, bloomH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1. Where the beam landed.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( beamBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		beamBuffer.ResizeViewPort();
		ScopedShaderBinding shader( beamShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		beamShader.Set( "InputTexture", 0 );
		beamShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		beamShader.Set( "InputSize", static_cast< float >( input.Width ), static_cast< float >( input.Height ) );
		beamShader.Set( "OutputSize", static_cast< float >( outputW ), static_cast< float >( outputH ) );

		beamShader.Set( "Pole0", drive.poles.p[ 0 ].x, drive.poles.p[ 0 ].y, drive.poles.p[ 0 ].strength );
		beamShader.Set( "Pole1", drive.poles.p[ 1 ].x, drive.poles.p[ 1 ].y, drive.poles.p[ 1 ].strength );
		beamShader.Set( "Pole2", drive.poles.p[ 2 ].x, drive.poles.p[ 2 ].y, drive.poles.p[ 2 ].strength );
		beamShader.Set( "Pole3", drive.poles.p[ 3 ].x, drive.poles.p[ 3 ].y, drive.poles.p[ 3 ].strength );
		beamShader.Set( "PoleHeight", kPoleHeight );

		beamShader.Set( "Deflection", drive.deflection );
		beamShader.Set( "PurityGainX", drive.purityGainX );
		beamShader.Set( "PurityGainY", drive.purityGainY );
		beamShader.Set( "GunSeparation", drive.gunSeparation );
		beamShader.Set( "GunTriangular", drive.gunTriangular );

		beamShader.Set( "StaticAmp", drive.staticAmp );
		beamShader.Set( "AcAmp", drive.acAmp );
		beamShader.Set( "Frequency", drive.frequency );
		beamShader.Set( "Time", static_cast< float >( now ) );
		beamShader.Set( "ScanPeriod", static_cast< float >( scanPeriod ) );

		beamShader.Set( "Swell", drive.swell );
		beamShader.Set( "Sag", drive.sag );
		beamShader.Set( "Overscan", drive.overscan );

		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. Phosphor decay. Skipped entirely at zero, which is why the cheap
	//    path stays cheap.
	//---------------------------------------------------------------------
	GLuint screenTexture = beamBuffer.GetTextureInfo().Handle;
	if( usePhosphor )
	{
		const int target  = phosphorIndex;
		const int history = 1 - phosphorIndex;

		ScopedFBOBinding fbo( phosphorBuffer[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		phosphorBuffer[ target ].ResizeViewPort();
		ScopedShaderBinding shader( phosphorShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, beamBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, phosphorBuffer[ history ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		const float decay = params[ PT_PERSISTENCE ] * 0.93f;
		phosphorShader.Set( "CurrentTexture", 0 );
		phosphorShader.Set( "HistoryTexture", 1 );
		//Blue goes out first and green hangs on longest, so a white highlight
		//dragged across the screen leaves a faintly green wake -- the detail
		//that makes the trail read as a tube rather than as a feedback buffer.
		phosphorShader.Set( "Decay", decay * 0.97f, decay, decay * 0.90f );
		quad.Draw();

		screenTexture = phosphorBuffer[ target ].GetTextureInfo().Handle;
		phosphorIndex = history;

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
	}

	//---------------------------------------------------------------------
	// 3. Halation: bright pass, then a separable blur, all at quarter size.
	//---------------------------------------------------------------------
	if( useHalation )
	{
		{
			ScopedFBOBinding fbo( bloomBuffer[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			bloomBuffer[ 0 ].ResizeViewPort();
			ScopedShaderBinding shader( bloomShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( screenTexture );

			bloomShader.Set( "SourceTexture", 0 );
			bloomShader.Set( "SourceSize", static_cast< float >( outputW ), static_cast< float >( outputH ) );
			bloomShader.Set( "Threshold", 0.5f );
			quad.Draw();
		}

		const struct
		{
			int from;
			int to;
			float dx;
			float dy;
		} blurs[] = {
			{ 0, 1, 1.0f / static_cast< float >( bloomW ), 0.0f },
			{ 1, 2, 0.0f, 1.0f / static_cast< float >( bloomH ) },
		};

		for( const auto& pass : blurs )
		{
			ScopedFBOBinding fbo( bloomBuffer[ pass.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			bloomBuffer[ pass.to ].ResizeViewPort();
			ScopedShaderBinding shader( blurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( bloomBuffer[ pass.from ].GetTextureInfo().Handle );

			blurShader.Set( "SourceTexture", 0 );
			blurShader.Set( "Direction", pass.dx, pass.dy );
			quad.Draw();
		}
	}

	//---------------------------------------------------------------------
	// 4. The glass, straight into whatever the host handed us.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( screenShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, screenTexture );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, bloomBuffer[ 2 ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		screenShader.Set( "ScreenTexture", 0 );
		screenShader.Set( "BloomTexture", 1 );
		screenShader.Set( "OutputSize", static_cast< float >( outputW ), static_cast< float >( outputH ) );

		screenShader.Set( "TubeEnabled", interferenceOnly ? 0.0f : 1.0f );

		screenShader.Set( "MaskPattern", params[ PT_MASK_PATTERN ] );
		screenShader.Set( "MaskPitch", controls::MaskPitchPixels( params[ PT_MASK_PITCH ] ) );
		screenShader.Set( "MaskStrength", params[ PT_MASK_STRENGTH ] );
		screenShader.Set( "MaskSpill", maskSpec.spill );
		screenShader.Set( "MaskGain", maskSpec.gain );

		screenShader.Set( "Scanlines", params[ PT_SCANLINES ] );
		screenShader.Set( "LineCount", controls::LineCount( params[ PT_LINE_COUNT ] ) );
		screenShader.Set( "BeamBloom", params[ PT_BEAM_BLOOM ] );
		screenShader.Set( "Halation", useHalation ? params[ PT_HALATION ] * 0.8f : 0.0f );
		screenShader.Set( "Brightness", params[ PT_BRIGHTNESS ] * 2.0f );
		screenShader.Set( "Contrast", params[ PT_CONTRAST ] * 2.0f );

		screenShader.Set( "Curvature", params[ PT_CURVATURE ] * 0.6f );
		screenShader.Set( "CornerRadius", params[ PT_CORNER_RADIUS ] * 0.35f );
		//Plus or minus about fifty degrees. Past that the near edge of the
		//screen is closer to the eye than the focal length and the projection
		//stops meaning anything.
		screenShader.Set( "PerspectiveX", ( params[ PT_PERSPECTIVE_X ] - 0.5f ) * 1.8f );
		screenShader.Set( "PerspectiveY", ( params[ PT_PERSPECTIVE_Y ] - 0.5f ) * 1.8f );
		screenShader.Set( "Zoom", lerp( 0.5f, 1.5f, params[ PT_ZOOM ] ) );
		screenShader.Set( "Vignette", params[ PT_VIGNETTE ] );

		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void Regauss::releaseBuffers()
{
	beamBuffer.Destroy();
	for( auto& buffer : phosphorBuffer )
		buffer.Destroy();
	for( auto& buffer : bloomBuffer )
		buffer.Destroy();
}

FFResult Regauss::DeInitGL()
{
	beamShader.FreeGLResources();
	phosphorShader.FreeGLResources();
	bloomShader.FreeGLResources();
	blurShader.FreeGLResources();
	screenShader.FreeGLResources();
	quad.Release();
	releaseBuffers();

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Regauss::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_DEGAUSS )
	{
		// Rising edge only. An event parameter goes to 1 and back to 0 as the
		// host draws the press and the release, and firing the coil on both
		// would run it twice for one click.
		//
		// -2 is "pressed, but this call has no idea what time it is". The next
		// ProcessOpenGL turns it into a real timestamp. Recording the press
		// here against a clock we have not read yet is the obvious version and
		// it fires the coil at t=0 for the whole first second of a session.
		if( value >= 0.5f && params[ PT_DEGAUSS ] < 0.5f )
			manualTrigger = -2.0;

		params[ PT_DEGAUSS ] = value;
		return FF_SUCCESS;
	}

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters --
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

FFResult Regauss::SetTextParameter( unsigned int index, const char* /*value*/ )
{
	// Display only -- there is nothing to store. It has to return FF_SUCCESS
	// all the same: see the declaration in Regauss.h. Anything else here and
	// no real host can instantiate the plugin at all.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return FF_FAIL;
}

void Regauss::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Regauss::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

char* Regauss::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

void Regauss::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
