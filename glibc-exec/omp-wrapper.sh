#!/data/data/com.termux/files/usr/bin/sh
# omp: oh-my-pi's official standalone linux-arm64 binary, started by gexec.
# Installed by termux-coding-agents.
#
# omp's self-updater replaces the first `omp` it finds on PATH, so the real
# binary's directory goes first on PATH for the omp process (only); `omp
# update` then updates the binary instead of overwriting this launcher.
OMP_BIN_DIR="${OMP_BIN_DIR:-$HOME/.local/share/omp/bin}"
[ -f "$OMP_BIN_DIR/omp" ] || { echo "omp: $OMP_BIN_DIR/omp missing. Re-run the termux-coding-agents installer with --omp" >&2; exit 1; }
export PATH="$OMP_BIN_DIR:$PATH"
export GEXEC_GLIBC_LD_PRELOAD="$PREFIX/lib/gexec/gexec-preload.so"
exec "$PREFIX/bin/gexec" -a omp "$OMP_BIN_DIR/omp" "$@"
