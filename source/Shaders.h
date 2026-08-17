#pragma once

/**
    The GLSL for each pass.

    (re)gauss is a tube with a magnet near it. The chain is short because the
    interesting part is not a stack of stages, it is that four passes all read
    the same vector field:

      Beam      where the electron beam actually lands, given the field. This
                is the whole effect: the displacement is the geometry error,
                the difference between the three guns' displacements is the
                convergence error, and the displacement measured in shadow-mask
                pitches is the purity error. One field evaluation per gun.
      Phosphor  emissive decay. It sits AFTER the beam pass because the
                phosphor is on the glass: a picture that is being shaken by a
                magnet smears, and it smears where the light landed rather than
                where it was meant to.
      Bloom     halation, at quarter size.
      Screen    the glass -- shadow mask, scanning beam, curvature, and where
                the viewer is sitting. Skipped wholesale in Interference Only.

    Unlike old-cathode, every pass runs at the composition's own resolution.
    There is no authentic raster to snap to: a magnetic field is a fact about
    the room, not about the television system, and the purity error's scale is
    set by the mask pitch, which is already in output pixels.

    ------------------------------------------------------------------- MaxUV

    The vertex shader passes texture coordinates through UNSCALED, which is the
    opposite of what the other FFGL plugins in the fleet do. Those are filters:
    they sample where they were told, so folding `MaxUV` into the varying is
    safe and saves a multiply. This one is a warp -- the beam pass samples
    wherever the field sends it, including outside the picture -- so the
    coordinate has to be clamped in picture space FIRST and scaled by MaxUV
    only at the fetch. Fold it into the varying and the clamp is applied to an
    already-scaled coordinate, which lets the fetch walk off into the host
    texture's undrawn padding at exactly the moments the effect is most
    visible.
*/
#include <string>

namespace regauss::shaders
{
/// Draws the screen quad. `uv` is 0..1 across the picture, NOT scaled by
/// MaxUV -- see the note above.
extern const char* const kVertex;

/// The field, as GLSL. Shared verbatim between the beam pass and the test
/// probe, which is what makes `rgtest --field` a check on the real thing
/// rather than on a copy of it. Its C++ twin is `regauss::fieldAt()`.
extern const char* const kFieldUniforms;
extern const char* const kFieldFunction;

/// The beam pass, assembled around the two strings above.
std::string BeamFragmentSource();

/// A program that renders the field itself into a float buffer, for the
/// harness to compare against Field.cpp.
std::string FieldProbeSource();

extern const char* const kPhosphorFragment;
extern const char* const kBloomFragment;
extern const char* const kBlurFragment;
extern const char* const kScreenFragment;

} // namespace regauss::shaders
