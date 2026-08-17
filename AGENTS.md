# (re)gauss — for another LLM, or a newcomer

Read this before changing the field model, the two sensitivities, or either
shader pass. `CLAUDE.md` is the short command reference; this file is the *why*.

## The one idea

**Everything on screen comes from one vector field, read three times.**

A colour CRT steers three electron beams with a magnetic field. Put another
magnetic field near it and the beams go somewhere else. That is the whole
plugin. Three symptoms fall out of it, and **not one of them is drawn**:

| Symptom | What causes it | Which control scales it |
| --- | --- | --- |
| The picture leans and bulges | the field's **strength** | Deflection |
| Coloured fringes on every edge | the field's **gradient** | Convergence (gun separation) |
| Whole regions the wrong colour | the field measured in **mask pitches** | Purity |

If a symptom needs its own control to look right, the field is wrong somewhere.
Adding a "colour fringe" slider would be exactly the mistake that adding a "dot
crawl" slider would be in old-cathode.

Convergence being a *gradient* effect is load-bearing and not obvious: the three
cathodes are in different places, so the three beams fly different paths and see
different fields. A **uniform** field displaces all three equally — geometry
error with no fringing at all, which is precisely what the Earth's field does to
a set that has been turned to face a different wall. A **steep** field (a fridge
magnet on the glass) fringes savagely. That is why the pole layouts are described
as near-pairs and far-pairs rather than as eight coordinates.

## Load-bearing invariants

**The two sensitivities are separate on purpose.** `kDeflectScale` and
`kPurityScale` in `Controls.h` are not one constant with a ratio. On a real tube,
how far a field bends the beam is set by the yoke and the anode voltage; how much
colour error that bend causes is set by the mask pitch and the gun-to-mask
distance. Different hardware, and their real ratio puts nearly everything
interesting at one end: a magnet too weak to move the geometry visibly will still
rotate the colours completely. That is the commonest real purity fault there is,
and a plugin that derived one from the other could not reach it.

**The field's exponent is one, not three halves.** `fieldAt()` divides by `r²`,
not `r³`. A point magnet's field really does fall as the cube, but what bends the
beam is the field **integrated along the whole path** from gun to glass, and a
line integral through a point source comes out a full power gentler. This is not
a fudge for looks — it is the difference between a stain covering a third of the
screen, which is what a speaker beside a television actually does, and a bright
sliver at the very edge, which is what the un-integrated law gives and which was
the first thing this plugin rendered.

**Pole positions are in half-widths, and `poles()` scales x by the aspect.**
Without that step every layout is calibrated for a square screen, and on 16:9 a
magnet meant to sit beside the set lands two thirds of the way into the picture.
Seed jitter and Wander drift are **not** scaled — they are physical movements of
a real object, and a real object does not travel further sideways on a wider set.

**The purity tent is exactly one phosphor wide.** That makes the mixing matrix
the identity at zero field. Any wider and the plugin desaturates the picture
while claiming to do nothing — the sort of thing nobody notices until it is in a
show. `rgtest --purity` asserts it, along with light conservation and the clean
rotation at one whole phosphor.

**A vertical landing error only costs colour on a mask whose triads stagger.**
`MaskSpec::staggers` carries it. An aperture grille's stripes run the full height
of the tube, so a vertical error slides the beam along its own stripe and costs
nothing — a genuine advantage of the design, and it falls out of the table rather
than being asserted in a shader.

**Audio drives the field, and is added AFTER the coil's envelope.** The default
layout is a speaker, and a speaker's stray field IS the audio signal — the voice
coil current is the music and the magnet assembly leaks it. So audio is not a
separate effect with its own path to the picture; it is another term in the same
field, and the lean, the fringing and the stain all follow from it unchanged.
Adding it inside the coil's envelope would be the mistake: degaussing clears
what the mask has *stored*, and does nothing about the speaker still playing. A
degauss during a loud passage must clean the mask and let the stain come
straight back, because that is what would happen in the room.

