from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
script = (ROOT / "mixxx-controller" / "Traktor-Kontrol-D2-scripts.js").read_text(
    encoding="utf-8"
)
patch = (ROOT / "mixxx-source-patches" / "cuecontrol-first-beat-auto-cue.patch").read_text(
    encoding="utf-8"
)

cue_handler = script[script.index("D2.cueButton = function"):script.index(
    "D2.hotcueButton = function"
)]
play_handler = script[script.index("D2.playButton = function"):script.index(
    "D2.cueButton = function"
)]

assert 'engine.setValue(activeGroup, "cue_cdj", 1)' in cue_handler
assert 'engine.setValue(activeGroup, "cue_cdj", 0)' in cue_handler
assert '"cue_preview"' not in cue_handler
assert '"cue_set"' not in cue_handler
assert cue_handler.count('"cue_gotoandstop"') == 1
assert '"cue_gotoandstop"' in cue_handler.split("if (D2.shiftPressed[group])", 1)[1]
assert 'engine.setValue(activeGroup, "play", playing ? 0 : 1)' in play_handler

assert "FirstBeat = 5" in patch
assert "seekToFirstBeatAutoCue" in patch
assert "mainCuePosition != mixxx::audio::kStartFramePos" in patch
assert "pTrack->setMainCuePosition(firstBeatPosition)" in patch
assert "SeekOnLoadFirstBeatSetsIndependentMainCue" in patch
assert "SeekOnLoadFirstBeatPreservesStoredMainCue" in patch

print("PIONEER_CUE_SOURCE_TEST_OK native=cue_cdj auto-cue=first-beat")
