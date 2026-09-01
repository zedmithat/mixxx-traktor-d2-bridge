#!/bin/sh
# Start the custom Mixxx build after the Wayland session and D2 bridge exist.
DISPLAY=${DISPLAY:-:0}
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
export DISPLAY XDG_RUNTIME_DIR WAYLAND_DISPLAY

# The generated image has used both wayland-0 and wayland-1 across releases.
# Recover from a systemd environment import race without adding a fixed delay.
if [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
    for zed_wayland_candidate in wayland-1 wayland-0; do
        if [ -S "$XDG_RUNTIME_DIR/$zed_wayland_candidate" ]; then
            WAYLAND_DISPLAY=$zed_wayland_candidate
            export WAYLAND_DISPLAY
            break
        fi
    done
fi

# The service is started by Sway after systemd receives the session
# environment. A bounded socket wait removes the old fixed five-second blank
# desktop while still surviving a slow first boot.
zed_wait=0
while [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && [ "$zed_wait" -lt 100 ]; do
    sleep 0.1
    zed_wait=$((zed_wait + 1))
done

if [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
    echo "ZED STARTUP ERROR: Wayland socket not ready" >&2
    exit 1
fi

exec /home/pi/mixxx-d2-build/mixxx --resource-path /usr/share/mixxx
