# termux-coding-agents

Run the **official, current Claude Code CLI natively on Android** in Termux, with the full interactive UI and its built-in auto-updates. No root, no proot, no PC. The same technique runs [oh-my-pi](https://github.com/can1357/oh-my-pi)'s official Linux build, and [pi](https://www.npmjs.com/package/@earendil-works/pi-coding-agent) works with a plain npm install.

| Agent | Status |
|---|---|
| **Claude Code** (latest) | ✅ Official linux-arm64 build: interactive UI, bundled search, auto-updates, `claude update` |
| **pi** | ✅ `npm install -g @earendil-works/pi-coding-agent` |
| **oh-my-pi** | ✅ Official `omp-linux-arm64` build: interactive UI, native addon, `omp update` |

Tested on a Samsung Galaxy Tab S9 FE, Android 16, Termux from Google Play (`googleplay.2026.10.09`), Claude Code 2.1.330 → 2.1.332, oh-my-pi 19.2.1.

## Install Claude Code

In Termux:

```sh
curl -fsSL https://raw.githubusercontent.com/bobbymac/termux-coding-agents/main/install.sh | bash
claude          # then /login
```

Or clone the repo and run `bash install.sh`. It takes about a minute and is safe to re-run. To add oh-my-pi, pass `--omp` (`... | bash -s -- --omp`); `--omp --no-claude` installs only oh-my-pi.

**Updates:** Claude Code updates itself, as on any Linux machine. `claude update` forces a check, and `claude install <version>` switches versions. The `claude` launcher always starts whatever version the official updater selected.

## Why this is hard, and how it works

Anthropic ships Claude Code for Linux as a glibc binary that bundles Anthropic's own build of Bun. The interactive UI depends on native functions that exist only in that build (`Bun.ant.CellSegmenter`), so running the JavaScript on public Bun or Node only gets you `claude -p`. The real binary has to run.

On Android, two things prevent that:

1. **Termux uses bionic, not glibc.** This part is solved: Termux provides an Android-adapted glibc in its `termux-glibc` repository.
2. **Android forbids `execve()` on app files.** Apps targeting Android 10+ (including Termux from Google Play) may not execute files from their own data directory. Termux works around this by starting every program through `/system/bin/linker64`, but glibc's loader can't be started that way (`Could not find a PHDR`). `glibc-runner`, `patchelf` and `proot` all hit this wall.

This repo adds two small programs (in [`glibc-exec/`](glibc-exec)):

- **`gexec`** is a userspace `exec`. It's an ordinary Android program that maps glibc's `ld.so` into memory itself and builds the startup stack the kernel would (`argv`, `envp`, `auxv`). Then it jumps to the loader's entry point, which loads the real program. Android permits this because mapping app files as executable is allowed (that's how apps load their own libraries); only `execve()` is blocked.
- **`gexec-preload.so`** is an `LD_PRELOAD` shim inside the glibc process. When Claude Code starts a program, it routes Termux binaries and scripts through `linker64` (with `termux-exec` restored for them) and glibc binaries back through `gexec`. It also makes `/proc/self/exe` report the real Claude binary, so Claude can relaunch itself (that's how its bundled ripgrep runs). It also replaces `getifaddrs()`: Android 11+ refuses the netlink request glibc uses (`RTM_GETLINK`, which fails with "Permission denied"), and that broke oh-my-pi's sign-in with "getifaddrs returned an error". The shim lists interfaces with `RTM_GETADDR` instead, as Android's own libc does.

`claude` in `$PREFIX/bin` is a short launcher script. It finds the version the official installer linked at `~/.local/bin/claude` and starts it with `gexec`.

## Let Claude Code manage Android

Termux apps can barely touch the system on their own. `settings`, `pm` and `dumpsys` refuse. To let Claude change settings, disable apps or grant permissions, connect ADB to the device itself over **Wireless debugging** (no PC, no root):

1. **Turn on Developer options:** Settings > About tablet > Software information > tap **Build number** 7 times.
2. **Open the pairing screen:** Settings > Developer options > turn on **Wireless debugging** > tap it > **Pair device with pairing code**.
3. **Pair from Termux:** use split screen or a floating window, so the code stays visible.
   ```sh
   adb-local pair <port shown> <6-digit code>
   ```
4. **Check it:** `adb-local status` should report `connected: 127.0.0.1:<port> (shell)`.

After a reboot or toggling Wireless debugging, run `adb-local connect` (Claude can too); it finds the new port itself. Optionally install **Termux:API** from the same store as Termux, for `termux-*` commands (battery, notifications, clipboard, brightness, …).

The installer also puts an Android-aware [`~/.claude/CLAUDE.md`](android/CLAUDE.md) in place. It tells Claude how the environment works and sets ground rules for system changes:
- explain the change and the undo first, and wait for approval;
- log every change with its undo command in `~/android-changes.md`;
- prefer reversible commands (`pm disable-user` over uninstall);
- never touch Termux, Tailscale, Wireless debugging, lock-screen or security settings, or anything that could cut off the session.

## Limitations

- **aarch64 only.** Tested on Android 16; it should work on any recent Android version.
- **No clipboard image paste or voice input.** Those parts expect a desktop Linux session.
- **Some launched programs see a different `argv[0]`.** Termux programs started from Claude see their full path as `argv[0]`, as with `termux-exec`.
- **Only the `claude` launcher works.** Run Claude Code through it, not the raw binary at `~/.local/bin/claude`. The installer appends `~/.local/bin` to the end of `PATH` (that silences the installer's warning), so the launcher always wins.
- **It's unofficial.** It's not supported by Anthropic and could break if a future Android or Claude Code release changes how programs start. `claude doctor` is the quickest health check.

This repo contains **no Anthropic code**. The installer downloads the official package from npm, and Claude Code's own installer and updater manage it from there. Your use of Claude Code is governed by [Anthropic's terms](https://code.claude.com/docs/en/legal-and-compliance).

## pi

```sh
npm install -g @earendil-works/pi-coding-agent
pi              # then /login
```

`@mariozechner/pi-coding-agent` is deprecated and renamed; uninstall it first if you have it.

## oh-my-pi

```sh
curl -fsSL https://raw.githubusercontent.com/bobbymac/termux-coding-agents/main/install.sh | bash -s -- --omp
omp             # then sign in
```

`bun install -g @oh-my-pi/pi-coding-agent` doesn't work on Termux. Android's Bun reports the platform as `android-arm64`, and `@oh-my-pi/pi-natives` (a Rust addon) has no build for it. Compiling it on the device fails too: the project needs nightly Rust, and `rustc` crashes on `cranelift-codegen`.

The installer uses the project's official standalone `omp-linux-arm64` binary instead (Bun runtime and native addon included). It verifies the binary's sha256 from the GitHub release and starts it with `gexec`, like Claude Code.

**Updates:** `omp update` works. omp replaces the first `omp` it finds on `PATH`, so the launcher puts the real binary's directory (`~/.local/share/omp/bin`) first on `PATH`, for the omp process only. Updates then land on the binary and the launcher stays intact. The installer also removes a leftover `~/.bun/bin/omp` link from an earlier `bun install -g`, since that would shadow the launcher.

## License

MIT for everything in this repo. The tools it installs are under their own licenses.
