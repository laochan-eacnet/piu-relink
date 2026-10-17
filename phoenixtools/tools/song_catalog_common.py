"""Shared catalog parsing and case-folded media lookup helpers."""
from pathlib import Path
from typing import Iterable
from song_catalog_model import (Catalog, StringEntry, StringTable, SONG_RECORD_SIZE,
    CHART_RECORD_SIZE, INVALID_INDEX, SONG_FIELDS, CHART_FIELDS, SONG_STRUCT, CHART_PREFIX)

def parse_string_table(path: Path) -> list[str]:
    return [entry.text for entry in StringTable.parse(path.read_bytes()).entries]

def string_at(strings: list[str], index: int, fallback: str = "") -> str:
    return strings[index] if 0 <= index < len(strings) else fallback

def child_casefold(directory: Path, name: str) -> Path | None:
    candidate = directory / name
    if candidate.exists(): return candidate
    try:
        wanted = name.casefold()
        return next((child for child in directory.iterdir() if child.name.casefold() == wanted), None)
    except OSError:
        return None

def resolve_song_directory(database: Path, song_id: int) -> tuple[Path | None, bool]:
    direct = child_casefold(database, f"{song_id:X}")
    if direct and direct.is_dir(): return direct, False
    base_id = song_id & 0xFFFF
    if base_id != song_id:
        base = child_casefold(database, f"{base_id:X}")
        if base and base.is_dir(): return base, True
    return None, False

def file_exists_casefold(directory: Path | None, relative_parts: Iterable[str]) -> bool:
    current = directory
    if current is None: return False
    for part in relative_parts:
        current = child_casefold(current, part)
        if current is None: return False
    return current.is_file()

def song_media_exists(database: Path, directory: Path | None, song_id: int, relative_parts: Iterable[str]) -> tuple[bool, bool]:
    parts = list(relative_parts)
    if file_exists_casefold(directory, parts): return True, False
    base_id = song_id & 0x0FFFFFFF
    if base_id and base_id != song_id:
        base = child_casefold(database, f"{base_id:X}")
        if base and base.is_dir() and file_exists_casefold(base, parts): return True, True
    return False, False

def chart_label(name: str, level: int) -> str:
    lower = name.casefold()
    if lower.startswith("dp"): return f"DP{level}"
    if lower.startswith("d"): return f"D{level}"
    if lower.startswith("s"): return f"S{level}"
    return f"L{level}"

def markdown_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ")
