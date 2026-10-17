#!/usr/bin/env python3
"""Export Phoenix D/LIST as a read-only Markdown song catalog."""

from __future__ import annotations

import argparse
import sys
import struct
from collections import defaultdict
from pathlib import Path

sys.dont_write_bytecode = True

from song_catalog_common import (
    CHART_RECORD_SIZE, INVALID_INDEX, SONG_RECORD_SIZE, chart_label,
    file_exists_casefold, parse_string_table, resolve_song_directory,
    song_media_exists, string_at,
)
from song_catalog_markdown import write_catalog


def arguments() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    default_root = (script_dir.parent.parent /
                    "extracted/phoneix-2024-inner-decrypted/game-files/D")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database-root", type=Path, default=default_root)
    parser.add_argument("--language", type=int, default=0, choices=range(6))
    parser.add_argument("--output", type=Path,
                        default=script_dir.parent / "docs/songlist-international.md")
    parser.add_argument("--local-only", action="store_true")
    return parser.parse_args()


def parse_charts(raw: bytes, strings: list[str], song_count: int,
                 chart_count: int) -> dict[int, list[dict[str, object]]]:
    chart_base = 8 + song_count * SONG_RECORD_SIZE
    result: dict[int, list[dict[str, object]]] = defaultdict(list)
    for index in range(1, chart_count):
        offset = chart_base + index * CHART_RECORD_SIZE
        source_index, display_index = struct.unpack_from("<hh", raw, offset)
        name_index = struct.unpack_from("<i", raw, offset + 4)[0]
        chart_class, level, type_flags, unknown = struct.unpack_from(
            "<BBBB", raw, offset + 8
        )
        if not 0 <= source_index < song_count:
            continue
        name = string_at(strings, name_index, f"chart-{index}")
        result[source_index].append({
            "index": index, "source_song_index": source_index,
            "display_song_index": display_index,
            "name": name, "label": chart_label(name, level), "level": level,
            "class": chart_class, "type_flags": type_flags, "unknown": unknown,
        })
    return result


def unique_charts(records: list[dict[str, object]],
                  directory: Path | None) -> list[dict[str, object]]:
    seen: set[tuple[str, int]] = set()
    result: list[dict[str, object]] = []
    for chart in records:
        key = (str(chart["name"]).casefold(), int(chart["level"]))
        if key in seen:
            continue
        seen.add(key)
        item = dict(chart)
        item["file_present"] = bool(directory and file_exists_casefold(
            directory, [directory.name, f"{chart['name']}.NX"]
        ))
        result.append(item)
    result.sort(key=lambda item: (
        0 if str(item["label"]).startswith("S") else
        1 if str(item["label"]).startswith("D") and
             not str(item["label"]).startswith("DP") else 2,
        int(item["level"]), str(item["name"]).casefold(),
    ))
    return result


def parse_songs(raw: bytes, strings: list[str], database: Path,
                song_count: int, charts: dict[int, list[dict[str, object]]],
                local_only: bool) -> list[dict[str, object]]:
    songs: list[dict[str, object]] = []
    for index in range(1, song_count):
        offset = 8 + index * SONG_RECORD_SIZE
        song_id, title, artist, bpm, fallback_index = struct.unpack_from(
            "<Iiiii", raw, offset
        )
        trailing = struct.unpack_from("<i", raw, offset + 28)[0]
        if song_id in (0, 0xFFFFFFFF):
            continue
        directory, base_media = resolve_song_directory(database, song_id)
        if local_only and not directory:
            continue
        media: dict[str, bool] = {}
        for key, parts in {
            "preview_audio": ["P", "A", ".A"], "full_audio": ["A", ".A"],
            "preview_video": ["P", "V", ".V2"], "full_video": ["V", ".V2"],
        }.items():
            media[key], used_base = song_media_exists(
                database, directory, song_id, parts
            )
            base_media = base_media or used_base
        songs.append({
            "index": index, "id": song_id,
            "title": string_at(strings, title, f"Song {song_id:X}"),
            "artist": string_at(strings, artist, "Unknown artist"),
            "bpm": string_at(strings, bpm, "?"),
            "fallback_song_index": fallback_index,
            "trailing": string_at(strings, trailing)
                        if trailing != INVALID_INDEX else "",
            "local": directory is not None, "uses_base_media": base_media,
            "directory": directory,
            "charts": unique_charts(charts.get(index, []), directory), **media,
        })
    return songs


def main() -> int:
    args = arguments()
    database = args.database_root.resolve()
    list_path = database / "LIST"
    raw = list_path.read_bytes()
    strings = parse_string_table(database / f"LIST.{args.language}")
    if len(raw) < 8:
        raise SystemExit(f"invalid short catalog: {list_path}")
    song_count, chart_count = struct.unpack_from("<II", raw, 0)
    expected = 8 + song_count * SONG_RECORD_SIZE + chart_count * CHART_RECORD_SIZE
    if len(raw) != expected:
        raise SystemExit(f"unexpected LIST size: got {len(raw)}, expected {expected}")
    charts = parse_charts(raw, strings, song_count, chart_count)
    songs = parse_songs(raw, strings, database, song_count, charts,
                        args.local_only)
    write_catalog(args.output, songs, list_path, args.language,
                  song_count, chart_count)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
