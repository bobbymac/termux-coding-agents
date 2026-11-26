# termux-coding-agents

Run AI coding agents **natively on Android in Termux**, with no proot, no chroot and no glibc layer:

| Agent | Status | How |
|---|---|---|
| [pi](https://www.npmjs.com/package/@earendil-works/pi-coding-agent) | ✅ Works | Plain `npm install` (no native code needed) |
| [Claude Code](https://code.claude.com) | ✅ Works (2.1.112) | `scripts/install-claude-code-js.sh`: last pure-JS release, full interactive UI on Termux Node |
| Claude Code latest | ⚠️ Print mode only | `scripts/install-claude-code.py`: current release on Android Bun, `claude-latest -p "..."` |
| [oh-my-pi](https://github.com/can1357/oh-my-pi) | 🔨 In progress | Needs its Rust `pi-natives` addon compiled for Android |

Tested on a Samsung Galaxy Tab S9 FE (Android 16, aarch64), Termux from F-Droid, Node 26, Bun 1.4.5, Rust 1.96.

## Why the usual installs fail

Termux runs on Android's **bionic** libc. Most prebuilt Linux binaries on npm are linked against **glibc** or **musl**, and `npm` reports Android as `android-arm64`, a platform most packages don't publish. So you see errors like:

```
error: Unsupported platform: android-arm64
Supported platforms: linux-x64, linux-arm64, darwin-x64, ...
```

```
Either postinstall did not run (--ignore-scripts, some pnpm configs)
or the platform-native optional dependency was not downloaded
```

Python tools such as `uv` don't help here: these are Node/Bun native addons and executables, not Python libraries. The fixes are to use a pure-JS path, to run the JavaScript on a native Android runtime, or to recompile the native code for Android.

## Prerequisites

```sh
pkg update
pkg install nodejs git python ripgrep
```

You also need **Bun built for Android** at `~/.bun/bin/bun` (`bun --version` should work, and `file $(command -v bun)` should mention `/system/bin/linker64`). The official `bun.sh` installer ships glibc builds that won't run.

## pi

```sh
npm install -g @earendil-works/pi-coding-agent
pi            # then /login
```

`@mariozechner/pi-coding-agent` is deprecated and renamed to `@earendil-works/pi-coding-agent`. Uninstall the old one first if you have it.

## Claude Code

### Interactive: `claude` (2.1.112)

```sh
sh scripts/install-claude-code-js.sh
claude        # then /login
```

2.1.112 (April 2026) is the **last release published as plain JavaScript** (`cli.js`). It runs on Termux's own Node.js, with the full interactive UI. From 2.1.113 on, npm only ships glibc/musl native binaries. The script pins that version and turns off the auto-updater, which would otherwise replace it with a build that can't run. It's an old version, so newer features and models may be missing, and Anthropic could at some point stop accepting old clients.

### Latest, print mode only: `claude-latest`

```sh
python3 scripts/install-claude-code.py          # latest
claude-latest -p "explain this repo"
```

Current releases ship as a [Bun single-file executable](https://bun.sh/docs/bundler/executables). The program inside is still JavaScript, stored as a module graph in the ELF `.bun` section. The script:

1. downloads the official `@anthropic-ai/claude-code-linux-arm64` package from npm on your device,
2. parses the `.bun` section and writes each embedded module to `~/.local/share/claude-android/<version>/`,
3. rewrites the internal `/$bunfs/root/` paths to that directory,
4. installs a `claude-latest` wrapper that runs the entry point with your Android `bun`.

**Why the interactive UI doesn't work:** the terminal renderer calls `Bun.ant.CellSegmenter`, a native text-layout API that exists only in Anthropic's internal Bun build (`This runtime has no Bun.ant.CellSegmenter`). Public Bun doesn't have it, so every frame throws and the screen stays blank, which looks like a hang. Non-interactive `-p` mode never draws the UI, so it works. A JavaScript stand-in would mean reverse-engineering an undocumented binary interface that changes between releases, so this repo doesn't attempt it.

Other limits of `claude-latest`: no auto-update (re-run the script), no clipboard image paste or voice (those are glibc `.node` addons), and search uses Termux `ripgrep`.

This repo contains **no Anthropic code**. Both scripts fetch the official packages from npm on your device. Your use of Claude Code is governed by [Anthropic's terms](https://code.claude.com/docs/en/legal-and-compliance). These are unofficial workarounds, not supported by Anthropic.

## oh-my-pi (work in progress)

`omp` installs with `bun install -g`, but it fails at startup because `@oh-my-pi/pi-natives` has no `android-arm64` build. The current attempt compiles it from source on the device:

```sh
pkg install rust clang make pkg-config binutils
git clone --depth 1 --branch v19.2.1 https://github.com/can1357/oh-my-pi.git
cd oh-my-pi && rm -f rust-toolchain.toml
RUSTC_BOOTSTRAP=1 cargo build --release -j4 --ignore-rust-version -p omp-natives
```

The project pins a nightly toolchain (rustc ≥ 1.101), and Termux ships stable Rust, which is why the build needs those flags. On a Galaxy Tab S9 FE, about 236 crates compile, then rustc crashes with a stack overflow parsing `cranelift-codegen` (pulled in for WASM support). Raising `RUST_MIN_STACK` didn't help. The next things to try are cross-compiling from a Linux PC with the Android NDK, or building without the WASM feature. Turn off fat LTO on low-RAM devices (`CARGO_PROFILE_RELEASE_LTO=off`).

## License

MIT for the scripts and docs in this repo. The tools themselves are under their own licenses.
