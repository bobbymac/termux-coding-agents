#!/data/data/com.termux/files/usr/bin/bash
# Install the official Claude Code (latest, auto-updating, full UI) on Termux.
#
#   curl -fsSL https://raw.githubusercontent.com/bobbymac/termux-coding-agents/main/install.sh | bash
#   or, from a clone:  bash install.sh
#
# What it does:
#   1. installs Termux's glibc (from the termux-glibc repo) plus build tools
#   2. builds gexec (userspace exec for glibc programs) and gexec-preload.so
#   3. installs the `claude` launcher and runs the official `claude install`
#   4. installs adb-local and an Android-aware ~/.claude/CLAUDE.md
# Re-running it is safe; it rebuilds gexec and leaves your Claude data alone.
set -euo pipefail

REPO_RAW="https://raw.githubusercontent.com/bobbymac/termux-coding-agents/main"
OFFICIAL="https://packages.termux.dev/apt/termux-main"
: "${PREFIX:?run this inside Termux}"
[ "$(uname -m)" = aarch64 ] || { echo "Only aarch64 devices are supported." >&2; exit 1; }

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# Work from a clone if we're in one, otherwise download the sources.
SRC=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)
if [ -z "$SRC" ] || [ ! -f "$SRC/glibc-exec/gexec.c" ]; then
	SRC=$(mktemp -d "$PREFIX/tmp/tca.XXXXXX")
	trap 'rm -rf "$SRC"' EXIT
	mkdir -p "$SRC/glibc-exec" "$SRC/android"
	for f in glibc-exec/gexec.c glibc-exec/gexec-preload.c glibc-exec/claude-wrapper.sh android/adb-local android/CLAUDE.md; do
		curl -fsSL "$REPO_RAW/$f" -o "$SRC/$f"
	done
fi

deb_from_official() { # install one package straight from the official Termux repo
	local pkg=$1 file tmp
	file=$(curl -fsSL "$OFFICIAL/dists/stable/main/binary-aarch64/Packages" |
		awk -v p="$pkg" '$0 == "Package: " p { f = 1 } f && /^Filename:/ { print $2; exit }')
	[ -n "$file" ] || { echo "can't find $pkg in the official repo" >&2; return 1; }
	tmp="$PREFIX/tmp/$(basename "$file")"
	curl -fsSL "$OFFICIAL/$file" -o "$tmp" && dpkg -i "$tmp" && rm -f "$tmp"
}

say "Installing build tools"
pkg install -y clang curl coreutils >/dev/null

say "Adding the Termux glibc repository"
if ! dpkg -s glibc-repo >/dev/null 2>&1; then
	# Some mirrors (e.g. termux.net) don't carry glibc-repo.
	pkg install -y glibc-repo >/dev/null 2>&1 || deb_from_official glibc-repo
fi
if apt update 2>&1 | grep -A1 termux-glibc | grep -q NO_PUBKEY; then
	# termux-keyring 3.15 dropped the key that signs the glibc repo; pin it from 3.14.
	echo "glibc repo key missing; pinning it from termux-keyring 3.14"
	tmp=$(mktemp -d)
	curl -fsSL "$OFFICIAL/pool/main/t/termux-keyring/termux-keyring_3.14_all.deb" -o "$tmp/k.deb"
	dpkg-deb -x "$tmp/k.deb" "$tmp/x"
	mkdir -p "$PREFIX/etc/apt/keyrings"
	cp "$tmp/x$PREFIX/share/termux-keyring/termux-autobuilds.gpg" "$PREFIX/etc/apt/keyrings/termux-glibc.gpg"
	rm -rf "$tmp"
	printf '# Termux glibc repo, pinned to its signing key\ndeb [signed-by=%s] https://packages-cf.termux.dev/apt/termux-glibc/ glibc stable\n' \
		"$PREFIX/etc/apt/keyrings/termux-glibc.gpg" > "$PREFIX/etc/apt/sources.list.d/glibc.list"
	apt update >/dev/null 2>&1
fi
pkg install -y glibc >/dev/null
G="$PREFIX/glibc"
[ -x "$G/lib/ld-linux-aarch64.so.1" ] || { echo "glibc install failed" >&2; exit 1; }

say "Building gexec and its glibc shim"
clang -O2 -Wall -o "$PREFIX/bin/gexec" "$SRC/glibc-exec/gexec.c"
mkdir -p "$PREFIX/lib/gexec"
clang --target=aarch64-unknown-linux-gnu -nostdinc -isystem "$(clang -print-resource-dir)/include" \
	-isystem "$G/include" -fPIC -shared -nostdlib -O2 -Wno-pointer-bool-conversion -fuse-ld=lld \
	-o "$PREFIX/lib/gexec/gexec-preload.so" "$SRC/glibc-exec/gexec-preload.c" \
	"$G/lib/libc.so.6" "$G/lib/ld-linux-aarch64.so.1"
GEXEC_GLIBC_LD_PRELOAD="$PREFIX/lib/gexec/gexec-preload.so" gexec "$G/bin/uname" -m >/dev/null ||
	{ echo "gexec self-test failed" >&2; exit 1; }

say "Installing the claude launcher"
install -m 755 "$SRC/glibc-exec/claude-wrapper.sh" "$PREFIX/bin/claude"
hash -r

if ! claude --version >/dev/null 2>&1; then
	say "Downloading Claude Code (official linux-arm64 build)"
	pkg install -y nodejs >/dev/null
	ver=$(npm view @anthropic-ai/claude-code-linux-arm64 version)
	tmp=$(mktemp -d "$PREFIX/tmp/cc.XXXXXX")
	(cd "$tmp" && npm pack --silent "@anthropic-ai/claude-code-linux-arm64@$ver" >/dev/null && tar xzf ./*.tgz)
	mkdir -p "$HOME/.local/share/claude/versions"
	bootstrap="$HOME/.local/share/claude/versions/$ver"
	mv "$tmp/package/claude" "$bootstrap"
	rm -rf "$tmp"
	say "Running the official installer"
	GEXEC_GLIBC_LD_PRELOAD="$PREFIX/lib/gexec/gexec-preload.so" gexec -a claude "$bootstrap" install
fi

if ! grep -q 'HOME/.local/bin' "$HOME/.bashrc" 2>/dev/null; then
	printf '\n# Claude Code native install (after $PREFIX/bin so the Termux launcher wins)\nexport PATH="$PATH:$HOME/.local/bin"\n' >> "$HOME/.bashrc"
fi

say "Android access helpers"
pkg install -y android-tools termux-api nmap >/dev/null
install -m 755 "$SRC/android/adb-local" "$PREFIX/bin/adb-local"
mkdir -p "$HOME/.claude"
if [ -e "$HOME/.claude/CLAUDE.md" ] && ! cmp -s "$SRC/android/CLAUDE.md" "$HOME/.claude/CLAUDE.md"; then
	cp "$SRC/android/CLAUDE.md" "$HOME/.claude/CLAUDE.android.md"
	echo "Kept your ~/.claude/CLAUDE.md; the Android guide is in ~/.claude/CLAUDE.android.md"
else
	cp "$SRC/android/CLAUDE.md" "$HOME/.claude/CLAUDE.md"
fi

say "Done: $(claude --version)"
cat <<'EOF'
Next:
  claude                      start, then /login
  adb-local pair PORT CODE    let Claude change Android settings (see README)
Updates install themselves; `claude update` forces a check.
EOF
