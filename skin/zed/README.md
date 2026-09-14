# ZED two-deck skin

This directory is the base `zed` skin source. September updates include
deck-following cover art and track metadata, a deck selector to the left of
the cover, Wi-Fi state controls, consistent performance-panel styling,
image-based beat-jump arrows and per-deck effect selectors.

## Base skin versus Touch FX

The photographed personal installation uses the generated **`zed-touchfx`**
variant, not an unmodified copy of this directory. The
[Touch FX integration](../../integrations/touch-fx-source/touch-fx/integration/install_rootfs.py)
copies the base skin into a separate variant, adds the Touch FX control and
connects it to the Python application through the supplied MIDI mapping.
It preserves the base skin. The standalone pad is not embedded into an XML
skin simply by copying `touchfx.py`.

That installer targets a validated, staged ZED root filesystem and checks its
controller/profile prerequisites; it is not a generic installer for an arbitrary
desktop Mixxx installation. Do not remove its checks or run it against `/`.
Wi-Fi/settings and custom Smart List controls also require the matching ZED
services/controller integration and custom Mixxx build. Copying this directory
alone does not install those components or update the D2 hardware renderer.

See the [source overlay documentation](../../integrations/touch-fx-source/README.md)
and [current hardware photos](../../docs/SEPTEMBER-2026-UPDATE.md).
`skin_preview.png` is a retained historical chooser thumbnail, not a current
hardware screenshot. This source publication does not modify installed devices.

## Validation

From the repository root, run:

```sh
python3 tests/test_zed_topbar_cover.py
python3 tests/test_zed_fx_layout.py
python3 tests/test_zed_beatjump_icons.py
python3 tests/test_zed_performance_panel_surfaces.py
python3 tests/test_zed_tab_background.py
```

These are source/layout regression checks, not a replacement for physical
touchscreen and audio testing. Upstream skin attribution and the existing
[license](LICENSE) are retained.
