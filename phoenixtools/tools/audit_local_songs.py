#!/usr/bin/env python3
"""Audit locally backed Phoenix songs, charts, media, and regional policy."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import struct
from collections import Counter, defaultdict
from pathlib import Path

sys.dont_write_bytecode = True

from song_audit_markdown import write_summary
from song_audit_support import anomaly_lists, regional_policy, song_resource_exists
from song_catalog_common import (
    CHART_RECORD_SIZE, SONG_RECORD_SIZE, file_exists_casefold,
    parse_string_table, resolve_song_directory, string_at,
)


def arguments() -> argparse.Namespace:
    workspace = Path(__file__).resolve().parent.parent.parent
    report_root = workspace / "phoenixtools-local/diagnostics/tests"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=workspace /
                        "extracted/phoneix-2024-inner-decrypted/game-files")
    parser.add_argument("--language", type=int, default=0, choices=range(6))
    parser.add_argument("--json-output", type=Path,
                        default=report_root / "song-resource-audit.json")
    parser.add_argument("--markdown-output", type=Path,
                        default=report_root / "song-resource-audit.md")
    return parser.parse_args()


def chart_names(raw: bytes, strings: list[str], song_count: int,
                chart_count: int) -> dict[int, list[str]]:
    base = 8 + song_count * SONG_RECORD_SIZE
    names: dict[int, list[str]] = defaultdict(list)
    for index in range(1, chart_count):
        offset = base + index * CHART_RECORD_SIZE
        source_index, _ = struct.unpack_from("<hh", raw, offset)
        name_index = struct.unpack_from("<i", raw, offset + 4)[0]
        if 0 < source_index < song_count:
            names[source_index].append(
                string_at(strings, name_index, f"chart-{index}")
            )
    return names


def audit_songs(raw: bytes, strings: list[str], database: Path,
                song_count: int, names: dict[int, list[str]]) -> tuple[
                    list[dict[str, object]], dict[int, dict[str, object]],
                    Counter[str], Counter[str]]:
    songs: list[dict[str, object]] = []
    by_id: dict[int, dict[str, object]] = {}
    media_counts: Counter[str] = Counter()
    class_counts: Counter[str] = Counter()
    specs = {
        "full_audio": ["A", ".A"], "preview_audio": ["P", "A", ".A"],
        "full_video": ["V", ".V2"], "preview_video": ["P", "V", ".V2"],
        "preview_graphics": ["P", ".G"],
    }
    for index in range(1, song_count):
        offset = 8 + index * SONG_RECORD_SIZE
        song_id, title, artist = struct.unpack_from("<Iii", raw, offset)
        if song_id in (0, 0xFFFFFFFF):
            continue
        directory, base_alias = resolve_song_directory(database, song_id)
        unique_names = list(dict.fromkeys(name.casefold() for name in names[index]))
        present, missing = [], []
        for name in unique_names:
            target = [directory.name, f"{name}.NX"] if directory else []
            (present if target and file_exists_casefold(directory, target)
             else missing).append(name)
        media, fallbacks = {}, {}
        for key, parts in specs.items():
            media[key], fallbacks[key] = song_resource_exists(
                database, directory, song_id, parts
            )
        kind = ("encoded_alias" if song_id & 0xF0000000 else
                "special_preview_record" if (song_id & 0xFF000) in
                (0xBF000, 0xDF000) else "ordinary_song")
        class_counts[kind] += 1
        if directory:
            class_counts[f"local_{kind}"] += 1
            media_counts["local"] += 1
            for key, value in media.items():
                media_counts[key] += bool(value)
            media_counts["with_catalog_charts"] += bool(unique_names)
            media_counts["with_local_nx"] += bool(present)
            media_counts["catalog_chart_refs"] += len(unique_names)
            media_counts["present_chart_refs"] += len(present)
            media_counts["missing_chart_refs"] += len(missing)
        record: dict[str, object] = {
            "index": index, "id": f"0x{song_id:X}", "id_value": song_id,
            "id_class": kind, "title": string_at(strings, title),
            "artist": string_at(strings, artist), "local": bool(directory),
            "directory": str(directory) if directory else None,
            "directory_base_alias": base_alias,
            "media_base_fallback": fallbacks, "media": media,
            "catalog_charts": unique_names, "present_charts": present,
            "missing_charts": missing,
        }
        songs.append(record)
        by_id[song_id] = record
    return songs, by_id, media_counts, class_counts


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    database = root / "D"
    raw = (database / "LIST").read_bytes()
    strings = parse_string_table(database / f"LIST.{args.language}")
    song_count, chart_count = struct.unpack_from("<II", raw, 0)
    expected = 8 + song_count * SONG_RECORD_SIZE + chart_count * CHART_RECORD_SIZE
    if len(raw) != expected:
        raise SystemExit(f"unexpected LIST size: got {len(raw)}, expected {expected}")
    names = chart_names(raw, strings, song_count, chart_count)
    songs, by_id, media, classes = audit_songs(
        raw, strings, database, song_count, names
    )
    regions = regional_policy(root, by_id)
    local = [song for song in songs if song["local"]]
    playable = [song for song in local
                if song["media"]["full_audio"] and  # type: ignore[index]
                song["present_charts"]]
    anomalies = anomaly_lists(local)
    report = {
        "generated": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "root": str(root),
        "catalog": {
            "raw_song_records": song_count, "raw_chart_records": chart_count,
            "valid_songs": len(songs), "local_songs": len(local),
            "playable_local_songs": len(playable),
            "missing_or_remote_songs": len(songs) - len(local),
            "id_classes": dict(classes),
        },
        "local_resource_counts": dict(media),
        "anomaly_counts": {key: len(value) for key, value in anomalies.items()},
        "anomalies": anomalies, "regions": regions, "songs": songs,
    }
    args.json_output.parent.mkdir(parents=True, exist_ok=True)
    args.json_output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    write_summary(args.markdown_output, report, songs, local, len(playable),
                  classes, media, anomalies, regions)
    print(json.dumps(report["catalog"], ensure_ascii=False))
    print(f"wrote {args.json_output}\nwrote {args.markdown_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
