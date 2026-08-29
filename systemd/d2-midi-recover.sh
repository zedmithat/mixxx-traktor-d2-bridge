#!/bin/sh
# PortMidi keeps subscriptions to the ALSA client instance that existed when
# its controller engine opened. If the bridge is restarted, that client
# vanishes and Mixxx must reopen the controller against the new instance.
sleep 1

mixxx_pid="$(pgrep -n -f '^/home/pi/mixxx-d2-build/mixxx' 2>/dev/null || true)"
if [ -z "$mixxx_pid" ]; then
    exit 0
fi

kill -TERM "$mixxx_pid" 2>/dev/null || true
for ignored in 1 2 3 4 5; do
    if ! kill -0 "$mixxx_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done

exec systemctl --user restart 'app-mixxx\x2dd2\x2dcustom@autostart.service'
