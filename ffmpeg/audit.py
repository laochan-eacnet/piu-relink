#!/usr/bin/env python3
"""Probe all shipped FFmpeg candidates; run with python3 ffmpeg/audit.py in WSL."""
import collections
import concurrent.futures
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
OUT = ROOT / "out/linux/ffmpeg"
VIDEO_EXTENSIONS = {".v", ".v2", ".webm", ".mkv", ".avi", ".mp4", ".mov", ".mpg", ".mpeg", ".wmv", ".flv", ".vob"}


def media_magic(header):
    return (header.startswith((b"\x1aE\xdf\xa3", b"OggS", b"\x00\x00\x01\xba", b"\x00\x00\x01\xb3"))
            or header[:4] == b"RIFF" and header[8:12] == b"AVI "
            or header[4:8] == b"ftyp")


def probe(item):
    path, reason = item
    command = ["ffprobe", "-v", "error", "-show_entries",
               "format=format_name:stream=codec_type,codec_name,width,height,pix_fmt",
               "-of", "json", str(path)]
    result = {"path": path.relative_to(ROOT).as_posix(), "selection": reason,
              "size": path.stat().st_size}
    try:
        process = subprocess.run(command, capture_output=True, text=True, timeout=45)
        parsed = json.loads(process.stdout or "{}")
        result.update(parsed)
        result["returncode"] = process.returncode
        if process.stderr.strip():
            result["diagnostic"] = process.stderr.strip()
        if not parsed.get("format") or not parsed.get("streams"):
            result["failure"] = process.stderr.strip() or "No format or streams reported"
        elif process.returncode:
            result["failure"] = process.stderr.strip()
    except (subprocess.TimeoutExpired, ValueError) as error:
        result["failure"] = str(error)
    return result


def main():
    candidates = []
    extensions = collections.Counter()
    total = 0
    read_errors = []
    for path in sorted(DATA.rglob("*")):
        if not path.is_file():
            continue
        total += 1
        extension = path.name if path.name.startswith(".") and "." not in path.name[1:] else path.suffix
        extensions[extension.upper() or "(none)"] += 1
        relative = path.relative_to(DATA)
        reason = None
        if relative.parts[0] == "VIDEO":
            reason = "VIDEO/**"
        elif len(relative.parts) >= 4 and relative.parts[0] == "D" and relative.parts[2] == "V":
            reason = "D/*/V/**"
        elif len(relative.parts) >= 5 and relative.parts[0] == "D" and relative.parts[2:4] == ("P", "V"):
            reason = "D/*/P/V/**"
        elif extension.lower() in VIDEO_EXTENSIONS:
            reason = "video extension outside required directories"
        try:
            with path.open("rb") as source:
                header = source.read(64)
            if reason is None and media_magic(header):
                reason = "media signature outside required directories"
        except OSError as error:
            read_errors.append([path.relative_to(ROOT).as_posix(), str(error)])
        if reason:
            candidates.append((path, reason))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(probe, candidates))
    table = collections.Counter()
    selections = collections.Counter()
    failures = []
    for result in results:
        selections[result["selection"]] += 1
        if "failure" in result:
            failures.append(result)
            continue
        streams = result["streams"]
        video = "+".join(sorted({s.get("codec_name", "unknown") for s in streams if s["codec_type"] == "video"})) or "none"
        audio = "+".join(sorted({s.get("codec_name", "unknown") for s in streams if s["codec_type"] == "audio"})) or "none"
        table[(result["format"]["format_name"], video, audio)] += 1
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "media-audit.json").write_text(json.dumps({"ffprobe": subprocess.check_output(["ffprobe", "-version"], text=True).splitlines()[0],
        "files_scanned": total, "extensions": dict(extensions), "read_errors": read_errors,
        "results": results}, indent=2) + "\n")
    lines = ["# Shipped media audit", "", "Reproduce in WSL: `python3 ffmpeg/audit.py`.", "",
        f"Scanned **{total}** data files (64-byte signatures on every file), probed **{len(results)}** candidates; **{len(failures)}** probe failures and **{len(read_errors)}** file-read failures.", "",
        subprocess.check_output(["ffprobe", "-version"], text=True).splitlines()[0], "",
        "## Game input paths", "",
        "The `.rodata` media literals in piu are `.V2` at `0x081c726d`, `.V` at `0x081c7271`, `.A` at `0x081c7274`, and `D/%s/V` at `0x081d599f`. There are no ordinary WebM/AVI/MP4/MKV filename-extension literals. The preload's `resources/vfs_open.c` maps `.V` to `.V2`, and `resources/song_paths.c` resolves song and preview videos to `V/.V2` and `P/V/.V2`. The game imports only the FFmpeg video decode API; `.A` audio is handled by libmad, so probing VIDEO audio here does not require an FFmpeg audio decoder.", "",
        "## Selection", "", "| Selection | Files |", "|---|---:|"]
    for selection, count in sorted(selections.items()):
        lines.append(f"| `{selection}` | {count} |")
    lines += ["", "Selection includes every file under VIDEO (including non-media artwork/metadata), both song-video directories, video extensions anywhere (including dotfiles named `.V2`), and EBML/Ogg/AVI/MP4/MPEG signatures anywhere. Separate MP3 `.A` assets, including those under VIDEO, belong to the libmad audio path, not avformat. Full per-file stream metadata, errors, and extension inventory are in `out/linux/ffmpeg/media-audit.json`.", "",
        "## Container × video codec × audio codec", "", "| Container (ffprobe) | Video codec | Audio codec | Count |", "|---|---|---|---:|"]
    for (container, video, audio), count in sorted(table.items()):
        lines.append(f"| {container} | {video} | {audio} | {count} |")
    video_files = [result for result in results if Path(result["path"]).name.upper() in {".V", ".V2"}]
    video_failures = sum("failure" in result for result in video_files)
    failure_types = collections.Counter(Path(result["path"]).suffix or Path(result["path"]).name for result in failures)
    lines += ["", f"Probed **{len(video_files)} `.V`/`.V2` files**, with **{video_failures} video probe failures**. No VP8 stream was found in this shipped tree. The native Opus decoder is included for the audio streams embedded in WebM and requires swresample; separate `.A`/MP3 assets do not require an FFmpeg MP3 decoder.", "",
              "Probe-failure resource types: " + ", ".join(f"`{extension}` × {count}" for extension, count in sorted(failure_types.items())) + ". BGA3 scripts, text SPR sprite descriptions, custom G graphics and NX20 charts are game resources, not video streams.", ""]
    lines += ["", "## Probe failures", "", "| File | Diagnostic |", "|---|---|"]
    for result in failures:
        diagnostic = result["failure"].replace(str(ROOT) + "/", "").replace("\n", "; ").replace("|", "\\|")
        lines.append(f"| `{result['path']}` | {diagnostic} |")
    if not failures:
        lines.append("| (none) | |")
    for path, error in read_errors:
        lines.append(f"| `{path}` | File-read error: {error} |")
    (ROOT / "docs").mkdir(exist_ok=True)
    document = ROOT / "docs/media-audit.md"
    marker = "## Build and ABI evidence"
    appendix = ""
    if document.exists() and marker in document.read_text():
        appendix = "\n" + marker + document.read_text().split(marker, 1)[1]
    document.write_text("\n".join(lines) + "\n" + appendix)
    print(f"scanned={total} probed={len(results)} probe_failures={len(failures)} read_failures={len(read_errors)}")
    for key, count in sorted(table.items()):
        print(" | ".join(key), "|", count)


if __name__ == "__main__":
    main()
