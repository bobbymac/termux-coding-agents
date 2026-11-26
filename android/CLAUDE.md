# Environment: Claude Code on an Android tablet (Termux)

You are running inside **Termux** (Google Play build) on an Android tablet, without root.
The user wants your help inspecting, configuring and improving this Android system.

## How things run here
- Shell commands run as the Termux app user (`u0_aNN`), with `$PREFIX=/data/data/com.termux/files/usr` and `$HOME=/data/data/com.termux/files/home`.
- Termux packages: `pkg install <name>` (no sudo, no apt-get as root). There is no `/usr`, `/bin` or `/etc` in the usual places.
- Claude Code itself is the official linux-arm64 build started by `gexec`. Don't modify `$PREFIX/bin/gexec`, `$PREFIX/lib/gexec/` or `$PREFIX/bin/claude`, or Claude Code stops starting.
- Shared storage (Downloads, DCIM) is under `~/storage/` once `termux-setup-storage` has been run.

## Three levels of access to Android
1. **Termux app user**: Termux files, network, `getprop`, `/system/bin/*` tools with app permissions. Most `settings`, `pm`, `dumpsys` calls are denied at this level.
2. **Termux:API** (`termux-*` commands: battery, notifications, clipboard, Wi-Fi info, sensors, volume, brightness, toast, etc.). This needs the Termux:API app installed from the same store as Termux. If `termux-battery-status` hangs or fails, tell the user to install it.
3. **ADB shell user (uid 2000)** via Wireless debugging to this device: `adb-local shell <cmd>`. This is the level that can actually change the system: `settings put global|secure|system`, `pm disable-user --user 0`, `pm grant/revoke`, `cmd appops`, `dumpsys`, `cmd package compile`, `wm`, `cmd notification`, `am`, and so on.
   - `adb-local status` / `adb-local connect` (finds the port itself).
   - Not paired yet: ask the user to open Settings > Developer options > Wireless debugging > "Pair device with pairing code", then run `adb-local pair PORT CODE` with the numbers shown.
   - The connection drops when Wireless debugging is switched off, after a reboot, or sometimes when Wi-Fi changes. Reconnect with `adb-local connect`.

## Rules for changing the system
- **Explain before acting.** Before any change to Android settings, apps or permissions, say what you will change, why, and how to undo it. Wait for the user's OK unless they already approved that specific change.
- **Keep a change log.** Add every applied change to `~/android-changes.md` as a table row: date, command, undo command, reason. Read it first when asked what has been changed or to roll something back.
- **Prefer reversible operations.** Use `pm disable-user --user 0 <pkg>` (undo: `pm enable <pkg>`) rather than `pm uninstall --user 0` (undo: `cmd package install-existing <pkg>`). Record the old value before `settings put` (undo with the old value, or `settings delete` if it was `null`).
- **Never** disable or remove: `com.termux*`, Tailscale (`com.tailscale.ipn`), Settings, the launcher in use, the keyboard in use, Google Play services, system UI, the package installer, or anything whose role you aren't sure of. Look it up first (`dumpsys package <pkg>`, `cmd package resolve-activity`).
- **Never** turn off Wireless debugging, Developer options or Wi-Fi from a command, or touch lock-screen or security settings, without explicit instruction. That can cut off this session.
- Never factory reset, wipe data, flash, or change bootloader or OEM unlock settings.
