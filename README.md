# termux-coding-agents

Run AI coding agents **natively on Android in Termux**, with no proot, no chroot and no glibc layer:

| Agent | Status | How |
|---|---|---|
| [pi](https://www.npmjs.com/package/@earendil-works/pi-coding-agent) | ✅ Works | Plain `npm install` (no native code needed) |
| [Claude Code](https://code.claude.com) | ✅ Works | `scripts/install-claude-code.py` runs the official release on Termux's Android build of Bun |
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

```sh
python3 scripts/install-claude-code.py          # latest
python3 scripts/install-claude-code.py 2.1.332  # a specific version
claude        # then /login
```

### How it works

Claude Code ships as a [Bun single-file executable](https://bun.sh/docs/bundler/executables). The program is still JavaScript, stored as a module graph in the ELF `.bun` section. The script:

1. downloads the official `@anthropic-ai/claude-code-linux-arm64` package from npm on your device,
2. parses the `.bun` section and writes each embedded module to `~/.local/share/claude-android/<version>/`,
3. rewrites the internal `/$bunfs/root/` paths to that directory,
4. installs a `claude` wrapper in `$PREFIX/bin` that runs the entry point with your Android `bun`.

This repo contains **no Anthropic code**. Everything is fetched from npm and extracted locally. Your use of Claude Code is still governed by [Anthropic's terms](https://code.claude.com/docs/en/legal-and-compliance). This is an unofficial workaround, not supported by Anthropic.

### Limitations

- **No auto-update.** The built-in updater would download a glibc binary, so the wrapper sets `DISABLE_AUTOUPDATER=1`. Re-run the script to update.
- **Clipboard image paste and voice input don't work.** Those use bundled glibc `.node` addons that can't load on bionic. Everything else tested so far works.
- **Search uses Termux's `ripgrep`** (`USE_BUILTIN_RIPGREP=0`) instead of the bundled one.
- **The extractor may need updating.** It relies on Bun's internal executable format. If a future release changes it, the script exits with a clear error rather than installing something broken.

## oh-my-pi (work in progress)

`omp` installs with `bun install -g`, but it fails at startup because `@oh-my-pi/pi-natives` has no `android-arm64` build. The current attempt compiles it from source on the device:

```sh
pkg install rust clang make pkg-config binutils
git clone --depth 1 --branch v19.2.1 https://github.com/can1357/oh-my-pi.git
cd oh-my-pi && rm -f rust-toolchain.toml
RUSTC_BOOTSTRAP=1 cargo build --release -j4 --ignore-rust-version -p omp-natives
```

The project pins a nightly toolchain (rustc ≥ 1.101), and Termux ships stable Rust, which is why the build needs those flags. Desktop-only crates (clipboard, X11/Wayland, voice) may also need patching for Android. This section will be updated with the result. Use `-j4` or lower on devices with less than 8 GB of RAM.

## License

MIT for the scripts and docs in this repo. The tools themselves are under their own licenses.
