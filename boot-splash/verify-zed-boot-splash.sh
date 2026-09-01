#!/bin/sh
set -eu

theme_dir=/usr/share/plymouth/themes/zed-d2
failed=0

check_file() {
    if [ ! -s "$1" ]; then
        echo "MISSING: $1" >&2
        failed=1
    fi
}

check_file "$theme_dir/zed-d2.plymouth"
check_file "$theme_dir/zed-d2.script"
check_file "$theme_dir/zed_z.png"
check_file "$theme_dir/zed_e.png"
check_file "$theme_dir/zed_d.png"
check_file "$theme_dir/progress_bg.png"
check_file "$theme_dir/progress_fill.png"
check_file "$theme_dir/zed-boot-copy.png"
check_file /home/pi/.local/share/zed-d2/boot-final.png
check_file /home/pi/.config/systemd/user/mixxx-d2.service
check_file /home/pi/bin/start-mixxx-d2.sh
check_file /home/pi/bin/zed-usb-monitor.py
check_file /home/pi/.config/systemd/user/zed-usb-monitor.service
check_file /etc/lightdm/lightdm.conf.d/90-zed-standalone.conf
check_file /home/pi/.local/share/icons/zed-invisible/cursors/left_ptr

grep -qw splash /boot/firmware/cmdline.txt || failed=1
grep -qw quiet /boot/firmware/cmdline.txt || failed=1
if grep -qw 'console=tty1' /boot/firmware/cmdline.txt; then
    echo "LOCAL CONSOLE ACTIVE: console=tty1" >&2
    failed=1
fi
grep -qx 'disable_splash=1' /boot/firmware/config.txt || failed=1
grep -q 'systemctl --user start mixxx-d2.service' /home/pi/.config/sway/config || failed=1
grep -q 'zed-boot-copy.png' "$theme_dir/zed-d2.script" || failed=1
grep -qx 'minimum-vt=1' /etc/lightdm/lightdm.conf.d/90-zed-standalone.conf || failed=1
grep -q 'Plymouth.SetQuitFunction' "$theme_dir/zed-d2.script" || failed=1
grep -qx 'seat seat0 xcursor_theme zed-invisible 1' /home/pi/.config/sway/config || failed=1
grep -qx 'export XCURSOR_THEME=zed-invisible' /home/pi/.profile || failed=1
grep -qx 'export XCURSOR_SIZE=1' /home/pi/.profile || failed=1
if grep -q '^seat seat0 hide_cursor ' /home/pi/.config/sway/config; then
    echo "VISIBLE STARTUP CURSOR: timeout-based hiding is still configured" >&2
    failed=1
fi

if [ -e /home/pi/.config/autostart/mixxx-d2-custom.desktop ]; then
    echo "DUPLICATE STARTER: XDG autostart still enabled" >&2
    failed=1
fi

if [ -S /run/user/1000/bus ] && \
        runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user is-active --quiet waybar.service; then
    echo "DESKTOP CHROME ACTIVE: waybar.service" >&2
    failed=1
fi
if [ -S /run/user/1000/bus ] && \
        ! runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user is-active --quiet zed-usb-monitor.service; then
    echo "USB MONITOR INACTIVE: zed-usb-monitor.service" >&2
    failed=1
fi
if [ -d /run/user/1000 ] && [ ! -s /run/user/1000/zed-usb-state ]; then
    echo "USB MONITOR STATE MISSING: /run/user/1000/zed-usb-state" >&2
    failed=1
fi
if [ -S /run/user/1000/bus ] && \
        runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user is-active --quiet swaync.service; then
    echo "DESKTOP CHROME ACTIVE: swaync.service" >&2
    failed=1
fi
if [ -S /run/user/1000/bus ] && \
        [ "$(runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
            systemctl --user is-enabled swaync.service 2>/dev/null || true)" != "masked" ]; then
    echo "DESKTOP CHROME ACTIVE: swaync.service is not masked" >&2
    failed=1
fi

if [ "$(systemctl is-enabled getty@tty1.service 2>/dev/null || true)" != "masked" ]; then
    echo "LOCAL CONSOLE ACTIVE: getty@tty1.service is not masked" >&2
    failed=1
fi
if systemctl is-active --quiet getty@tty1.service; then
    echo "LOCAL CONSOLE ACTIVE: getty@tty1.service is running" >&2
    failed=1
fi

if [ "$failed" -ne 0 ]; then
    echo "ZED_BOOT_SPLASH_VERIFY_FAILED" >&2
    exit 1
fi

echo "ZED_BOOT_SPLASH_VERIFY_OK"
