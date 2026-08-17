# Attributions

(re)gauss is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

> **This copy is hand-written and provisional.** The generator skips any repo
> without a `stoatworks-labs/` origin remote, and this one has no remote yet.
> Run the sync once the repo is pushed and it will replace this file wholesale.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there is no
other way to be loadable by Resolume Arena and Avenue.

### OpenFX

<https://github.com/AcademySoftwareFoundation/openfx>  
Licence: BSD-3-Clause  
Copyright: The Open Effects Association

A subset vendored at `external/openfx`: the C headers and the official C++ Support
library. The OpenFX build of this plugin is defined by them, the same way the FFGL
build is defined by the FFGL SDK.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: Modified BSD / MIT  
Copyright: Nigel Stewart, Milan Ikits, Marcelo Magallon, Lev Povalahev

Pulled in through `vcpkg.json` on Windows and Linux only. macOS uses the system
OpenGL framework, so it is not a dependency there.

### zlib

<https://zlib.net>  
Licence: zlib  
Copyright: Jean-loup Gailly and Mark Adler

Linked by the offline harness (`tools/rgtest`) only, to deflate the PNGs it writes.
It ships with macOS, which is why the PNG writer in the harness is fifty lines
rather than a vendored dependency. It is **not** linked into the shipped plugin.
