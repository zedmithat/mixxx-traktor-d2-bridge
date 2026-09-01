#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
theme_dir=/usr/share/plymouth/themes/zed-d2
share_dir=/home/pi/.local/share/zed-d2
backup_dir=/home/pi/d2-backups/zed-boot-splash
sway_config=/home/pi/.config/sway/config
autostart_file=/home/pi/.config/autostart/mixxx-d2-custom.desktop
lightdm_handoff=/etc/lightdm/lightdm.conf.d/90-zed-standalone.conf
usb_monitor_script=/home/pi/bin/zed-usb-monitor.py
usb_monitor_service=/home/pi/.config/systemd/user/zed-usb-monitor.service

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

install -d -m 0755 "$backup_dir" "$theme_dir" "$share_dir"
if [ ! -e "$backup_dir/cmdline.txt.before" ]; then
    cp -a /boot/firmware/cmdline.txt "$backup_dir/cmdline.txt.before"
fi
if [ ! -e "$backup_dir/config.txt.before" ]; then
    cp -a /boot/firmware/config.txt "$backup_dir/config.txt.before"
fi
if [ ! -e "$backup_dir/sway-config.before" ]; then
    cp -a "$sway_config" "$backup_dir/sway-config.before"
fi
if [ -f "$autostart_file" ] && \
        [ ! -e "$backup_dir/mixxx-d2-custom.desktop.before" ]; then
    cp -a "$autostart_file" "$backup_dir/mixxx-d2-custom.desktop.before"
fi
if [ ! -s "$backup_dir/waybar-enabled.before" ] && [ -S /run/user/1000/bus ]; then
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user is-enabled waybar.service \
        > "$backup_dir/waybar-enabled.before" 2>/dev/null || \
        printf '%s\n' disabled > "$backup_dir/waybar-enabled.before"
fi
if [ ! -s "$backup_dir/swaync-enabled.before" ] && [ -S /run/user/1000/bus ]; then
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user is-enabled swaync.service \
        > "$backup_dir/swaync-enabled.before" 2>/dev/null || \
        printf '%s\n' disabled > "$backup_dir/swaync-enabled.before"
fi
if [ -e "$lightdm_handoff" ] && \
        [ ! -e "$backup_dir/90-zed-standalone.conf.before" ]; then
    cp -a "$lightdm_handoff" "$backup_dir/90-zed-standalone.conf.before"
fi
if [ ! -s "$backup_dir/getty-tty1-enabled.before" ]; then
    systemctl is-enabled getty@tty1.service \
        > "$backup_dir/getty-tty1-enabled.before" 2>/dev/null || \
        printf '%s\n' disabled > "$backup_dir/getty-tty1-enabled.before"
fi
if [ -e "$usb_monitor_script" ] && \
        [ ! -e "$backup_dir/zed-usb-monitor.py.before" ]; then
    cp -a "$usb_monitor_script" "$backup_dir/zed-usb-monitor.py.before"
fi
if [ -e "$usb_monitor_service" ] && \
        [ ! -e "$backup_dir/zed-usb-monitor.service.before" ]; then
    cp -a "$usb_monitor_service" "$backup_dir/zed-usb-monitor.service.before"
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    plymouth plymouth-themes librsvg2-bin

if [ ! -s "$backup_dir/plymouth-theme.before" ]; then
    plymouth-set-default-theme > "$backup_dir/plymouth-theme.before"
fi

install -m 0644 "$script_dir/zed-d2.plymouth" "$theme_dir/zed-d2.plymouth"
install -m 0644 "$script_dir/zed-d2.script" "$theme_dir/zed-d2.script"
install -m 0644 "$project_dir/skin/zed/images/zed_launch_z.svg" "$theme_dir/zed_z.svg"
install -m 0644 "$project_dir/skin/zed/images/zed_launch_e.svg" "$theme_dir/zed_e.svg"
install -m 0644 "$project_dir/skin/zed/images/zed_launch_d.svg" "$theme_dir/zed_d.svg"
install -m 0644 "$script_dir/zed-boot-background.svg" "$theme_dir/zed-boot-background.svg"

