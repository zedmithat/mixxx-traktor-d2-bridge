#!/bin/sh
# Start the custom Mixxx build after the desktop session and D2 bridge exist.
sleep 5
DISPLAY=${DISPLAY:-:0}
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
if [ -z "$WAYLAND_DISPLAY" ] && [ -S "$XDG_RUNTIME_DIR/wayland-1" ]; then
    WAYLAND_DISPLAY=wayland-1
fi
export DISPLAY XDG_RUNTIME_DIR WAYLAND_DISPLAY
exec /home/pi/mixxx-d2-build/mixxx --resource-path /usr/share/mixxx
