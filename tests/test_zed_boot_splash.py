from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / "boot-splash"

required = (
    "zed-d2.plymouth",
    "zed-d2.script",
    "zed-boot-background.svg",
    "zed-boot-copy.svg",
    "progress-bg.svg",
    "progress-fill.svg",
    "install-zed-boot-splash.sh",
    "verify-zed-boot-splash.sh",
    "restore-zed-boot-splash.sh",
    "disable-network-install-ui.sh",
    "generate-transparent-xcursor.py",
    "install-zed-invisible-cursor.sh",
    "90-zed-standalone.conf",
)
for name in required:
    path = BOOT / name
    assert path.is_file() and path.stat().st_size > 0, f"missing boot asset {name}"

for name in (
    "zed-boot-background.svg",
    "zed-boot-copy.svg",
    "progress-bg.svg",
    "progress-fill.svg",
):
    ET.parse(BOOT / name)

plymouth = (BOOT / "zed-d2.plymouth").read_text(encoding="utf-8")
script = (BOOT / "zed-d2.script").read_text(encoding="utf-8")
installer = (BOOT / "install-zed-boot-splash.sh").read_text(encoding="utf-8")
verifier = (BOOT / "verify-zed-boot-splash.sh").read_text(encoding="utf-8")
eeprom_ui = (BOOT / "disable-network-install-ui.sh").read_text(encoding="utf-8")
cursor_installer = (BOOT / "install-zed-invisible-cursor.sh").read_text(encoding="utf-8")
service = (ROOT / "systemd" / "mixxx-d2.service").read_text(encoding="utf-8")
usb_service = (ROOT / "systemd" / "zed-usb-monitor.service").read_text(
    encoding="utf-8"
)
launcher = (ROOT / "systemd" / "start-mixxx-d2.sh").read_text(encoding="utf-8")
bridge = (ROOT / "bridge" / "d2_bridge.c").read_text(encoding="utf-8")

assert "ModuleName=script" in plymouth
assert "Plymouth.SetRefreshFunction" in script
assert "Plymouth.SetBootProgressFunction" in script
assert "Plymouth.SetQuitFunction" in script
for letter in ("zed_z", "zed_e", "zed_d"):
    assert f'Image("{letter}.png")' in script

for token in (
    "quiet",
    "splash",
    "loglevel=3",
    "logo.nologo",
    "vt.global_cursor_default=0",
    "plymouth.ignore-serial-consoles",
):
    assert token in installer

assert "d2-backups/zed-boot-splash" in installer
assert '[ ! -e "$backup_dir/cmdline.txt.before" ]' in installer
assert "mixxx-d2-custom.desktop.disabled" in installer
assert "systemctl --user start mixxx-d2.service" in installer
assert "systemctl --user start waybar.service" in installer
assert "mask --now waybar.service" in installer
assert "mask --now swaync.service" in installer
assert "zed-invisible" in installer
assert "install-zed-invisible-cursor.sh" in installer
assert "generate-transparent-xcursor.py" in cursor_installer
assert "seat seat0 hide_cursor 100" not in installer
assert "seat seat0 xcursor_theme zed-invisible 1" in cursor_installer
assert "ZED_INVISIBLE_CURSOR_INSTALLED_RELOGIN_REQUIRED" in cursor_installer
assert "90-zed-standalone.conf" in installer
assert "minimum-vt=1" in (BOOT / "90-zed-standalone.conf").read_text(encoding="utf-8")
assert "token != 'console=tty1'" in installer
assert "systemctl mask --now getty@tty1.service" in installer
assert "getty@tty1.service is not masked" in verifier
assert "DUPLICATE STARTER" in verifier
assert "DESKTOP CHROME ACTIVE" in verifier
assert "swaync.service is not masked" in verifier
assert "VISIBLE STARTUP CURSOR" in verifier
assert "eeprom-config.before" in eeprom_ui
assert "eeprom-version.before" in eeprom_ui
assert "raspi-config nonint do_network_install_ui B2" in eeprom_ui
assert "NET_INSTALL_AT_POWER_ON=1" in eeprom_ui
assert "ZED_EEPROM_NETWORK_INSTALL_UI_DISABLED_PENDING_REBOOT" in eeprom_ui
assert "ZED_EEPROM_NETWORK_INSTALL_UI_DISABLED_APPLIED_REBOOT_REQUIRED" in eeprom_ui
assert "After=d2-bridge.service sway-session.service" in service
assert "Restart=on-failure" in service
assert "sleep 5" not in launcher
assert "Wayland socket not ready" in launcher
assert "wayland-1 wayland-0" in launcher
assert "zed-usb-monitor.py" in installer
assert "enable --now zed-usb-monitor.service" in installer
assert "zed-usb-monitor.service" in verifier
assert "IOSchedulingClass=idle" in usb_service
assert "ReadWritePaths=/run/user/1000" in usb_service

assert "d2_render_startup_fast" in bridge
assert "mixxx_ready" in bridge
assert 'strcmp(argv[1], "--render-startup-test")' in bridge
assert 'strcmp(key, "VIEW") == 0' in bridge
for text in ("OPEN DJ SYSTEM", "BUILT FOR DJS.", "NOT SHAREHOLDERS."):
    assert text in bridge

with tempfile.TemporaryDirectory() as temp_dir:
    cursor_path = Path(temp_dir) / "left_ptr"
    subprocess.run(
        [sys.executable, str(BOOT / "generate-transparent-xcursor.py"), str(cursor_path)],
        check=True,
    )
    cursor = cursor_path.read_bytes()
    assert len(cursor) == 68
    magic, header_size, version, toc_count = struct.unpack_from("<IIII", cursor, 0)
    assert magic == 0x72756358
    assert header_size == 16
    assert version == 0x00010000
    assert toc_count == 1
    assert cursor[-4:] == b"\x00\x00\x00\x00"

print("ZED_BOOT_SPLASH_TEST_OK plymouth=true sway-handoff=true d2-startup=true")
