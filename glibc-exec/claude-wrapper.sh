#!/data/data/com.termux/files/usr/bin/sh
# claude: run the official Claude Code (linux-arm64 glibc build) on Android/Termux.
# Installed by termux-coding-agents. The real binaries live where the official
# installer and updater put them; gexec starts them despite Android's W^X rule.
CLAUDE_DATA="${CLAUDE_DATA_DIR:-$HOME/.local/share/claude}"
LINK="$HOME/.local/bin/claude"

bin=""
[ -e "$LINK" ] && bin=$(realpath "$LINK" 2>/dev/null)
if [ -z "$bin" ] || [ ! -f "$bin" ]; then
	newest=$(ls -1 "$CLAUDE_DATA/versions" 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1)
	[ -n "$newest" ] && bin="$CLAUDE_DATA/versions/$newest"
fi
if [ -z "$bin" ] || [ ! -f "$bin" ]; then
	echo "claude: no Claude Code binary found. Run: claude-termux-setup" >&2
	exit 1
fi

export GEXEC_GLIBC_LD_PRELOAD="$PREFIX/lib/gexec/gexec-preload.so"
exec "$PREFIX/bin/gexec" -a claude "$bin" "$@"
