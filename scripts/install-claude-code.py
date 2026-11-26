#!/usr/bin/env python3
"""Install the LATEST Claude Code on Termux (Android, aarch64) as `claude-latest`.

Limitation: only non-interactive print mode (`claude-latest -p "..."`) works.
The interactive UI calls Bun.ant.CellSegmenter, a native API that exists only
in Anthropic's internal Bun build, so the full-screen UI never draws. For the
interactive UI use scripts/install-claude-code-js.sh (2.1.112, plain Node).

Anthropic publishes Claude Code as a Bun single-file executable linked against
glibc/musl, which Android's bionic libc cannot load. The JavaScript inside that
executable runs fine on Termux's native Android build of Bun, though. This
script:

  1. downloads the official @anthropic-ai/claude-code-linux-arm64 package
     from npm (on your own device; nothing is redistributed by this repo),
  2. reads the embedded Bun module graph out of the ELF `.bun` section,
  3. writes each module to disk and rewrites the `/$bunfs/root/` paths,
  4. installs a `claude-latest` wrapper in $PREFIX/bin that runs it with `bun`.

Usage:  python3 install-claude-code.py [VERSION]     (default: latest)
Requires: bun, npm, ripgrep (pkg install nodejs ripgrep; bun via bun-termux).
"""
import os
import struct
import subprocess
import sys
import tarfile
import tempfile

PKG = "@anthropic-ai/claude-code-linux-arm64"
TRAILER = b"\n---- Bun! ----\n"
BUNFS = b"/$bunfs/root/"
HOME = os.path.expanduser("~")
PREFIX = os.environ.get("PREFIX", "/data/data/com.termux/files/usr")
INSTALL_ROOT = os.path.join(HOME, ".local", "share", "claude-android")


def sh(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw).stdout.strip()


def elf_section(data, wanted):
    """Return (offset, size) of the named section in a 64-bit little-endian ELF."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit("not a 64-bit little-endian ELF file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)

    def hdr(i):
        return struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)

    strtab_off = hdr(shstrndx)[4]
    for i in range(shnum):
        name_off, _, _, _, off, size, *_ = hdr(i)
        end = data.index(b"\0", strtab_off + name_off)
        if data[strtab_off + name_off:end].decode() == wanted:
            return off, size
    sys.exit(f"section {wanted} not found (Bun executable format changed?)")


def extract(binary, dest):
    data = open(binary, "rb").read()
    off, size = elf_section(data, ".bun")
    length, = struct.unpack_from("<Q", data, off)
    payload = data[off + 8:off + 8 + length]
    t = payload.rfind(TRAILER)
    if t < 0:
        sys.exit("Bun trailer not found")
    # Offsets struct just before the trailer:
    # u64 byte_count, (u32 off, u32 len) modules, u32 entry_point_id, (u32, u32) argv, u32 flags
    _, mod_off, mod_len, entry_id, _, _, _ = struct.unpack_from("<QIIIIII", payload, t - 32)
    table = payload[mod_off:mod_off + mod_len]

    def name_at(rec):
        o, l = struct.unpack_from("<II", rec, 0)
        return payload[o:o + l]

    # The per-module record size differs between Bun versions; find the one
    # where every record starts with a /$bunfs/root/ path.
    for rec_size in range(24, 256, 4):
        if mod_len % rec_size:
            continue
        n = mod_len // rec_size
        if all(name_at(table[i * rec_size:]).startswith(BUNFS) for i in range(n)):
            break
    else:
        sys.exit("could not work out the module table layout")

    os.makedirs(dest, exist_ok=True)
    entry = None
    names = []
    for i in range(n):
        rec = table[i * rec_size:(i + 1) * rec_size]
        name = name_at(rec)[len(BUNFS):].decode()
        c_off, c_len = struct.unpack_from("<II", rec, 8)
        rel = name if i != entry_id else name + ".js"
        if i == entry_id:
            entry = rel
        path = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(payload[c_off:c_off + c_len])
        names.append(rel)
    return entry, names


def rewrite(dest, entry, names):
    """Point /$bunfs/root/ references at the real directory."""
    root = (dest.rstrip("/") + "/").encode()
    entry_ref = BUNFS + entry[:-3].encode()
    for rel in names:
        if not rel.endswith((".js", ".mjs")):
            continue  # leave binary assets untouched
        path = os.path.join(dest, rel)
        src = open(path, "rb").read()
        new = src.replace(b"// @bun @bytecode", b"// @bun", 1)
        new = new.replace(entry_ref + b'"', root + entry.encode() + b'"')
        new = new.replace(BUNFS, root)
        if new != src:
            open(path, "wb").write(new)


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else sh("npm", "view", PKG, "version")
    dest = os.path.join(INSTALL_ROOT, version)
    print(f"Claude Code {version} -> {dest}")
    with tempfile.TemporaryDirectory() as tmp:
        print("  downloading from npm ...")
        tgz = sh("npm", "pack", "--silent", f"{PKG}@{version}", cwd=tmp).splitlines()[-1]
        with tarfile.open(os.path.join(tmp, tgz)) as tar:
            tar.extract("package/claude", tmp, filter="data")
        print("  extracting embedded JavaScript ...")
        entry, names = extract(os.path.join(tmp, "package", "claude"), dest)
    rewrite(dest, entry, names)
    print(f"  {len(names)} modules, entry {entry}")

    wrapper = os.path.join(PREFIX, "bin", "claude-latest")
    with open(wrapper, "w") as f:
        f.write(f"""#!{PREFIX}/bin/sh
# Latest Claude Code via Android Bun (print mode only) - installed by termux-coding-agents
export DISABLE_AUTOUPDATER=1      # the built-in updater fetches glibc binaries
export USE_BUILTIN_RIPGREP=0      # use Termux's ripgrep (pkg install ripgrep)
exec bun "{dest}/{entry}" "$@"
""")
    os.chmod(wrapper, 0o755)
    print("  checking ...", sh(wrapper, "--version"))
    print('Done. Use `claude-latest -p "..."`. Re-run this script to update.')


if __name__ == "__main__":
    main()
