#!/bin/sh
set -eu

backup_dir=/home/pi/d2-backups/zed-boot-splash
pending_eeprom=/boot/firmware/pieeprom.upd

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

install -d -m 0755 "$backup_dir"
if [ ! -s "$backup_dir/eeprom-config.before" ]; then
    rpi-eeprom-config > "$backup_dir/eeprom-config.before"
fi
if [ ! -s "$backup_dir/eeprom-version.before" ]; then
    vcgencmd bootloader_version > "$backup_dir/eeprom-version.before"
fi

# Use Raspberry Pi OS' supported tool. B2 keeps the recovery UI available when
# SHIFT is held or boot fails, but removes the forced power-on countdown.
raspi-config nonint do_network_install_ui B2

if [ -s "$pending_eeprom" ]; then
    pending_config=$(mktemp)
    trap 'rm -f "$pending_config"' EXIT HUP INT TERM
    rpi-eeprom-config "$pending_eeprom" > "$pending_config"
    if grep -q '^NET_INSTALL_AT_POWER_ON=1$' "$pending_config"; then
        echo "EEPROM network-install UI is still forced on" >&2
        exit 1
    fi
    echo "ZED_EEPROM_NETWORK_INSTALL_UI_DISABLED_PENDING_REBOOT"
else
    # Current Raspberry Pi OS releases may flash and verify the image directly,
    # then remove pieeprom.upd. A successful raspi-config exit is authoritative.
    echo "ZED_EEPROM_NETWORK_INSTALL_UI_DISABLED_APPLIED_REBOOT_REQUIRED"
fi
