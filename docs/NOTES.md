# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*(re)gauss — magnetic interference on a CRT + degauss coil; PUBLIC MIT, v0.1.0 RELEASED, runs in Arena; Beat/Bar + audio still unverified in the host*

**(re)gauss** (started 2026-08-17) — a magnet near a television, and the coil
that clears it. `~/projects/resolume/regauss`, **PUBLIC MIT**,
github.com/stoatworks-labs/regauss, **v0.1.0 released 2026-08-17** — signed,
notarised, six assets. Video `https://www.youtube.com/watch?v=p3-tEdHAAlc`,
Instagram Reel `https://www.instagram.com/reel/DcINsymiZu4/`, live demo
`regauss-demo.stoatworks-labs.com`, website card
`stoatworks-labs.com/software/regauss`.

**The one idea:** everything comes from ONE vector field over the tube's face,
read once per electron gun. Geometry error is the field's strength, convergence
error is its **gradient** (three cathodes, three paths, three different fields —
so a uniform field fringes nothing), and purity error is its size measured in
**shadow-mask pitches**. None of the three is drawn. Same discipline as
[old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`)'s "dot crawl is never drawn".

**Deflection and Purity are separate sensitivities and must stay separate** — on
a real tube they are set by different hardware (yoke/anode vs mask pitch/gun
distance), and their real ratio means a magnet too weak to move the geometry
still rotates the colours completely. That is the commonest real fault.

**The field's exponent is 1, not 1.5.** It stands for the field integrated along
the beam's flight, not the field where it lands. With the un-integrated law the
stain was a sliver at the extreme edge; this was the single biggest calibration
fix.

**Degauss is real state, not an animation.** A decaying AC envelope walks the
mask's retained magnetisation down to ~0.09 at 0.77 s, then Recovery brings it
back. The picture dims AND swells while the coil loads the HT (slower electrons
bend further).

**Audio drives the field, not a separate effect** — the default layout is an
unshielded speaker and a speaker's stray field IS the audio signal. Band =
which driver in the cabinet. The audio term is added AFTER the coil envelope, so
degaussing during a loud passage cleans the mask and the stain returns at once.
FFGL only; OFX `Settings` audio fields default to 0 so the term falls out.

Three targets: FFGL bundle (`RG01`), OpenFX (links Field/Controls/Masks from
source, mirrors only the two per-pixel passes; no Degauss button — the coil is
scheduled, Beat/Bar run off a Tempo control), and a WebGL2 demo.

**The demo's shaders are GENERATED** by `demo/extract-shaders.mjs` from
`source/shaders/` into a committed `demo/shaders.js`, and `verify.sh --check`s
the drift. That is a deliberate improvement on the rest of the fleet's "copied
unedited" demos, where nothing checks the claim.

**Run in Resolume Arena 2026-08-17 (Allan's own report): it loads, renders, and the Degauss button fires the coil once per press** — so Resolume does draw an FF_TYPE_EVENT as a button and does send one rising edge, which the trigger design assumed. That also retires SetTextParameter / OBJECT-registration / param-declaration worries as a class. **Beat/Bar and the audio reader are STILL unverified** — the manual press and the Auto schedule reach manualTrigger by different routes, and the FFT has only ever been rgtest's synthetic one. Never run in a live show; OFX never loaded into Resolve. Everything else is offline: `verify.sh`:
GLSL field vs Field.cpp <1e-6 through a probe built from the SAME strings; the
purity matrix (identity at rest, clean rotation at one phosphor, light
conserved, no vertical purity on a grille); the coil envelope and the full
magnetise/clear/recover loop; 40 live parameters; 4 measured mask gains; the
universal binary. OFX renders under ofxprobe.

**Mask gains are measured at flat 0.30, NOT 0.05** — at 0.05 the 8-bit
readback's rounding exceeds the quantity measured and a 2.4% gain change moved
the mean 5.3% the wrong way. Differs from [old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`) deliberately.

**Windows needed `<cstdint>` and `std::` on the fixed-width types** — libc++
drags them in through `<cmath>` and into the global namespace, MSVC does not,
and the error points at the line AFTER the offending one. Caught by dispatching
the release workflow before tagging, which is what that trigger is for.

Related: [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md), [new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md),
[ffgl audio bpm patterns](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_audio_bpm_patterns.md), [plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md),
**disclaimer scope** (working-practice note, kept in Claude memory).
