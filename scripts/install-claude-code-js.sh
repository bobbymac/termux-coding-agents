#!/data/data/com.termux/files/usr/bin/sh
# Install the interactive Claude Code on Termux as `claude`.
#
# 2.1.112 is the last release published as plain JavaScript (cli.js). From
# 2.1.113 on, npm ships only glibc/musl native binaries, and their UI needs
# Anthropic-internal Bun APIs, so this pins the last version that runs on
# Termux's own Node.js. The auto-updater is disabled so it can't replace
# itself with a native build that won't run.
set -e
VERSION="${1:-2.1.112}"
DIR="$HOME/.local/share/claude-js"
mkdir -p "$DIR"
cd "$DIR"
npm install --no-audit --no-fund "@anthropic-ai/claude-code@$VERSION"
CLI="$DIR/node_modules/@anthropic-ai/claude-code/cli.js"
[ -f "$CLI" ] || { echo "no cli.js in $VERSION (only <= 2.1.112 ship one)"; exit 1; }
cat > "$PREFIX/bin/claude" <<WRAP
#!$PREFIX/bin/sh
# Claude Code $VERSION on Termux Node - installed by termux-coding-agents
export DISABLE_AUTOUPDATER=1      # the updater would switch to a glibc-only native build
export USE_BUILTIN_RIPGREP=0      # use Termux ripgrep
exec node "$CLI" "\$@"
WRAP
chmod +x "$PREFIX/bin/claude"
claude --version
echo "Done. Run \`claude\` and use /login."
