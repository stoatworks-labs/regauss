#include "Masks.h"

#include <algorithm>

namespace regauss
{
namespace
{
/// The gains are measured, not computed -- see MaskSpec::gain. The shapes here
/// are the same four old-cathode draws, so these started as its figures and
/// were re-measured against this plugin's own screen pass, which scans at a
/// line count the operator chooses rather than at a fixed SD raster.
///
/// The procedure, and the trap in it:
///
///     rgtest --flat 0.30 --measure --width 1280 --height 960 \
///       --set "Mask Strength=1.0" --set "Mask Pattern=N" \
///       --set Magnetisation=0 --set Interference=0 --set Deflection=0 \
///       --set Purity=0 --set Scanlines=0 --set Curvature=0 \
///       --set Vignette=0 --set "Corner Radius=0" --set Halation=0 \
///       --set Persistence=0
///
/// Everything that touches level is switched off, Pattern 0 gives the
/// reference, and each gain is scaled by reference/measured until the two
/// agree. All four land within 0.4% as they stand.
///
/// **Measure at 0.30, not at 0.05.** The obvious choice is a low level, to be
/// safe from clipping -- old-cathode measures at 0.05 for exactly that reason.
/// It does not work here. At 0.05 the spill between phosphors comes out around
/// four parts in 255, the readback is 8-bit, and the rounding is a larger
/// error than the thing being measured: a 2.4% change in gain moved the
/// measured mean by 5.3%, in the wrong direction. 0.30 keeps the brightest
/// phosphor near 0.70 -- clear of clipping with the largest of these gains --
/// and the figures then converge in one pass.
///
/// "None" is a real choice and not an off switch: with no mask there is no
/// phosphor structure for a landing error to fall between, so Purity stops
/// doing anything and the field shows up purely as geometry and convergence.
/// That is the honest behaviour of a monochrome tube, which has no mask and
/// cannot have a purity fault.
const MaskSpec kMasks[] = {
	//                     spill  gain   staggers  delta gun
	{ "None",              0.00f, 1.000f, false,   false },
	{ "Shadow Mask",       0.35f, 2.235f, true,    true  },
	{ "Aperture Grille",   0.28f, 2.171f, false,   false },
	{ "Slot Mask",         0.32f, 2.099f, true,    false },
	{ "RGB Stripe",        0.15f, 2.314f, false,   false },
};
} // namespace

int maskCount()
{
	return static_cast< int >( sizeof( kMasks ) / sizeof( kMasks[ 0 ] ) );
}

const MaskSpec& mask( int index )
{
	return kMasks[ std::clamp( index, 0, maskCount() - 1 ) ];
}

} // namespace regauss
