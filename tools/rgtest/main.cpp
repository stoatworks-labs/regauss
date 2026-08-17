/**
    rgtest -- drive (re)gauss offline and write a PNG, or check its maths.

    Driving Resolume from an agent session is not reliable, and "it compiled" is
    no evidence at all that a field model does what its comments claim. So this
    builds a headless GL 4.1 core context, drives the real Regauss class through
    the real FFGL entry sequence, and writes out the frame.

    The checks are the part worth having:

      --field    renders the GLSL field through a probe assembled from the SAME
                 strings the beam pass uses, and compares it against Field.cpp
                 on the CPU. Nothing else can catch the two drifting apart, and
                 they are what the OpenFX build and the browser demo both copy.
      --coil     the degauss envelope, on the CPU: does the field really fall to
                 one per cent over the stated duration, does the retained
                 magnetisation really reach zero, and does it really come back.
      --purity   at a landing error of exactly one phosphor the colours must
                 rotate cleanly, and at zero the pass must be the identity.

    And the renders:

        rgtest --list
        rgtest --out /tmp/frame.png --width 1280 --height 720
        rgtest --set "Layout=1" --set "Magnetisation=0.9" --frames 30
        rgtest --degauss 0.15 --frames 20      # capture mid-transient
        rgtest --flat 0.5 --measure            # what the mask costs in light
*/

#include "Regauss.h"

#include "Controls.h"
#include "Field.h"
#include "Masks.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test picture.
//
// Chosen so that a wrong answer is visible rather than so that it looks nice.
// Each band answers a different question about a magnet:
//
//   colour bars    purity error rotates hues -- red becomes green -- and a
//                  saturated bar makes a fraction of a phosphor obvious
//   white grid     geometry error bends the lines, and convergence error puts
//                  coloured fringes on every one of them
//   fine stripes   the mask and the beam profile, at a scale where a landing
//                  error of one phosphor is a full colour inversion
//   flat grey      the stain itself, with nothing else in the way
//
// A photograph would hide every one of those.
//---------------------------------------------------------------------------
void setPixel( std::vector< unsigned char >& image, int width, int height, int x, int y, float r, float g, float b )
{
	//y from the top, which is how the pattern below is described and how the
	//PNG will be read. GL puts row zero at the bottom, so the flip lives here
	//rather than somewhere in the middle of the chain.
	const size_t i = ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4;
	image[ i + 0 ] = static_cast< unsigned char >( std::lround( std::fmin( std::fmax( r, 0.0f ), 1.0f ) * 255.0f ) );
	image[ i + 1 ] = static_cast< unsigned char >( std::lround( std::fmin( std::fmax( g, 0.0f ), 1.0f ) * 255.0f ) );
	image[ i + 2 ] = static_cast< unsigned char >( std::lround( std::fmin( std::fmax( b, 0.0f ), 1.0f ) * 255.0f ) );
	image[ i + 3 ] = 255;
}

std::vector< unsigned char > buildFlatPicture( int width, int height, float level )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			setPixel( image, width, height, x, y, level, level, level );
	return image;
}

std::vector< unsigned char > buildTestPicture( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const float bars[ 7 ][ 3 ] = {
		{ 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0.0f }, { 0.0f, 0.75f, 0.75f }, { 0.0f, 0.75f, 0.0f },
		{ 0.75f, 0.0f, 0.75f }, { 0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.75f }
	};

	const int barsEnd  = height * 30 / 100;
	const int gridEnd  = height * 62 / 100;
	const int fineEnd  = height * 82 / 100;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = static_cast< float >( x ) / static_cast< float >( width );

			if( y < barsEnd )
			{
				const int bar  = static_cast< int >( u * 7.0f );
				const float* c = bars[ bar < 0 ? 0 : ( bar > 6 ? 6 : bar ) ];
				setPixel( image, width, height, x, y, c[ 0 ], c[ 1 ], c[ 2 ] );
			}
			else if( y < gridEnd )
			{
				//A white grid on black. Every line is a straight edge in both
				//axes, which is what makes a bend and a colour fringe legible
				//at the same time.
				const int step = std::max( 8, height / 20 );
				const bool on  = ( x % step == 0 ) || ( ( y - barsEnd ) % step == 0 );
				const float v  = on ? 1.0f : 0.02f;
				setPixel( image, width, height, x, y, v, v, v );
			}
			else if( y < fineEnd )
			{
				//Vertical stripes tightening left to right, down to a couple of
				//pixels. Where they approach the mask pitch a landing error
				//inverts them rather than merely shifting them.
				const float cycles = 8.0f + u * u * 220.0f;
				const float v = 0.5f + 0.48f * std::sin( u * cycles * 6.2831853f );
				setPixel( image, width, height, x, y, v, v, v );
			}
			else
			{
				//Flat mid grey: the stain, with nothing else on top of it.
				setPixel( image, width, height, x, y, 0.5f, 0.5f, 0.5f );
			}
		}
	}

	return image;
}

