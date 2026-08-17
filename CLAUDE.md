# regauss

Electromagnetic interference on a CRT, and the degauss coil that clears it, as
an FFGL effect for Resolume Arena/Avenue plus an OpenFX build for
Resolve/Nuke/Natron/Vegas. C++/GLSL, CMake MODULE → universal `.bundle` (macOS)
+ Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the field model, the two sensitivities, or
either shader pass.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/rgtest --out /tmp/frame.png`
- List parameters: `./build/rgtest --list`
- Set a control: `--set "Layout=1" --set "Magnetisation=0.9"` (by display name)
- Fire the coil part way through: `--degauss 0.15 --frames 20`
- Put real footage through the real shaders:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/rgtest --pipe --width W --height H [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh`
- GLSL field vs `Field.cpp`: `./build/rgtest --field`
- The purity model: `./build/rgtest --purity`
- The degauss coil and the magnetise/clear/recover loop: `./build/rgtest --coil`
- No dead controls: `python3 tools/sweep.py`
- Universal + exports: `lipo -archs build/Regauss.bundle/Contents/MacOS/Regauss`
  and `nm -gU … | grep _plugMain`

## OpenFX build
- `source/ofx/RegaussOFX.cpp` → `build/Regauss.ofx.bundle` (target `RegaussOFX`,
  `-DBUILD_OFX=OFF` to skip). `Field.cpp`, `Controls.cpp` and `Masks.cpp` link
  straight from source, so the model cannot drift; only the two per-pixel passes
  are mirrored. Every mirrored line is marked `//= mirrored`.
- Three deliberate departures: no Degauss button (OFX renders out of order, so
  the coil is scheduled and there is a Degauss At offset), Beat/Bar run off a
  Tempo control rather than a host transport, and there is no Persistence or
  Halation.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.regauss --size 640x360 --out /tmp/rg.bmp`
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Notes
- **One field, read three times.** Geometry error is its strength, convergence
  error is its gradient, purity error is its size in mask pitches. Nothing is
  drawn directly. See `AGENTS.md`.
- Deflection and Purity are **separate sensitivities** and always will be — a
  real tube's two are set by different hardware, and a weak magnet gives a big
  purity error with no visible geometry error at all.
- The field's exponent is **one, not three halves**: it stands for the field
  integrated along the beam's flight, not the field where it lands.
- Runs at the composition's resolution, unlike old-cathode. A magnet is a fact
  about the room, not about a broadcast standard.
- Mask gains in `source/Masks.cpp` are **measured** with
  `rgtest --flat 0.30 --measure` — at 0.30, not 0.05; see the note in that file.
- The vertex shader passes UVs through **unscaled**; MaxUV is applied at the
  fetch, after clamping. This plugin warps.
- Always override `SetTextParameter` to return FF_SUCCESS for the About block,
  or no host can instantiate the plugin.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. It names the pass, logs the GL
vendor/renderer/version, and records the host clock's unit once it is decided.

    ~/Library/Logs/regauss/regauss.YYYY-MM-DD.log
