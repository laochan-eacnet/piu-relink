#!/usr/bin/env python3
"""Static acceptance checks for the rebuilt ELF libraries; run in WSL."""
from pathlib import Path
import hashlib
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "out/linux/ffmpeg"
LIBRARIES = {
    "libavcodec.so.58": "LIBAVCODEC_58",
    "libavformat.so.58": "LIBAVFORMAT_58",
    "libavutil.so.56": "LIBAVUTIL_56",
    "libswscale.so.5": "LIBSWSCALE_5",
    "libswresample.so.3": "LIBSWRESAMPLE_3",
}
ALLOWED = set(LIBRARIES) | {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2"}


def run(*args):
    return subprocess.check_output(args, text=True)


def main():
    lines = []
    all_exports = set()
    for soname, symbol_version in LIBRARIES.items():
        path = OUTPUT / soname
        assert path.is_file() and not path.is_symlink(), path
        header = run("readelf", "-h", str(path))
        assert "ELF32" in header and "Intel 80386" in header, soname
        dynamic = run("readelf", "-d", str(path))
        assert re.search(r"\(SONAME\).*\[" + re.escape(soname) + r"\]", dynamic), soname
        needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        assert set(needed) <= ALLOWED, (soname, needed)
        assert "TEXTREL" not in dynamic, soname
        relocations = run("readelf", "-rW", str(path))
        tls_count = len(re.findall(r"R_386_\S*TLS\S*", relocations))
        assert tls_count == 0, soname
        segments = run("readelf", "-lW", str(path))
        assert not re.search(r"^\s*TLS\s", segments, re.MULTILINE), soname
        code = run("i686-linux-gnu-objdump", "-d", str(path))
        gs_count = len(re.findall(r"%gs\b", code))
        assert gs_count == 0, soname
        versions = run("readelf", "-V", str(path))
        dynsyms = run("readelf", "--dyn-syms", "--wide", str(path))
        glibc = {tuple(map(int, value.split("."))) for value in re.findall(r"\bGLIBC_([0-9.]+)", versions + dynsyms)}
        assert all(version <= (2, 31) for version in glibc), (soname, glibc)
        exports = run("i686-linux-gnu-nm", "-D", "--defined-only", str(path))
        assert "@@" + symbol_version in exports, (soname, symbol_version)
        for line in exports.splitlines():
            symbol = line.split()[-1]
            if "@@" in symbol:
                name, version = symbol.split("@@", 1)
                all_exports.add((name, version))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        aliases = sorted(OUTPUT.glob(soname.rsplit(".", 1)[0] + "*"))
        for alias in aliases:
            assert not alias.is_symlink() and hashlib.sha256(alias.read_bytes()).hexdigest() == digest, alias
        max_glibc = ".".join(map(str, max(glibc))) if glibc else "none"
        lines.append(f"{soname}: ELF32 i386; SONAME={soname}; symbol_version={symbol_version}; max_glibc=GLIBC_{max_glibc}")
        lines.append("  NEEDED=" + ",".join(needed))
        lines.append(f"  gs_references={gs_count} TLS_relocations={tls_count} PT_TLS=0 TEXTREL=0 aliases={len(aliases)} real identical files")
    requested = []
    for line in (ROOT / "imports.tsv").read_text().splitlines():
        columns = line.split("\t")
        if len(columns) >= 3 and columns[0] == "piu" and columns[2].startswith(("LIBAV", "LIBSWSCALE")):
            requested.append((columns[1], columns[2]))
    missing = set(requested) - all_exports
    assert not missing, missing
    lines.append(f"piu FFmpeg imports: {len(requested)}/{len(requested)} defined at the exact requested symbol version; missing=0")
    for name, version in sorted(requested):
        lines.append(f"  {name}@{version}")
    lines.append("PASS: all rebuilt libraries meet the relinker ELF constraints")
    text = "\n".join(lines) + "\n"
    (OUTPUT / "verification.txt").write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
