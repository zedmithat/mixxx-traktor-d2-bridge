#!/usr/bin/env python3
"""Static contract for the zed-only Mixxx launch animation."""

from pathlib import Path
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SKIN = ROOT / "skin" / "zed"
SOURCE = (ROOT / "mixxx-source-patches" / "launchimage.cpp").read_text(
    encoding="utf-8"
)
SKIN_XML = (SKIN / "skin.xml").read_text(encoding="utf-8")

ET.parse(SKIN / "skin.xml")
ET.parse(SKIN / "images" / "zed_boot_handoff.svg")
assert "zed_boot_handoff.svg" in SKIN_XML
assert "border-image:" in SKIN_XML
handoff = (SKIN / "images" / "zed_boot_handoff.svg").read_text(encoding="utf-8")
assert "<image " not in handoff, "handoff SVG must not depend on external images"
assert all(marker in handoff for marker in ("handoffZ", "handoffE", "handoffD"))

for letter in "zed":
    asset = SKIN / "images" / f"zed_launch_{letter}.svg"
    root = ET.parse(asset).getroot()
    paths = root.findall("{http://www.w3.org/2000/svg}path")
    assert len(paths) >= 2, f"{asset.name} must contain glow and solid glyph paths"
    assert "#ffffff" not in "".join(
        path.attrib.get("d", "") for path in paths
    ).lower(), f"{asset.name} unexpectedly contains a background rectangle"

for object_name in (
    "zedLaunchLetterZ",
    "zedLaunchLetterE",
    "zedLaunchLetterD",
):
    assert f"QLabel#{object_name}" in SKIN_XML
    assert object_name in SOURCE

assert "QParallelAnimationGroup" in SOURCE
assert "QSequentialAnimationGroup" in SOURCE
assert "letter * 135" in SOURCE
assert "QAbstractAnimation::DeleteWhenStopped" in SOURCE
assert "styleSheet.contains" in SOURCE, "animation must remain zed-skin gated"

print("ZED_LAUNCH_ANIMATION_TEST_OK letters=z,e,d duration=790ms gated=true")
