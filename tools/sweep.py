"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**Render big enough.** The scanline and mask anti-aliasing fade out on fwidth.
Below about two output pixels per scan line the modulation is genuinely
unresolvable, so Scanlines correctly does nothing and looks broken.

**Half of this plugin does nothing until the coil has fired.** Duration,
Intensity, Coil Sag and Recovery are all properties of a transient, and against
a baseline with Auto off they are all stone dead and all correct. The baseline
therefore runs Auto on an interval, and the capture time is chosen to land
inside a transient -- which is also why several parameters need a CONTEXT entry
below rather than sharing one.

**Watch for controls masked by other controls.** If something reads dead, work
out what is hiding it before assuming the test is wrong.

**Never sweep the About block.** Those parameters are buttons that open a web
browser, and sweeping them opens one tab per press.
"""
import subprocess, zlib, struct, sys, tempfile

SC = tempfile.mkdtemp(prefix="rgsweep")
BIN = "./build/rgtest"

WIDTH, HEIGHT = 1280, 960

# A baseline with every stage active, so nothing is dead merely because the
# thing it modifies is switched off. Auto is on an interval so that the coil
# fires and the whole Degauss group has something to be a property of.
BASE = {
    "Magnetisation": 0.8, "Wander": 0.5, "Interference": 0.35, "Deflection": 0.4,
    "Purity": 0.7, "Convergence": 0.5, "Overscan": 0.3,
    "Auto": 1, "Interval": 0.3, "Duration": 0.62, "Intensity": 0.8,
    "Coil Sag": 0.8, "Recovery": 0.5,
    "Mask Pattern": 1, "Mask Strength": 0.7, "Scanlines": 0.6, "Halation": 0.4,
    "Persistence": 0.3, "Beam Bloom": 0.5,
    "Curvature": 0.3, "Vignette": 0.3, "Corner Radius": 0.2,
}

# Frames at 60 fps. 100 frames is 1.667 s, which with the baseline's interval
# lands partway through a transient -- late enough that Interval changes which
# firing is the current one, early enough that the coil is still doing
# something.
FRAMES, FPS = 100, 60

# Parameters that need the world arranged differently before they can be seen
# at all. The value is (extra settings, frames, fps).
CONTEXT = {
    # Recovery only means anything once a transient is OVER, so: one manual
    # firing at t=0, and a capture five seconds later. At 0.2 s the mask has
    # taken all its magnetisation back; at 60 s it is still clean.
    "Recovery": ({"Auto": 0, "Degauss": 1}, 100, 20),

    # The poles drift slowly on purpose -- rates are around a tenth of a hertz
    # so the loop is not findable by eye. Over 1.7 s they barely move, so this
    # one needs a longer look.
    "Wander": ({}, 120, 8),

    # An event, not a value. Sweeping it from 0 to 1 IS the press, and it has
    # to be judged against a baseline where the coil is not already firing for
    # its own reasons.
    "Degauss": ({"Auto": 0}, 30, 60),

    # Frequency is the rate of the alternating field, so it needs the field to
    # be alternating and the coil to be quiet enough not to swamp it.
    "Frequency": ({"Auto": 0, "Interference": 0.6}, 40, 60),

    # Interference likewise: with the coil firing on an interval its own field
    # is several times larger and the comparison is against noise.
    "Interference": ({"Auto": 0}, 40, 60),
}

# Options are discrete; sweep them across their real element range.
DISCRETE = {
    "Render": (0, 1), "Layout": (0, 4), "Auto": (0, 3), "Mask Pattern": (1, 4),
    "Preset": (0, 8), "Degauss": (0, 1),
}


def render(path, overrides, frames, fps):
    args = [BIN, "--out", path, "--width", str(WIDTH), "--height", str(HEIGHT),
            "--frames", str(frames), "--fps", str(fps)]
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    return open(path, "rb").read()


def pixels(png):
    i = 8
    idat = b""
    w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


listing = subprocess.run([BIN, "--list"], capture_output=True, text=True).stdout
params = [" ".join(l.split()[1:-1]) for l in listing.strip().splitlines()]

# Everything from the About text line onwards is the Stoatworks About block:
# one display-only string and a row of buttons that open a web browser. Not
# controls, and pressing them is not a test.
if "About" in params:
    params = params[:params.index("About")]

print(f"{'parameter':<20} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    extra, frames, fps = CONTEXT.get(p, ({}, FRAMES, FPS))

    a = render(f"{SC}/a.png", {**extra, p: lo}, frames, fps)
    b = render(f"{SC}/b.png", {**extra, p: hi}, frames, fps)
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok:
        dead.append(p)
    print(f"{p:<20} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
