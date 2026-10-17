#!/usr/bin/env python3
"""Convert Pump It Up Phoenix NX20 charts to and from StepMania .ssc.

Thin CLI over StepNX Studio (https://github.com/Autumnal-nn/stepnx-studio),
which owns the NX20 codec and the SSC projections. This script adds only the
Phoenix-specific parts: song-folder discovery, audio copy, Phoenix header
metadata / step-artist trailer, Phoenix chart file naming, LM.NX, and an
optional plain-StepMania (SM5/ITGmania/OutFox) note flattening.

    python tools/nxssc.py nx2ssc data/D/11030 -o out/conrad/11030.ssc --title "..." --artist "..."
    python tools/nxssc.py ssc2nx song.ssc -o newsong/ --step-artist NAME

StepNX is looked up in $STEPNX_SRC, then ext/stepnx-studio/src, then the
installed `stepnx` package.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
for candidate in (os.environ.get("STEPNX_SRC"), ROOT / "ext/stepnx-studio/src"):
    if candidate and Path(candidate, "stepnx").is_dir():
        sys.path.insert(0, str(candidate))
        break

try:
    from stepnx.codecs import nx20
    from stepnx.exporters import (
        SscExportError, SscSongInfo, compile_ssc_export, render_compiled_reports,
    )
    from stepnx.importers import ssc as ssc_import
    from stepnx.workspace.folder import create_blank_lightmap
except ImportError as exc:  # pragma: no cover - environment guard
    raise SystemExit(
        f"StepNX Studio not found ({exc}). Clone it:\n"
        "  git clone https://github.com/Autumnal-nn/stepnx-studio ext/stepnx-studio\n"
        "or set STEPNX_SRC to its src/ directory."
    )

PROFILE = "prime2"  # closest StepNX engine profile to Phoenix (Fiesta+ Header 1000..1008)

# Phoenix header metadata (StepNX docs/NX20_HEADER_1000_1008.md).
H_SECTION, H_LEVEL, H_STEP_ARTIST = 1000, 1001, 1008
SECTION_ARCADE = 1

STEPS_PREFIX = {"pump-single": "S", "pump-double": "D"}

# XSanity brace cell: {<head><bank><layer>}; layer chars of the fake note class.
_BRACE = re.compile(r"\{([^}]*)\}")
_FAKE_LAYERS = frozenset("9a8wqt")


def _warn(message: str) -> None:
    print(message, file=sys.stderr)


# --------------------------------------------------------------------- NX -> SSC

def _chart_paths(inputs: list[Path]) -> list[Path]:
    paths: list[Path] = []
    for item in inputs:
        if item.is_dir():
            # Accept data/D/<ID> as well as data/D/<ID>/<ID>.
            inner = item / item.name
            folder = inner if inner.is_dir() else item
            paths.extend(sorted(p for p in folder.iterdir()
                                if p.is_file() and p.suffix.upper() == ".NX"))
        else:
            paths.append(item)
    return [p for p in paths if p.name.upper() != "LM.NX"]


def _song_audio(paths: list[Path]) -> Path | None:
    """Phoenix full-song audio D/<ID>/A/.A is a plain MP3 stream."""
    for chart in paths:
        audio = chart.parent.parent / "A" / ".A"
        if audio.is_file():
            return audio
    return None


def _flatten_sm5(text: str, counts: Counter) -> str:
    """Rewrite XSanity brace cells into one-character SM5 notes.

    Normal/bonus notes keep their head (1/2/3/4/M); fake taps become F; fake
    holds, items and Brain Shower cells have no SM5 note and are dropped.
    Only #NOTES bodies are touched.
    """

    def cell(match: re.Match) -> str:
        body = match.group(1)
        head, layer = body[:1], body[-1:]
        if len(body) == 3 and layer in _FAKE_LAYERS:
            if head == "1":
                counts["fake tap -> F"] += 1
                return "F"
            counts[f"fake '{head}' dropped"] += 1
            return "0"
        if len(body) == 3 and head in "1234M":
            return head
        counts[f"special {{{body}}} dropped"] += 1
        return "0"

    def notes(match: re.Match) -> str:
        return match.group(1) + _BRACE.sub(cell, match.group(2)) + ";"

    return re.sub(r"(#NOTES:)(.*?);", notes, text, flags=re.S)


def nx2ssc(args: argparse.Namespace) -> int:
    paths = _chart_paths(args.inputs)
    if not paths:
        raise SystemExit("no .NX charts found")
    output: Path = args.output
    output.parent.mkdir(parents=True, exist_ok=True)

    music = args.music or ""
    audio = None if args.no_audio else _song_audio(paths)
    if audio is not None and not music:
        music = output.stem + ".mp3"
    if audio is not None:
        shutil.copyfile(audio, output.parent / music)

    reports = []
    for path in paths:
        document = nx20.load(path, profile=PROFILE)
        try:
            report = compile_ssc_export(document, description=path.stem)
        except (SscExportError, ValueError) as exc:
            _warn(f"{path.name}: skipped: {exc}")
            continue
        reports.append(report)
        steps = report.charts[0].chart.steps_type
        _warn(f"{path.name}: {steps}, {len(report.charts)} chart(s)")
        for item in report.diagnostics:
            count = f" x{item.occurrences}" if item.occurrences > 1 else ""
            _warn(f"  {item.code}{count}: {item.message}")
    if not reports:
        raise SystemExit("nothing exported")

    random_types = Counter(r.charts[0].chart.steps_type for r in reports if r.helper_count)
    for steps, count in random_types.items():
        if count > 1:
            _warn(f"warning: {count} random {steps} charts share XSanity's DIVISION pool")

    song = SscSongInfo(title=args.title or output.stem, artist=args.artist or "", music=music)
    text = render_compiled_reports(tuple(reports), song)
    if args.dialect == "sm5":
        counts: Counter = Counter()
        text = _flatten_sm5(text, counts)
        for what, count in sorted(counts.items()):
            _warn(f"sm5: {what} x{count}")
    output.write_text(text, encoding="utf-8", newline="\n")
    _warn(f"wrote {output} ({len(reports)} chart(s)"
          + (f", audio {music}" if audio is not None else "") + ")")
    return 0


# --------------------------------------------------------------------- SSC -> NX

def _phoenix_bytes(document, level: int, step_artist: str) -> bytes:
    """Replace header metadata with Phoenix's Section/Level/Step Artist set.

    Layout: 'NX20', start_column, columns, packed, meta_count, meta pairs, ...
    The step artist is a NUL-terminated string at trailer offset 0, padded to
    4 bytes and followed by the trailer's total size, exactly as the official
    charts store it.
    """

    data = nx20.serialize(document)
    meta_count = struct.unpack_from("<I", data, 16)[0]
    body_start = 20 + 8 * meta_count
    body_end = len(data) - len(document.envelope.raw)

    pairs = [(H_SECTION, SECTION_ARCADE), (H_LEVEL, level)]
    if step_artist:
        pairs.append((H_STEP_ARTIST, 0))
        text = step_artist.encode("utf-8") + b"\0"
        text += b"\0" * (-len(text) % 4)
        trailer = text + struct.pack("<I", len(text) + 4)
    else:
        trailer = struct.pack("<I", 4)
    header = data[:16] + struct.pack("<I", len(pairs)) + b"".join(struct.pack("<II", *p) for p in pairs)
    return header + data[body_start:body_end] + trailer


def _name_token(value: str) -> str:
    token = re.sub(r"[^0-9A-Z]+", "", value.upper())
    return token or "CUSTOM"


def ssc2nx(args: argparse.Namespace) -> int:
    source: Path = args.input
    data = source.read_bytes()
    result = ssc_import.parse_ssc(data, source=str(source), profile=PROFILE)
    # Per-chart tags for STEPSTYPE/METER/CREDIT; same tag scanner the importer uses.
    text = data.decode("utf-8-sig", errors="replace")
    global_fields, sections = ssc_import._split_sections(ssc_import._scan_tags(text))

    out: Path = args.output
    out.mkdir(parents=True, exist_ok=True)
    used: set[str] = set()
    bpm = None
    written = 0
    for chart in result.charts:
        section = sections[int(chart.key.rsplit("-", 1)[1])]
        steps = ssc_import._field(section, global_fields, "STEPSTYPE").casefold()
        level = int(float(ssc_import._field(section, global_fields, "METER") or 0))
        credit = args.step_artist or ssc_import._field(section, global_fields, "CREDIT")
        label = ssc_import._field(section, global_fields, "DESCRIPTION") or chart.key

        prefix = STEPS_PREFIX.get(steps)
        columns = int(chart.document.columns.value)
        if prefix is None:
            prefix = {5: "S", 10: "D"}.get(columns)
            if prefix is None:
                _warn(f"{label}: skipped: {steps or '?'} ({columns} lanes) has no Phoenix layout")
                continue
            _warn(f"{label}: {steps} imported as {prefix} by lane geometry; "
                  "player ownership (co-op) is not represented")
        if not 1 <= level <= 99:
            _warn(f"{label}: METER {level} out of range; using 1")
            level = 1

        name = f"{prefix}{level}_{_name_token(credit or args.name_suffix)}"
        stem, n = name, 2
        while stem.casefold() in used:
            stem, n = f"{name}{n}", n + 1
        used.add(stem.casefold())

        payload = _phoenix_bytes(chart.document, level, credit)
        document = nx20.parse_bytes(payload, source=stem, profile=PROFILE)
        if nx20.serialize(document) != payload:
            raise SystemExit(f"{stem}: internal error: NX20 rebuild is not byte-exact")
        nx20.save_atomic(document, out / f"{stem}.NX", overwrite=args.force)
        written += 1
        if bpm is None:
            first = document.splits[0].blocks[0]
            bpm = float(first.bpm.value) if float(first.bpm.value) > 0 else None
        _warn(f"{label} [{steps}] -> {stem}.NX")
        for line in chart.diagnostics:
            _warn(f"  {line}")

    if not written:
        raise SystemExit("no chart converted")
    lightmap = next((p for p in out.iterdir() if p.name.upper() == "LM.NX"), None)
    if lightmap is None:
        nx20.save_atomic(create_blank_lightmap(bpm or 120.0, profile=PROFILE), out / "LM.NX")
        _warn(f"wrote blank {out / 'LM.NX'}")
    _warn(f"wrote {written} chart(s) to {out}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    a = sub.add_parser("nx2ssc", help="song folder / .NX files -> one .ssc (+ song audio)")
    a.add_argument("inputs", nargs="+", type=Path,
                   help="data/D/<ID>, data/D/<ID>/<ID>, or individual .NX files")
    a.add_argument("-o", "--output", type=Path, required=True, help="target .ssc")
    a.add_argument("--title", default="")
    a.add_argument("--artist", default="")
    a.add_argument("--music", default="", help="#MUSIC file name (default <output stem>.mp3)")
    a.add_argument("--no-audio", action="store_true", help="do not copy D/<ID>/A/.A")
    a.add_argument("--dialect", choices=("xsanity", "sm5"), default="xsanity",
                   help="xsanity keeps fake/hidden/item/noteskin cells as {..} groups; "
                        "sm5 flattens them for stock StepMania 5 / ITGmania / OutFox")
    a.set_defaults(handler=nx2ssc)

    b = sub.add_parser("ssc2nx", help=".ssc -> Phoenix-named .NX charts (+ blank LM.NX)")
    b.add_argument("input", type=Path)
    b.add_argument("-o", "--output", type=Path, required=True, help="target chart folder (D/<ID>/<ID>)")
    b.add_argument("--step-artist", default="", help="override #CREDIT as step artist / file suffix")
    b.add_argument("--name-suffix", default="CUSTOM", help="file suffix when no step artist is known")
    b.add_argument("--force", action="store_true", help="overwrite existing .NX files")
    b.set_defaults(handler=ssc2nx)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