rsvg-convert -w 108 -h 132 -o "$theme_dir/zed_z.png" "$theme_dir/zed_z.svg"
rsvg-convert -w 108 -h 132 -o "$theme_dir/zed_e.png" "$theme_dir/zed_e.svg"
rsvg-convert -w 108 -h 132 -o "$theme_dir/zed_d.png" "$theme_dir/zed_d.svg"
rsvg-convert -w 328 -h 3 -o "$theme_dir/progress_bg.png" "$script_dir/progress-bg.svg"
rsvg-convert -w 328 -h 3 -o "$theme_dir/progress_fill.png" "$script_dir/progress-fill.svg"
rsvg-convert -w 520 -h 116 -o "$theme_dir/zed-boot-copy.png" \
    "$script_dir/zed-boot-copy.svg"
install -d -m 0755 /etc/lightdm/lightdm.conf.d
install -m 0644 "$script_dir/90-zed-standalone.conf" "$lightdm_handoff"
(
    cd "$theme_dir"
    rsvg-convert -w 1280 -h 800 -o "$share_dir/boot-final.png" \
        zed-boot-background.svg
)
chown -R pi:pi "$share_dir"

"$script_dir/install-zed-invisible-cursor.sh"

install -m 0755 "$project_dir/systemd/start-mixxx-d2.sh" /home/pi/bin/start-mixxx-d2.sh
install -m 0644 "$project_dir/systemd/mixxx-d2.service" \
    /home/pi/.config/systemd/user/mixxx-d2.service
install -m 0755 "$project_dir/systemd/zed-usb-monitor.py" "$usb_monitor_script"
install -m 0644 "$project_dir/systemd/zed-usb-monitor.service" \
    "$usb_monitor_service"
chown pi:pi /home/pi/bin/start-mixxx-d2.sh \
    /home/pi/.config/systemd/user/mixxx-d2.service \
    "$usb_monitor_script" "$usb_monitor_service"

# One authoritative Mixxx launcher. Remove the generated XDG service and the
# old direct Sway exec, then start the user unit after Sway imports Wayland.
if [ -f "$autostart_file" ]; then
    mv "$autostart_file" "$backup_dir/mixxx-d2-custom.desktop.disabled"
fi
sed -i '\|start-mixxx-d2.sh|d' "$sway_config"
sed -i '\|systemctl --user start mixxx-d2.service|d' "$sway_config"
sed -i '\|systemctl --user start waybar.service|d' "$sway_config"
sed -i 's|^output \* bg .*|output * bg /home/pi/.local/share/zed-d2/boot-final.png fill|' \
    "$sway_config"
cat >> "$sway_config" <<'EOF'

# Zed standalone startup: systemd owns the only Mixxx process.
exec --no-startup-id systemctl --user import-environment DISPLAY WAYLAND_DISPLAY SWAYSOCK XDG_CURRENT_DESKTOP XDG_SESSION_TYPE
exec --no-startup-id systemctl --user start mixxx-d2.service
EOF
chown pi:pi "$sway_config"

python3 - <<'PY'
from pathlib import Path

cmdline_path = Path('/boot/firmware/cmdline.txt')
tokens = cmdline_path.read_text(encoding='utf-8').strip().split()
tokens = [token for token in tokens if token != 'console=tty1']
for token in ('quiet', 'splash', 'loglevel=3', 'logo.nologo',
              'vt.global_cursor_default=0', 'plymouth.ignore-serial-consoles'):
    if token not in tokens:
        tokens.append(token)
cmdline_path.write_text(' '.join(tokens) + '\n', encoding='utf-8')

config_path = Path('/boot/firmware/config.txt')
config = config_path.read_text(encoding='utf-8')
if 'disable_splash=1' not in config.splitlines():
    config += '\n# Zed standalone startup\ndisable_splash=1\n'
    config_path.write_text(config, encoding='utf-8')
PY

plymouth-set-default-theme -R zed-d2
systemctl mask --now getty@tty1.service
if [ -S /run/user/1000/bus ]; then
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user daemon-reload
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user enable --now zed-usb-monitor.service
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user mask --now waybar.service || true
    runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
        systemctl --user mask --now swaync.service || true
fi

echo "Zed boot splash installed. Reboot only after running verify-zed-boot-splash.sh."
