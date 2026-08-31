from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
library = (ROOT / "mixxx-source-patches" / "librarycontrol.cpp").read_text(encoding="utf-8")
sidebar = (ROOT / "mixxx-source-patches" / "wlibrarysidebar.cpp").read_text(encoding="utf-8")
bridge = (ROOT / "bridge" / "d2_bridge.c").read_text(encoding="utf-8")
controller = (ROOT / "mixxx-controller" / "Traktor-Kontrol-D2-scripts.js").read_text(
    encoding="utf-8"
)

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

print("D2_SMART_PLAYLIST_SOURCE_TEST_OK presets=6 native-track-model=true")
