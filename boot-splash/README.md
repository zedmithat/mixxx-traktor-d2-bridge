# Zed standalone boot splash

This package creates one continuous startup path:

1. Raspberry Pi firmware splash and console noise are hidden.
2. Plymouth reveals the Zed `z-e-d` mark while the system boots.
3. `OPEN DJ SYSTEM` and `BUILT FOR DJS. NOT SHAREHOLDERS.` complete the
   startup identity.
4. Sway uses the exact final Plymouth frame as its background.
5. One systemd user service starts the D2 bridge before Mixxx.
6. Mixxx's native Zed launch animation hands off to the Player screen.

The installer also suppresses Waybar and shortens the cursor-hide delay so no
desktop chrome flashes between Plymouth and the full-screen Mixxx session.
LightDM is pinned to Plymouth's VT1 so it retains the final splash frame while
Sway takes ownership of the display; this removes the brief black VT switch.
The local tty1 kernel console/getty is disabled so no terminal frame can leak
through the handoff; the serial console and SSH recovery paths remain intact.
The package also installs a one-pixel transparent XCursor theme before Sway is
started. This avoids Sway's unavoidable 100 ms minimum cursor-hide delay, so a
pointer cannot flash during the Plymouth-to-Mixxx handoff.

On Raspberry Pi 5 systems configured with `NET_INSTALL_AT_POWER_ON=1`, the
bootloader deliberately displays its network-install screen before Linux can
start Plymouth. After reviewing the saved EEPROM configuration, stage the
supported on-demand setting with:

```sh
sudo ./boot-splash/disable-network-install-ui.sh
```

This preserves the installed bootloader version and keeps the recovery UI
available when SHIFT is held or a boot error occurs. It only removes the forced
power-on countdown and takes effect after reboot.

The installer backs up every modified boot, Sway and autostart file under
`/home/pi/d2-backups/zed-boot-splash`. Run the verifier before rebooting:

```sh
sudo ./boot-splash/install-zed-boot-splash.sh
sudo ./boot-splash/verify-zed-boot-splash.sh
```

Rollback, if the first boot does not pass verification:

```sh
sudo ./boot-splash/restore-zed-boot-splash.sh
```

Do not reboot if verification fails. The first reboot should be performed
with SSH reachable or a keyboard attached so the backup can be restored.