//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

//===========================================================================
// --field: the GLSL field against the C++ field.
//===========================================================================
int checkField()
{
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "rgtest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	ffglex::FFGLShader probe;
	if( !probe.Compile( regauss::shaders::kVertex, regauss::shaders::FieldProbeSource().c_str() ) )
	{
		std::fprintf( stderr, "rgtest: the field probe would not compile\n" );
		return 1;
	}

	ffglex::FFGLScreenQuad quad;
	if( !quad.Initialise() )
	{
		std::fprintf( stderr, "rgtest: quad geometry failed\n" );
		return 1;
	}

	//Float, not 8-bit. The field is signed and runs past one near a pole, so
	//a byte target would clamp away exactly the region worth checking.
	const int W = 96;
	const int H = 64;

	GLuint texture = 0, fbo = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, W, H, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "rgtest: float framebuffer is incomplete\n" );
		return 1;
	}

	const float aspect = static_cast< float >( W ) / static_cast< float >( H );

	//Every layout, at a seed and a drift that are not the defaults -- a
	//comparison that only ever runs at zero would miss a sign error in the
	//wander term.
	struct Case
	{
		int layout;
		float seed;
		float wander;
		float time;
	};
	const Case cases[] = {
		{ regauss::kSpeakerLeft, 0.0f, 0.0f, 0.0f },
		{ regauss::kCornerMagnet, 0.37f, 0.0f, 0.0f },
		{ regauss::kRingMagnet, 0.62f, 0.45f, 3.7f },
		{ regauss::kWandering, 0.11f, 0.80f, 11.3f },
		{ regauss::kEarthField, 0.94f, 0.20f, 2.2f },
	};

	std::vector< float > pixels( static_cast< size_t >( W ) * H * 4 );
	double worst = 0.0;
	int failures = 0;

	for( const Case& c : cases )
	{
		const regauss::PoleSet set = regauss::poles( c.layout, c.seed, c.wander, c.time, aspect );

		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, W, H );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );

		glUseProgram( probe.GetGLID() );
		probe.Set( "Pole0", set.p[ 0 ].x, set.p[ 0 ].y, set.p[ 0 ].strength );
		probe.Set( "Pole1", set.p[ 1 ].x, set.p[ 1 ].y, set.p[ 1 ].strength );
		probe.Set( "Pole2", set.p[ 2 ].x, set.p[ 2 ].y, set.p[ 2 ].strength );
		probe.Set( "Pole3", set.p[ 3 ].x, set.p[ 3 ].y, set.p[ 3 ].strength );
		probe.Set( "PoleHeight", regauss::kPoleHeight );
		probe.Set( "Aspect", aspect );
		quad.Draw();
		glUseProgram( 0 );

		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, W, H, GL_RGBA, GL_FLOAT, pixels.data() );

		double caseWorst = 0.0;
		for( int y = 0; y < H; ++y )
		{
			for( int x = 0; x < W; ++x )
			{
				const float u  = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( W );
				const float v  = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( H );
				const float px = ( u * 2.0f - 1.0f ) * aspect;
				const float py = v * 2.0f - 1.0f;

				float bx = 0.0f, by = 0.0f;
				regauss::fieldAt( set, px, py, bx, by );

				const size_t i = ( static_cast< size_t >( y ) * W + x ) * 4;
				const double dx = std::fabs( pixels[ i + 0 ] - bx );
				const double dy = std::fabs( pixels[ i + 1 ] - by );

				//Relative to the local magnitude: near a pole the field is a
				//few units and an absolute tolerance would be meaningless,
				//far from one it is tiny and an absolute tolerance would pass
				//anything.
				const double scale = std::max( 1.0, std::sqrt( double( bx ) * bx + double( by ) * by ) );
				caseWorst = std::max( caseWorst, std::max( dx, dy ) / scale );
			}
		}

		const char* verdict = caseWorst < 1e-4 ? "ok" : "*** MISMATCH ***";
		if( caseWorst >= 1e-4 )
			++failures;
		std::printf( "  layout %d seed %.2f wander %.2f   worst relative error %.3e   %s\n",
		             c.layout, c.seed, c.wander, caseWorst, verdict );
		worst = std::max( worst, caseWorst );
	}

	std::printf( "\nGLSL fieldAt vs Field.cpp: worst %.3e over %d layouts\n",
	             worst, int( sizeof( cases ) / sizeof( cases[ 0 ] ) ) );

	glDeleteFramebuffers( 1, &fbo );
	glDeleteTextures( 1, &texture );
	quad.Release();
	probe.FreeGLResources();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --coil: the degauss envelope. Pure C++, no GL needed.
