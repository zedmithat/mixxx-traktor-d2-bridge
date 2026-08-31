from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
library = (ROOT / "mixxx-source-patches" / "librarycontrol.cpp").read_text(encoding="utf-8")
sidebar = (ROOT / "mixxx-source-patches" / "wlibrarysidebar.cpp").read_text(encoding="utf-8")
bridge = (ROOT / "bridge" / "d2_bridge.c").read_text(encoding="utf-8")
controller = (ROOT / "mixxx-controller" / "Traktor-Kontrol-D2-scripts.js").read_text(
    encoding="utf-8"
)
library_skin_path = ROOT / "skin" / "zed" / "library.xml"
library_skin = library_skin_path.read_text(encoding="utf-8")
ET.parse(library_skin_path)

for label in (
    "MATCH CURRENT DECK",
    "RECENTLY ADDED",
    "UNPLAYED",
    "4+ STARS",
    "BPM 120-126",
    "ALL TRACKS",
):
    assert label in library and label in bridge, f"missing smart preset {label}"

for query in ("played:0", "rating:>=4", "bpm:120-126", "added:>="):
    assert query in library, f"missing Mixxx search query {query}"

assert "KeyUtils::getCompatibleKeys" in library
assert "bpm * 0.96" in library and "bpm * 1.04" in library
assert 'd2ActivateVisibleLabel(QStringLiteral("Tracks"))' in library
assert "emit clicked(index)" in sidebar
assert 'strcmp(key, "SMARTMENU")' in bridge
assert "d2_render_smart_lists" in bridge
assert "d2_screen_state[player].smart_menu ? 61" in bridge
assert 'D2.sendState(group, "SMARTMENU"' in controller
assert 'engine.setValue("[Library]", "d2_smart_list"' in controller

desktop_controls = (
    "smart_match_deck",
    "smart_recent",
    "smart_unplayed",
    "smart_rating",
    "smart_bpm_120_126",
    "smart_all_tracks",
)
for control in desktop_controls:
    assert control in library, f"missing desktop smart control {control}"
    assert f"[Library],{control}" in library_skin, f"skin does not bind {control}"

assert 'ConfigKey("[Skin]", "active_deck")' in library
assert 'ConfigKey(group, "visual_bpm")' in library
assert 'ConfigKey(group, "visual_key")' in library
assert library_skin.count("<ObjectName>SmartPresetButton</ObjectName>") == 6

print(
    "D2_SMART_PLAYLIST_SOURCE_TEST_OK "
    "presets=6 desktop-toolbar=true native-track-model=true"
)