**Phosphor decay sits AFTER the beam pass.** The phosphor is a coating on the
glass: it glows where the beam *hit*, not where it was *aimed*. Move persistence
before the beam pass and a violent degauss reads as a still frame with a wobble
applied, instead of the picture being physically thrown about.

**`ScanPeriod` is measured, not assumed.** It decides whether mains interference
stands still or rolls: a 50 Hz field on a set scanning at 50 Hz puts every line at
the same phase and the bend does not move. Assume 60 and that behaviour is simply
wrong at every other rate.

**The vertex shader does NOT fold `MaxUV` into the varying.** Every other FFGL
plugin in the fleet does, and they are all filters — they sample where they are
told. This one warps, so the coordinate must be clamped in picture space *first*
and scaled by MaxUV only at the fetch. Fold it in and the fetch walks into the
host texture's undrawn padding at exactly the moments the effect is most visible.

**Everything is premultiplied.** Anything changing how much light leaves the
screen (mask, scan, brightness) scales colour alone; anything changing coverage
(bezel, the crop outside the raster) scales colour and alpha together. Contrast
pivots on `0.5 * alpha`, not on `0.5`, or every partly transparent pixel gets
tinted.

## Relationship to old-cathode

They are complementary and deliberately do not overlap.

- **old-cathode** is a *signal path*: what happened to the picture on its way to
  the set. It runs its stages at an authentic SD raster because the artefacts
  belong to a broadcast standard.
- **(re)gauss** is a *fact about the room*: a magnet near the glass. It runs at
  the composition's own resolution, because a magnetic field has nothing to do
  with a broadcast standard.

Stack them by putting (re)gauss in **Interference Only** mode after old-cathode.
Two shadow masks in a chain is a moiré generator and two sets of curvature is a
fisheye. Interference Only bypasses the whole screen pass and the halation; it
keeps Persistence, because a wobble that leaves no trail looks wrong and the
trail is a consequence of the wobble. Purity still works in that mode — it uses
the mask pitch as an *implied* scale rather than a drawn one, which is what makes
the stain the right size over somebody else's CRT.

## Where things live

| File | What it is |
| --- | --- |
| `source/Field.{h,cpp}` | the magnets and the coil. No GL, no state. The reference `fieldAt()`. |
| `source/Controls.{h,cpp}` | 0..1 host values → physical quantities. `drive()` is called by both builds. |
| `source/Masks.{h,cpp}` | the four masks, and the two mechanical facts each one implies. |
| `source/shaders/FieldGLSL.cpp` | the GLSL field, shared verbatim by the beam pass and the test probe. |
| `source/shaders/Beam.cpp` | where the beam lands. The plugin. |
| `source/shaders/Screen.cpp` | the glass. Bypassed in Interference Only. |
| `source/Regauss.{h,cpp}` | FFGL host glue, the clock, the trigger schedule, the FFT reader. |
| `source/ofx/RegaussOFX.cpp` | the CPU mirror of the two passes. |
| `tools/rgtest` | the offline harness and the three maths checks. |

## Traps

- **A GLSL uniform name that does not match the C++ is silently ignored.**
  `glGetUniformLocation` returns -1 and `glUniform(-1)` is a documented no-op, so
  a control can be stone dead while everything compiles, links and renders. Only
  `tools/sweep.py` catches it.
- **`FFGLShader::Set` has no integer-vector overload.** `Set(name, i, j)` resolves
  to the float overload and issues a `glUniform2f` against an `ivec2`, which is a
  `GL_INVALID_OPERATION` leaving the uniform at zero with nothing to see.
- **`layout` is a GLSL reserved word**, as are `flat`, `active`, `filter`,
  `input`, `output`, `sample`, `common`. A shader that fails to compile surfaces
  at runtime as "the effect does nothing" — and the beam pass is assembled from
  several strings, so a reported line number is in a file that does not exist.
  Check `~/Library/Logs/regauss/`.
