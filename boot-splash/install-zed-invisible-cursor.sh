#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backup_dir=/home/pi/d2-backups/zed-boot-splash
profile_file=/home/pi/.profile
sway_config=/home/pi/.config/sway/config
cursor_theme_dir=/home/pi/.local/share/icons/zed-invisible

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

install -d -m 0755 "$backup_dir" "$cursor_theme_dir/cursors"
if [ ! -e "$backup_dir/profile.before" ]; then
    cp -a "$profile_file" "$backup_dir/profile.before"
fi
if [ ! -e "$backup_dir/sway-config.before" ]; then
    cp -a "$sway_config" "$backup_dir/sway-config.before"
fi

python3 "$script_dir/generate-transparent-xcursor.py" \
    "$cursor_theme_dir/cursors/left_ptr"
for cursor_name in \
        default arrow top_left_arrow pointer hand1 hand2 text xterm crosshair \
        watch wait progress move fleur all-scroll grab grabbing not-allowed \
        no-drop copy alias help context-menu; do
    ln -sfn left_ptr "$cursor_theme_dir/cursors/$cursor_name"
done
cat > "$cursor_theme_dir/index.theme" <<'EOF'
[Icon Theme]
Name=Zed Invisible Cursor
Comment=Transparent cursor for the standalone Zed DJ interface
EOF
chown -R pi:pi "$cursor_theme_dir"

python3 - <<'PY'
from pathlib import Path

profile_path = Path('/home/pi/.profile')
start = '# ZED INVISIBLE CURSOR START'
end = '# ZED INVISIBLE CURSOR END'
text = profile_path.read_text(encoding='utf-8')
while start in text and end in text:
    before, remainder = text.split(start, 1)
    _, after = remainder.split(end, 1)
    text = before.rstrip() + '\n' + after.lstrip('\n')
text = text.rstrip() + f'''\n\n{start}
export XCURSOR_THEME=zed-invisible
export XCURSOR_SIZE=1
{end}\n'''
profile_path.write_text(text, encoding='utf-8')
PY
chown pi:pi "$profile_file"

sed -i '/^seat seat0 hide_cursor /d' "$sway_config"
sed -i '/^seat seat0 xcursor_theme /d' "$sway_config"
cat >> "$sway_config" <<'EOF'

# Zed standalone startup: never expose a pointer during handoff.
seat seat0 xcursor_theme zed-invisible 1
EOF
chown pi:pi "$sway_config"

for sway_socket in /run/user/1000/sway-ipc.*.sock; do
    if [ -S "$sway_socket" ]; then
        runuser -u pi -- env XDG_RUNTIME_DIR=/run/user/1000 \
            swaymsg -s "$sway_socket" reload >/dev/null
        break
    fi
done

echo "ZED_INVISIBLE_CURSOR_INSTALLED_RELOGIN_REQUIRED"
