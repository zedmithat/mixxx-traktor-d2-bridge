#!/bin/sh
set -eu

backup_dir=/home/pi/d2-backups/zed-boot-splash
sway_config=/home/pi/.config/sway/config
autostart_file=/home/pi/.config/autostart/mixxx-d2-custom.desktop
lightdm_handoff=/etc/lightdm/lightdm.conf.d/90-zed-standalone.conf
profile_file=/home/pi/.profile
usb_monitor_script=/home/pi/bin/zed-usb-monitor.py
usb_monitor_service=/home/pi/.config/systemd/user/zed-usb-monitor.service

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

for required in cmdline.txt.before config.txt.before sway-config.before; do
    if [ ! -s "$backup_dir/$required" ]; then
        echo "Missing rollback file: $backup_dir/$required" >&2
        exit 1
    fi
done

install -m 0644 "$backup_dir/cmdline.txt.before" /boot/firmware/cmdline.txt
install -m 0644 "$backup_dir/config.txt.before" /boot/firmware/config.txt
install -m 0644 "$backup_dir/sway-config.before" "$sway_config"
chown pi:pi "$sway_config"
if [ -s "$backup_dir/profile.before" ]; then
    install -m 0644 "$backup_dir/profile.before" "$profile_file"
    chown pi:pi "$profile_file"
fi

rm -f /home/pi/.config/systemd/user/mixxx-d2.service
systemctl unmask getty@tty1.service || true
if grep -qx enabled "$backup_dir/getty-tty1-enabled.before" 2>/dev/null; then
    systemctl enable getty@tty1.service
fi
if [ -s "$backup_dir/mixxx-d2-custom.desktop.before" ]; then
    install -m 0644 "$backup_dir/mixxx-d2-custom.desktop.before" "$autostart_file"
    chown pi:pi "$autostart_file"
fi
if [ -s "$backup_dir/90-zed-standalone.conf.before" ]; then
    install -m 0644 "$backup_dir/90-zed-standalone.conf.before" \
        "$lightdm_handoff"
else
    rm -f "$lightdm_handoff"
fi

if [ -s "$backup_dir/plymouth-theme.before" ]; then
    previous_theme=$(sed -n '1p' "$backup_dir/plymouth-theme.before")
    if [ -n "$previous_theme" ]; then
        plymouth-set-default-theme -R "$previous_theme"
    else
        update-initramfs -u
    fi
else
    update-initramfs -u
fi

if [ -S /run/user/1000/bus ]; then
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user disable --now zed-usb-monitor.service || true
fi
if [ -s "$backup_dir/zed-usb-monitor.py.before" ]; then
    install -m 0755 "$backup_dir/zed-usb-monitor.py.before" "$usb_monitor_script"
    chown pi:pi "$usb_monitor_script"
else
    rm -f "$usb_monitor_script"
fi
if [ -s "$backup_dir/zed-usb-monitor.service.before" ]; then
    install -m 0644 "$backup_dir/zed-usb-monitor.service.before" \
        "$usb_monitor_service"
    chown pi:pi "$usb_monitor_service"
else
    rm -f "$usb_monitor_service"
fi
rm -f /run/user/1000/zed-usb-state

if [ -S /run/user/1000/bus ]; then
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user unmask waybar.service || true
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user unmask swaync.service || true
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user daemon-reload
    if grep -qx enabled "$backup_dir/waybar-enabled.before" 2>/dev/null; then
        runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
            systemctl --user enable --now waybar.service
    fi
    if grep -qx enabled "$backup_dir/swaync-enabled.before" 2>/dev/null; then
        runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
            systemctl --user enable --now swaync.service
    fi
fi

echo "Zed boot changes restored. Reboot when ready."
