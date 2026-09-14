# Current Touch FX source overlay

This directory preserves the 37-file, allowlisted ZED pi-gen Touch FX source
layout. It is nested here so that its pi-gen BSD notice, original ZED GPL grant,
packaging tests and stage paths do not replace this repository's existing files.
It contains no Linux image, device credentials, music library or third-party DJ
firmware. `SOURCE-MANIFEST.json` lists the copied payload hashes; this README and
the manifest are wrapper documentation, not entries in that payload list.

The implementation matches the accepted 2026-09-12 final image's `touchfx.py`,
`touchfx_core.py` and `TouchFX-macros.js` hashes. Older dated package references in
the preserved source documentation describe development history, **not the latest
binary download**. The clean OS release remains on hold; no image is installed by
cloning or running these source tests.

- [Application and standalone setup](touch-fx/README.md)
- [Macro controls and approximations](touch-fx/MACROS.md)
- [ZED integration and unit isolation](touch-fx/integration/README.md)
- [GPL-3.0-or-later scope and upstream exceptions](touch-fx/LICENSING.md)
- [Project update and UI previews](../../docs/SEPTEMBER-2026-UPDATE.md)

## Preview without MIDI or a Pi

From this directory, with Python, PyQt5 and mido installed in a development
environment:

```sh
python3 touch-fx/touchfx.py --dry-run --zed-session
```

This previews the two-deck UI with simulated MIDI, not audio/DSP processing.
The ZED mapping reserves Effect Unit 3; the standalone mapping reserves Unit 1.
Do not enable both mappings on one port or install an overlay over a playing
system. The integrated installer requires the reviewed base ZED profile and
checks its D2 mapping identity; this folder is not a complete pi-gen checkout.

## Source tests

```sh
python3 -B -m unittest discover -s touch-fx/tests -p 'test_*.py'
node touch-fx/tests/test_mapping.cjs
node touch-fx/tests/test_macros.cjs
```

Qt tests run offscreen. Platform-specific filesystem/Wayland tests may require
Linux or report skips; these tests do not replace physical audio acceptance.
Do not copy a virtual environment or generated test captures into a source
release. Build-stage shell files must keep LF endings and executable permissions
when used in a Linux image-builder checkout.
