#pragma once

namespace regauss
{
/**
    A shadow mask, as the four facts the rest of the plugin needs about it.

    Two of them are optical and two of them are mechanical, and it is the
    mechanical pair that make this a table rather than a switch in the shader:
    the mask decides how a landing error turns into a colour error, and it
    decides it differently for each pattern.
*/
struct MaskSpec
{
	const char* name;

	/// How much light a phosphor's neighbours still receive. The beam spot is
	/// wider than one stripe, so this is never zero on a real tube, and it is
	/// what makes a mask read as a texture over the picture rather than as
	/// three separated primaries.
	float spill;

	/// What the mask costs in light, inverted. MEASURED, never derived --
	/// `rgtest --flat 0.05 --measure` renders a uniform field through the real
	/// shader and reports the mean. Edge shaping and anti-aliasing both eat
	/// into the duty cycle, so the analytic answer is always wrong by a few
	/// per cent. Re-measure after changing any mask shape.
	float gain;

	/// Whether a VERTICAL landing error also shifts which phosphor the beam
	/// hits. True of any mask whose triads are staggered row to row -- a delta
	/// mask and a slot mask -- and false of an aperture grille, whose stripes
	/// are continuous from top to bottom. This is not a detail: it is the
	/// reason a Trinitron shrugs off a vertical purity error that would stain
	/// a shadow-mask set, and it comes out of the model rather than being
	/// asserted anywhere.
	bool staggers;

	/// Whether the tube was built with a delta gun (three cathodes on a
	/// triangle) rather than an in-line gun (three in a row). It follows the
	/// mask because the tube was built as one thing, and it decides whether
	/// convergence error appears as a red/blue horizontal split or as a
	/// three-way spray.
	bool triangularGun;
};

int maskCount();
const MaskSpec& mask( int index );

} // namespace regauss
