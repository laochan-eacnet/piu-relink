"""Policy and resource helpers for audit_local_songs.py."""

import re
from pathlib import Path

from song_catalog_common import child_casefold, file_exists_casefold

HEX_ID = re.compile(r"0x([0-9a-fA-F]+)")
POLICY_ENTRY = re.compile(r"([A-Z_]+)\s*\(([^)]*)\)")


def policy_ids(path: Path) -> dict[str, list[int]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return {
        key: [int(value, 16) for value in HEX_ID.findall(body)]
        for key, body in POLICY_ENTRY.findall(text)
    }


def song_resource_exists(database: Path, directory: Path | None,
                         song_id: int, parts: list[str]) -> tuple[bool, bool]:
    if file_exists_casefold(directory, parts):
        return True, False
    base_id = song_id & 0x0FFFFFFF
    if base_id and base_id != song_id:
        base = child_casefold(database, f"{base_id:X}")
        if base and base.is_dir() and file_exists_casefold(base, parts):
            return True, True
    return False, False


def regional_policy(root: Path, by_id: dict[int, dict[str, object]]) -> dict[str, object]:
    report: dict[str, object] = {}
    for path in sorted((root / "MARKET").glob("*/SONG.TXT"),
                       key=lambda item: item.parent.name):
        entries = policy_ids(path)
        checked: dict[str, object] = {}
        for key in ("START_SONG", "DEMO_SONG", "RESTRICTED_SONGS_BASIC",
                    "RESTRICTED_SONGS_FULL"):
            references = []
            for value in entries.get(key, []):
                record = by_id.get(value)
                if record is None and (value & 0xFFFF) != value:
                    record = by_id.get(value & 0xFFFF)
                references.append({
                    "id": f"0x{value:X}", "catalog": record is not None,
                    "local": bool(record and record["local"]),
                })
            checked[key] = references
        report[path.parent.name] = checked
    return report


def anomaly_lists(local_songs: list[dict[str, object]]) -> dict[str, list[object]]:
    media_keys = (
        "full_audio", "preview_audio", "full_video", "preview_video",
        "preview_graphics",
    )
    result = {
        f"without_{key}": [song["id"] for song in local_songs
                           if not song["media"][key]]  # type: ignore[index]
        for key in media_keys
    }
    result["without_local_nx"] = [song["id"] for song in local_songs
                                  if not song["present_charts"]]
    result["with_missing_chart_refs"] = [song["id"] for song in local_songs
                                         if song["missing_charts"]]
    return result
