from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
browse = (
    ROOT / "mixxx-source-patches" / "browsefeature.cpp"
).read_text(encoding="utf-8")
folder_model = (
    ROOT / "mixxx-source-patches" / "foldertreemodel.cpp"
).read_text(encoding="utf-8")
library = (
    ROOT / "mixxx-source-patches" / "librarycontrol.cpp"
).read_text(encoding="utf-8")
monitor = (ROOT / "systemd" / "zed-usb-monitor.py").read_text(encoding="utf-8")

for source in (browse, folder_model):
    for hidden in (
        "$recycle.bin",
        "system volume information",
        "recovery",
        "efi",
        "lost+found",
        "__macosx",
    ):
        assert hidden in source, f"missing hidden USB folder filter: {hidden}"

assert "shouldHideDjSystemEntry" in browse
assert "isDjBrowsableVolume" in browse
assert "isLikelyAudioFile" in browse
assert "if (!isDjBrowsableVolume(device))" in browse
assert "shouldHideDjSystemDirectory" in folder_model
assert "QFile::decodeName(entry->d_name)" in folder_model

assert "getBrowseRowCount() > 0" in library
assert "isLeafNodeSelected() || trackCount > 0" in library
assert "FocusWidget::TracksTable" in library

assert "generated_mount_name" in monitor
assert '"-usb-" in lowered' in monitor
assert "device_sort_key" in monitor
assert "os.walk" not in monitor and "rglob(" not in monitor

print(
    "D2_USB_PLAYER_SOURCE_TEST_OK system-folders=hidden "
    "music-volume=preferred mixed-folder=tracks"
)