- **`SetTextParameter` must return FF_SUCCESS for the About block.** The SDK's
  `instantiateGL` sets every parameter's default on a fresh instance and deletes
  the instance if any set fails; the base class's version is a stub returning
  FF_FAIL. Omit the override and **no real host can instantiate the plugin at
  all** — while every harness here stays happy, because they drive the class
  directly and never go through `plugMain`.
- **`ScopedFBOBinding` restores the framebuffer and not the viewport.** Capture
  the host viewport at the top of `ProcessOpenGL`.
- **Allocating an FBO unbinds your input texture.** Every `Ensure()` happens
  before any texture binding for that reason. The symptom is correct on every
  frame *except* the one that allocates.
- **Resolume sends `SetTime` in milliseconds.** The unit is decided from the
  first plausible frame delta; nothing consumes `hostTime` raw. Getting it wrong
  makes a one-second degauss last a millisecond.
- **Mask gains are measured at flat 0.30, not 0.05.** See `source/Masks.cpp`. At
  0.05 the 8-bit readback's rounding is larger than the quantity being measured,
  and a 2.4% gain change moved the measured mean by 5.3% in the wrong direction.
- **Never let `tools/sweep.py` touch the About block.** Those parameters are
  buttons that open a web browser.
- **The FFT buffer parameter must be skipped by the sweep.** Its single float
  value is meaningless — the content is 64 elements the host writes — so
  sweeping it reports a false dead every time.
- **A synthetic test spectrum must fall back between beats.** The first one here
  did not: its floor sat just above the trigger threshold, so the coil fired
  once for the whole run and `Trigger Coil` and `Threshold` produced
  byte-identical sweep diffs. That identity is the symptom to watch for — the
  controls both passed.

## What is genuinely verified, and what is assumed

**Verified, by `tools/verify.sh`:**

- The GLSL `fieldAt()` matches `Field.cpp` to under 1e-6 relative, across all
  five layouts including seeded and drifting ones. The probe is assembled from
  the *same strings* the beam pass uses, so this is not a check on a copy.
- The purity model: identity at zero field, a clean rotation at one whole
  phosphor, wrap at a full triad, light conserved from −3 to +3 phosphors, no
  purity error on a maskless tube, and no *vertical* purity error on a grille.
- The coil: peak at the moment of firing, down to one per cent after exactly the
  stated Duration, monotonically decreasing throughout, HT recovering faster than
  the field, and the full loop — magnetised 1.0, down to 0.09 at 0.77 s after the
  button, back to 1.0 over the Recovery time.
- All 40 parameters change the picture.
- The audio path, end to end against `rgtest --audio`'s synthetic spectrum: the
  field pumps with the injected bass, the four bands read different levels, and
  the coil re-arms and fires once per kick rather than once per session.
- The four mask gains are within 0.4% of the unmasked reference.
- The bundle exports `plugMain` and carries `RG01`; the OFX bundle's plist names
  its real binary and ad-hoc signs.

**Assumed, and not yet checked:**

- **It has never been loaded into Resolume or Resolve.** Everything above is
  offline. The OpenFX bundle has been loaded and rendered through by `ofxprobe`,
  which is not Resolve.
- Whether Resolume draws `FF_TYPE_EVENT` as a button the way the Degauss control
  assumes, and whether a MIDI or OSC mapping to it produces one rising edge per
  press rather than a stream.
- Whether Resolume's FFT arrives in the shape the reader assumes. The audio path
  has only ever seen `rgtest`'s synthetic spectrum: the bin count, the
  normalisation and whether the magnitudes need the sqrt are all taken from the
  fleet's other plugins rather than measured here.
- Whether Beat and Bar lock to a real Resolume transport. The recovery of the
  bar line from `barPhase` is the same arithmetic tinsel and orrery use and that
  *has* been checked live in Arena — but not in this plugin.
- Whether Resolume consumes the `FF_EVENT_FLAG_VALUE` events the preset applier
  raises, so the sliders visibly move when a preset is picked.
- Nothing has been built on Windows or Linux. The macOS build IS universal and
  checked with `lipo` — that one is verified, not assumed.