//===========================================================================
int checkCoil()
{
	int failures = 0;
	auto fail = [ & ]( const char* what ) {
		std::printf( "  \033[31mFAILED: %s\033[0m\n", what );
		++failures;
	};

	const float duration  = 1.5f;
	const float intensity = 2.0f;

	//Idle before it fires.
	const regauss::Coil idle = regauss::coil( -1.0f, duration, intensity );
	std::printf( "  idle:            ac=%.4f retained=%.4f sag=%.4f\n", idle.ac, idle.retained, idle.sag );
	if( idle.ac != 0.0f )
		fail( "an idle coil is applying a field" );
	if( std::fabs( idle.retained - 1.0f ) > 1e-6f )
		fail( "an idle coil has already demagnetised the mask" );

	//At the moment of firing.
	const regauss::Coil start = regauss::coil( 0.0f, duration, intensity );
	std::printf( "  t=0:             ac=%.4f retained=%.4f sag=%.4f\n", start.ac, start.retained, start.sag );
	if( std::fabs( start.ac - intensity ) > 1e-4f )
		fail( "the coil does not reach its stated peak" );
	if( std::fabs( start.retained - 1.0f ) > 1e-4f )
		fail( "the mask is demagnetised before the coil has done anything" );

	//At the stated duration the field must be down to one per cent -- that is
	//what the control's label promises, and a label that lies is worse than no
	//label.
	const regauss::Coil done = regauss::coil( duration, duration, intensity );
	std::printf( "  t=duration:      ac=%.4f retained=%.4f sag=%.4f\n", done.ac, done.retained, done.sag );
	if( std::fabs( done.ac / intensity - 0.01f ) > 0.002f )
		fail( "the field is not down to one per cent after `duration` seconds" );
	if( done.retained > 0.02f )
		fail( "the mask is still magnetised when the coil has finished" );

	//Monotone all the way down. A field that came back up would re-magnetise
	//the mask, which is the one thing a degausser must never do.
	float previous = 1e9f;
	for( int i = 0; i <= 200; ++i )
	{
		const float t = duration * 2.0f * static_cast< float >( i ) / 200.0f;
		const regauss::Coil c = regauss::coil( t, duration, intensity );
		if( c.ac > previous + 1e-6f )
		{
			fail( "the coil's envelope is not monotonically decreasing" );
			break;
		}
		previous = c.ac;
	}

	//The HT has to recover faster than the field dies, or the picture stays
	//dim long after it has stopped moving.
	const regauss::Coil mid = regauss::coil( duration * 0.5f, duration, intensity );
	if( mid.sag / intensity >= mid.ac / intensity )
		fail( "the HT sag outlasts the field it is caused by" );
	std::printf( "  t=duration/2:    ac=%.4f sag=%.4f  (sag must recover first)\n", mid.ac, mid.sag );

	//---------------------------------------------------------------------
	// And the whole loop, through drive(): magnetised, degaussed, recovered.
	//---------------------------------------------------------------------
	regauss::controls::Settings s;
	s.magnetisation = 1.0f;
	s.duration      = 0.62f;//about 1.5 s
	s.intensity     = 0.7f;
	s.recovery      = 0.65f;//about 8 s
	s.coilSag       = 1.0f; //or the sag and swell columns below say nothing
	s.maskPitch     = 0.30f;
	s.maskPattern   = 1;
	s.purity        = 0.5f;
	s.deflection    = 0.5f;

	const regauss::MaskSpec& m = regauss::mask( 1 );

	const float trigger = 10.0f;
	const struct { float t; const char* label; } samples[] = {
		{ 9.0f, "before" }, { 10.0f, "fired" }, { 11.5f, "settling" },
		{ 12.0f, "clean" }, { 18.0f, "creeping back" }, { 60.0f, "long after" }
	};

	std::printf( "\n  the loop (Magnetisation 1.0, degauss at t=10):\n" );
	float atLong = 0.0f;
	for( const auto& sample : samples )
	{
		const regauss::controls::Drive d =
			regauss::controls::drive( s, m, sample.t, trigger, 1920.0f, 1080.0f );
		std::printf( "    t=%5.1f  %-14s staticAmp=%.4f acAmp=%.4f sag=%.4f swell=%.4f\n",
		             sample.t, sample.label, d.staticAmp, d.acAmp, d.sag, d.swell );

		if( std::string( sample.label ) == "long after" )
			atLong = d.staticAmp;
	}

	//How clean it actually gets.
	//
	// Sampling at a couple of chosen instants is not good enough here: the
	// curve is a falling exponential crossing a rising one, so the interesting
	// number is the MINIMUM, and where that minimum falls moves whenever
	// either time constant is touched. A fixed sample would keep passing while
	// the trough drifted away from it.
	float trough    = 1e9f;
	float troughAt  = 0.0f;
	for( int i = 0; i <= 600; ++i )
	{
		const float t = trigger + 6.0f * static_cast< float >( i ) / 600.0f;
		const regauss::controls::Drive d = regauss::controls::drive( s, m, t, trigger, 1920.0f, 1080.0f );
		if( d.staticAmp < trough )
		{
			trough   = d.staticAmp;
			troughAt = t - trigger;
		}
	}
	std::printf( "    cleanest: %.4f, %.2f s after the button\n", trough, troughAt );

	//The point of the button: the coil really does clear what the mask was
	//holding, rather than merely shaking the picture for a second.
	if( trough > 0.25f )
		fail( "the mask is not appreciably cleaner after a degauss" );
	//And the point of Recovery: it does come back, so the Magnetisation slider
	//is not dead for the rest of the session.
	if( atLong < 0.95f )
		fail( "the magnetisation never recovers, leaving the slider dead" );

	std::printf( "\n" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --purity: the landing error against the colour it produces.
//
// Reimplements the mixing the beam pass does, from the same rule, and asserts
// the two properties the whole model rests on: identity at rest, and a clean
// rotation at one whole phosphor.
//===========================================================================
int checkPurity()
{
	int failures = 0;

	auto mixed = []( float in[ 3 ], float landing ) {
		float out[ 3 ] = { 0.0f, 0.0f, 0.0f };
		for( int c = 0; c < 3; ++c )
		{
			for( int k = 0; k < 3; ++k )
			{
				float d = static_cast< float >( c ) + landing - static_cast< float >( k );
				d -= 3.0f * std::round( d / 3.0f );
				out[ k ] += in[ c ] * std::max( 0.0f, 1.0f - std::fabs( d ) );
			}
		}
		return std::array< float, 3 > { out[ 0 ], out[ 1 ], out[ 2 ] };
	};

	float red[ 3 ] = { 1.0f, 0.0f, 0.0f };
	float mix[ 3 ] = { 0.7f, 0.35f, 0.1f };

	const auto atRest = mixed( mix, 0.0f );
	std::printf( "  landing 0.0  (%.2f %.2f %.2f) -> (%.4f %.4f %.4f)   must be identity\n",
	             mix[ 0 ], mix[ 1 ], mix[ 2 ], atRest[ 0 ], atRest[ 1 ], atRest[ 2 ] );
	for( int k = 0; k < 3; ++k )
	{
		if( std::fabs( atRest[ k ] - mix[ k ] ) > 1e-6f )
		{
			std::printf( "  \033[31mFAILED: the purity pass is not the identity at zero field\033[0m\n" );
			++failures;
			break;
		}
	}

	//One whole phosphor: red's gun lands squarely on the green stripe. This is
	//the fault everybody recognises -- a magnetised corner goes green rather
	//than merely dim.
	const auto rotated = mixed( red, 1.0f );
	std::printf( "  landing 1.0  (1.00 0.00 0.00) -> (%.4f %.4f %.4f)   red must become green\n",
	             rotated[ 0 ], rotated[ 1 ], rotated[ 2 ] );
	if( rotated[ 1 ] < 0.999f || rotated[ 0 ] > 1e-3f )
	{
		std::printf( "  \033[31mFAILED: a one-phosphor error does not rotate the colours cleanly\033[0m\n" );
		++failures;
	}

	//Three phosphors is a whole triad: back where it started.
	const auto wrapped = mixed( mix, 3.0f );
	std::printf( "  landing 3.0  (%.2f %.2f %.2f) -> (%.4f %.4f %.4f)   a whole triad wraps\n",
	             mix[ 0 ], mix[ 1 ], mix[ 2 ], wrapped[ 0 ], wrapped[ 1 ], wrapped[ 2 ] );
	for( int k = 0; k < 3; ++k )
	{
		if( std::fabs( wrapped[ k ] - mix[ k ] ) > 1e-5f )
		{
			std::printf( "  \033[31mFAILED: a full triad of error does not wrap back to the identity\033[0m\n" );
			++failures;
			break;
		}
	}

	//Energy is conserved: the tent weights for one gun sum to one at every
	//offset, so the mask cannot invent or lose light.
	for( int i = 0; i <= 60; ++i )
	{
		const float landing = -3.0f + 6.0f * static_cast< float >( i ) / 60.0f;
		float ones[ 3 ] = { 1.0f, 1.0f, 1.0f };
		const auto out  = mixed( ones, landing );
		const float sum = out[ 0 ] + out[ 1 ] + out[ 2 ];
		if( std::fabs( sum - 3.0f ) > 1e-4f )
		{
			std::printf( "  \033[31mFAILED: light is not conserved at landing %.3f (sum %.5f)\033[0m\n", landing, sum );
			++failures;
			break;
		}
	}
	std::printf( "  light is conserved across landing errors of -3 to +3 phosphors\n" );

	//A mask with no structure cannot have a purity error at all.
	regauss::controls::Settings s;
	s.purity      = 1.0f;
	s.maskPitch   = 0.3f;
	s.maskPattern = 0;//None
	const regauss::controls::Drive none =
		regauss::controls::drive( s, regauss::mask( 0 ), 0.0f, -1.0f, 1920.0f, 1080.0f );
	if( none.purityGainX != 0.0f || none.purityGainY != 0.0f )
	{
		std::printf( "  \033[31mFAILED: a monochrome tube has a purity error\033[0m\n" );
		++failures;
	}
	else
	{
		std::printf( "  a tube with no mask has no purity error, whatever Purity says\n" );
	}

	//An aperture grille has no vertical structure, so a vertical error costs
	//nothing. This is the Trinitron's real advantage and it must fall out of
	//the table rather than being asserted in the shader.
	s.maskPattern = 2;
	const regauss::controls::Drive grille =
		regauss::controls::drive( s, regauss::mask( 2 ), 0.0f, -1.0f, 1920.0f, 1080.0f );
	s.maskPattern = 1;
	const regauss::controls::Drive shadow =
		regauss::controls::drive( s, regauss::mask( 1 ), 0.0f, -1.0f, 1920.0f, 1080.0f );

	std::printf( "  grille purityGainY=%.4f, shadow mask purityGainY=%.4f\n",
	             grille.purityGainY, shadow.purityGainY );
	if( grille.purityGainY != 0.0f || shadow.purityGainY <= 0.0f )
	{
		std::printf( "  \033[31mFAILED: vertical purity error does not follow the mask's stagger\033[0m\n" );
		++failures;
	}

	std::printf( "\n" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Parameter automation for --pipe.
//
// A plain text file of `frame  Parameter Name  value` lines. Values are held
// before the first key and after the last, and linearly interpolated between.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

bool readExactly( void* into, size_t bytes )
{
	unsigned char* p = static_cast< unsigned char* >( into );
	size_t got = 0;
	while( got < bytes )
	{
		const size_t n = fread( p + got, 1, bytes - got, stdin );
		if( n == 0 )
			return false;//clean EOF, or a short final frame we cannot use
		got += n;
	}
	return true;
}

void usage()
{
	std::printf(
		"rgtest -- render (re)gauss to a PNG, or check its maths\n"
		"\n"
		"  --out PATH        where to write (default /tmp/regauss.png)\n"
		"  --width N         output width (default 1280)\n"
		"  --height N        output height (default 720)\n"
		"  --frames N        frames to run before capturing (default 8)\n"
		"  --fps N           frame rate the clock advances at (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1\n"
		"  --degauss T       press the Degauss button T seconds into the run\n"
		"  --alpha           keep the alpha channel instead of compositing on black\n"
		"  --flat V          render a uniform field at level V instead of the test card\n"
		"  --measure         print the mean RGB of the middle of the picture\n"
		"  --list            print every parameter and its default, then exit\n"
		"\n"
		"  --field           GLSL fieldAt() against Field.cpp (needs a GPU)\n"
		"  --coil            the degauss envelope and the magnetise/clear/recover loop\n"
		"  --purity          the landing-error to colour mapping\n"
		"\n"
		"  --pipe            read raw RGBA frames from stdin, write them to stdout:\n"
		"                      ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \\\n"
		"                        | rgtest --pipe --width 1920 --height 1080 \\\n"
		"                        | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov\n"
		"  --script PATH     parameter automation for --pipe\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/regauss.png";
	int width = 1280;
	int height = 720;
	int frames = 8;
	bool keepAlpha = false;
	bool listOnly = false;
	bool measure = false;
	bool pipeMode = false;
	float flatLevel = -1.0f;
	float degaussAt = -1.0f;
	std::string scriptPath;
	double fps = 60.0;
	std::vector< std::pair< std::string, float > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--field" )
			return checkField();
		else if( arg == "--coil" )
			return checkCoil();
		else if( arg == "--purity" )
			return checkPurity();
		else if( arg == "--out" )
			outputPath = next();
		else if( arg == "--width" )
			width = std::atoi( next().c_str() );
		else if( arg == "--height" )
			height = std::atoi( next().c_str() );
		else if( arg == "--frames" )
			frames = std::atoi( next().c_str() );
		else if( arg == "--fps" )
			fps = std::strtod( next().c_str(), nullptr );
		else if( arg == "--degauss" )
			degaussAt = std::strtof( next().c_str(), nullptr );
		else if( arg == "--alpha" )
			keepAlpha = true;
		else if( arg == "--measure" )
			measure = true;
		else if( arg == "--pipe" )
			pipeMode = true;
		else if( arg == "--script" )
			scriptPath = next();
		else if( arg == "--flat" )
			flatLevel = std::strtof( next().c_str(), nullptr );
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "rgtest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "rgtest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "rgtest: width, height, frames and fps must all be positive\n" );
		return 2;
	}

	Regauss plugin;

	//The names come from the plugin's own declaration rather than from a table
	//here, so a parameter that is renamed or reordered cannot leave the harness
	//quietly setting the wrong one.
	auto indexOfParameter = [ & ]( const std::string& name ) -> int {
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
				return static_cast< int >( i );
		}
		return -1;
	};

	if( listOnly )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			std::printf( "%2u  %-20s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
		return 0;
	}

	for( const auto& override : overrides )
	{
		const int index = indexOfParameter( override.first );
		if( index < 0 )
		{
			std::fprintf( stderr, "rgtest: no parameter named '%s' (try --list)\n", override.first.c_str() );
			return 2;
		}
		plugin.SetFloatParameter( static_cast< unsigned int >( index ), override.second );
	}

	const int degaussIndex = indexOfParameter( "Degauss" );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "rgtest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	//In pipe mode stdout carries the video, so everything conversational has to
	//go to stderr or it ends up inside a frame.
	std::fprintf( pipeMode ? stderr : stdout, "GL %s / %s\n",
	              glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	const std::vector< unsigned char > picture = flatLevel >= 0.0f
	                                                 ? buildFlatPicture( width, height, flatLevel )
	                                                 : buildTestPicture( width, height );
	GLuint inputTexture = 0;
	glGenTextures( 1, &inputTexture );
	glBindTexture( GL_TEXTURE_2D, inputTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, picture.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//Somewhere to put the result: this is the host's framebuffer as far as the
	//plugin is concerned.
	GLuint outputTexture = 0;
	GLuint outputFBO = 0;
	glGenTextures( 1, &outputTexture );
	glBindTexture( GL_TEXTURE_2D, outputTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &outputFBO );
	glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "rgtest: output framebuffer is incomplete\n" );
		return 1;
	}

	FFGLViewportStruct viewport = { 0, 0, static_cast< FFUInt32 >( width ), static_cast< FFUInt32 >( height ) };
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "rgtest: InitGL failed -- see the diagnostics log for which shader\n" );
		return 1;
	}

	//A synthetic transport, so the Beat and Bar modes have a grid to land on.
	//Without it they read as dead: the host is the only thing that ever
	//supplies a tempo, and an unset one is not the same as 120.
	plugin.SetBeatInfo( 120.0f, 0.0f );

	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle = inputTexture;
	FFGLTextureStruct* inputs[ 1 ] = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures = 1;
	process.inputTextures = inputs;
	process.HostFBO = outputFBO;

	//The transport, advanced with the clock so a Bar-synced coil fires where
	//it should. Four beats to the bar at 120 bpm is two seconds.
	auto driveTransport = [ & ]( double seconds ) {
		const double bars = seconds / 2.0;
		plugin.SetBeatInfo( 120.0f, static_cast< float >( bars - std::floor( bars ) ) );
	};

	if( pipeMode )
	{
		std::map< std::string, Track > tracks;
		if( !scriptPath.empty() )
		{
			std::string error;
			tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "rgtest: %s\n", error.c_str() );
				return 2;
			}
			//Fail on a name that does not exist rather than silently animating
			//nothing for forty seconds.
			for( const auto& entry : tracks )
			{
				if( indexOfParameter( entry.first ) < 0 )
				{
					std::fprintf( stderr, "rgtest: script names '%s', which is not a parameter (try --list)\n",
					              entry.first.c_str() );
					return 2;
				}
			}
		}

		const size_t frameBytes = static_cast< size_t >( width ) * height * 4;
		std::vector< unsigned char > in( frameBytes );
		std::vector< unsigned char > flip( frameBytes );
		std::vector< unsigned char > out( frameBytes );
		const size_t rowBytes = static_cast< size_t >( width ) * 4;

		long frame = 0;
		while( readExactly( in.data(), frameBytes ) )
		{
			//ffmpeg hands over top-down rows; GL wants the bottom row first.
			for( int y = 0; y < height; ++y )
				std::memcpy( flip.data() + static_cast< size_t >( y ) * rowBytes,
				             in.data() + static_cast< size_t >( height - 1 - y ) * rowBytes, rowBytes );

			glBindTexture( GL_TEXTURE_2D, inputTexture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flip.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );

			for( const auto& entry : tracks )
				plugin.SetFloatParameter( static_cast< unsigned int >( indexOfParameter( entry.first ) ),
				                          valueAt( entry.second, static_cast< int >( frame ) ) );

			const double seconds = static_cast< double >( frame ) / fps;
			driveTransport( seconds );
			plugin.SetTime( seconds );

			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "rgtest: ProcessOpenGL failed on frame %ld\n", frame );
				return 1;
			}

			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flip.data() );
			for( int y = 0; y < height; ++y )
				std::memcpy( out.data() + static_cast< size_t >( y ) * rowBytes,
				             flip.data() + static_cast< size_t >( height - 1 - y ) * rowBytes, rowBytes );

			if( fwrite( out.data(), 1, frameBytes, stdout ) != frameBytes )
			{
				std::fprintf( stderr, "rgtest: short write on frame %ld\n", frame );
				return 1;
			}
			++frame;
		}

		fflush( stdout );
		std::fprintf( stderr, "rgtest: %ld frames through the chain\n", frame );
		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return 0;
	}

	//Run several frames rather than one. Persistence reads its own history,
	//the poles drift, the coil decays, and the scan period is measured from
	//the frame delta -- a single frame would be testing none of that. Time
	//comes from the counter, not the clock, so two runs produce identical
	//pixels.
	bool pressed = false;
	for( int frame = 0; frame < frames; ++frame )
	{
		const double seconds = static_cast< double >( frame ) / fps;

		if( degaussAt >= 0.0f && !pressed && seconds >= static_cast< double >( degaussAt ) && degaussIndex >= 0 )
		{
			//Press and release, exactly as a host would draw a button: the
			//plugin fires on the rising edge only.
			plugin.SetFloatParameter( static_cast< unsigned int >( degaussIndex ), 1.0f );
			plugin.SetFloatParameter( static_cast< unsigned int >( degaussIndex ), 0.0f );
			pressed = true;
		}

		driveTransport( seconds );
		plugin.SetTime( seconds );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "rgtest: ProcessOpenGL failed on frame %d\n", frame );
			return 1;
		}
	}

	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

	const GLenum error = glGetError();
	if( error != GL_NO_ERROR )
		std::fprintf( stderr, "rgtest: GL error 0x%04x during render\n", error );

	//GL hands back bottom-up; PNG wants top-down.
	std::vector< unsigned char > flipped( pixels.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             pixels.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );

	//The plugin outputs premultiplied alpha, so the colour is already the
	//over-black composite. Flattening is just a matter of forcing alpha opaque.
	if( !keepAlpha )
	{
		for( size_t i = 3; i < flipped.size(); i += 4 )
			flipped[ i ] = 255;
	}

	if( measure )
	{
		//The middle half only. The edges carry the vignette, the curvature crop
		//and the bezel, none of which say anything about what the mask cost.
		double sum[ 3 ] = { 0.0, 0.0, 0.0 };
		size_t counted = 0;
		for( int y = height / 4; y < height * 3 / 4; ++y )
		{
			for( int x = width / 4; x < width * 3 / 4; ++x )
			{
				const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
				sum[ 0 ] += flipped[ i + 0 ];
				sum[ 1 ] += flipped[ i + 1 ];
				sum[ 2 ] += flipped[ i + 2 ];
				++counted;
			}
		}
		const double n = static_cast< double >( counted ) * 255.0;
		std::printf( "mean RGB %.4f %.4f %.4f\n", sum[ 0 ] / n, sum[ 1 ] / n, sum[ 2 ] / n );
	}

	if( !writePng( outputPath, width, height, flipped ) )
	{
		std::fprintf( stderr, "rgtest: could not write %s\n", outputPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outputPath.c_str(), width, height, frames );

	plugin.DeInitGL();
	glDeleteFramebuffers( 1, &outputFBO );
	glDeleteTextures( 1, &outputTexture );
	glDeleteTextures( 1, &inputTexture );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
