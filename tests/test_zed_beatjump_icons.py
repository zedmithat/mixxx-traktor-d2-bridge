from pathlib import Path
import struct
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SKIN = ROOT / "skin" / "zed"

for name in ("beatjump-backward.svg", "beatjump-forward.svg"):
    tree = ET.parse(SKIN / "icons" / name)
    assert not any(node.tag.endswith("rect") for node in tree.iter())

for name in ("beatjump-backward.png", "beatjump-forward.png"):
    data = (SKIN / "icons" / name).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    width, height = struct.unpack(">II", data[16:24])
    assert (width, height) == (95, 75)
    assert data[25] == 6, "beatjump PNG must retain RGBA transparency"

for name in ("beatjump.xml", "beatjump_deck1.xml", "beatjump_deck2.xml"):
    path = SKIN / name
    ET.parse(path)
    source = path.read_text(encoding="utf-8")
    assert "⏪" not in source and "⏩" not in source
    assert source.count("<ObjectName>BeatJumpBackward</ObjectName>") == 4
    assert source.count("<ObjectName>BeatJumpForward</ObjectName>") == 4

qss = (SKIN / "style.qss").read_text(encoding="utf-8")
assert "url(skin:icons/beatjump-backward.png)" in qss
assert "url(skin:icons/beatjump-forward.png)" in qss
assert "background-position: center center;" in qss
assert "qproperty-alignment: 'AlignCenter';" in qss
assert "padding-left: 34px" not in qss and "padding-right: 34px" not in qss
assert "background-color: #1b1e24;" in qss
assert "border: 1px solid #6d8591;" in qss

print("ZED_BEATJUMP_ICONS_TEST_OK transparent=true format=rgba size=95x75 corner-arrows=true controls=24")
