from pathlib import Path
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
bridge = (ROOT / "bridge" / "d2_bridge.c").read_text(encoding="utf-8")
controller = (
    ROOT / "mixxx-controller" / "Traktor-Kontrol-D2-scripts.js"
).read_text(encoding="utf-8")
mapping_path = ROOT / "mixxx-controller" / "Traktor-Kontrol-D2.midi.xml"
mapping = mapping_path.read_text(encoding="utf-8")
library = (
    ROOT / "mixxx-source-patches" / "librarycontrol.cpp"
).read_text(encoding="utf-8")
sidebar = (
    ROOT / "mixxx-source-patches" / "wlibrarysidebar.cpp"
).read_text(encoding="utf-8")
monitor = (ROOT / "systemd" / "zed-usb-monitor.py").read_text(encoding="utf-8")

ET.parse(mapping_path)

assert "/run/user/1000/zed-usb-state" in bridge
assert "d2_load_usb_source_state" in bridge
assert 'e->button.id == 28 &&' in bridge
assert "midi_note(midi_channel, 101" in bridge
assert "d2_usb_source.present ? bright : dim" in bridge
assert '"USB:%.11s"' in bridge

assert "D2.usbOpenButton" in controller
assert 'D2.pulse("[Library]", "d2_usb_open")' in controller
assert "<midino>0x65</midino>" in mapping

assert 'ConfigKey("[Library]", "d2_usb_open")' in library
assert 'QStringLiteral("/run/user/1000/zed-usb-state")' in library
assert "d2OpenRemovableDevice(label)" in library
assert "QTimer::singleShot(650, this, activateUsb)" in library
assert 'QStringLiteral("Computer")' in sidebar
assert "emit clicked(index)" in sidebar

# The monitor must publish mount metadata only. Recursive scanning, copying and
# external database/import commands do not belong in the live performance path.
for forbidden in ("os.walk", "rglob(", "copyfile", "rsync", "mixxxdb"):
    assert forbidden not in monitor

print(
    "D2_USB_SOURCE_TEST_OK direct-native-browser=true "
    "capture-context=true scan=false"
)
