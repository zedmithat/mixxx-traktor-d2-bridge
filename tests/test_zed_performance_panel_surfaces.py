#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QSS = (ROOT / "skin" / "zed" / "style.qss").read_text(encoding="utf-8")

for selector in (
    "#HotCuePanel",
    "#KeyShiftPanel",
    "#HotCues",
    "#Beatloop",
    "#KeyShiftFunction",
    "#Stems",
):
    assert selector in QSS, f"missing unified surface selector: {selector}"

assert "background-color: #0d1116;" in QSS
assert "background-color: #1b1e24;" in QSS
assert "border: 1px solid #6d8591;" in QSS
assert "background-color: #17343d;" in QSS
assert "border: 1px solid #00e5ff;" in QSS

for obsolete in (
    "background-image: url(skin:icons/button.png);",
    "background-image: url(skin:icons/button-loop.png);",
    "border: 2px solid #f87021;",
):
    assert obsolete not in QSS, f"obsolete per-page surface remains: {obsolete}"

print("ZED_PERFORMANCE_SURFACES_TEST_OK pages=5 panel=#0d1116 button=#1b1e24 active=#17343d")
