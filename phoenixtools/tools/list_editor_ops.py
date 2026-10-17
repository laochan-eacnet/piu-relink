"""Importable LIST catalog operations used by list_editor.py."""
from __future__ import annotations
import argparse, copy, struct, sys
from pathlib import Path
from song_catalog_common import Catalog, file_exists_casefold, resolve_song_directory, song_media_exists

STYLE_FIELD = "byte10"
STYLES = {"single": 0, "double": 2, "co-op": 3}
MEDIA = {
    "full audio": ("A", ".A"), "full video": ("V", ".V2"),
    "preview audio": ("P", "A", ".A"), "preview video": ("P", "V", ".V2"),
    "preview graphics": ("P", ".G"),
}

def integer(value: str) -> int:
    try: return int(value, 0)
    except ValueError:
        try: return int(value, 10)
        except ValueError:
            if value and all(c in "0123456789abcdefABCDEF" for c in value): return int(value, 16)
    raise argparse.ArgumentTypeError(f"not an integer: {value!r}")

def chart_style(value: str) -> int | str:
    if value.casefold() in STYLES: return value.casefold()
    result = integer(value)
    if not 0 <= result <= 255: raise argparse.ArgumentTypeError("chart type must fit an unsigned byte")
    return result

def ordinary_id(song_id: int) -> bool:
    return (0 < song_id <= 0xFFFFFFFF and (song_id & 0xE0000000) != 0xE0000000
            and (song_id & 0xDF000) != 0xDF000 and (song_id & 0xEF000) != 0xAF000
            and (song_id & 0xFFFDF000) != 0xDF000 and (song_id & 0xFFF0F000) != 0xF000
            and (song_id & 0xFF000) not in (0xBF000, 0xCF000, 0xDF000))

def song_index(catalog: Catalog, song_id: int) -> int:
    matches = [i for i, song in enumerate(catalog.songs) if song["id"] == song_id]
    if not matches: raise ValueError(f"song ID 0x{song_id:X} is not in the catalog")
    if len(matches) != 1: raise ValueError(f"song ID 0x{song_id:X} is ambiguous (indices {matches})")
    return matches[0]

def record_at(records: list[dict], index: int, label: str) -> dict:
    if not 0 <= index < len(records): raise ValueError(f"{label} index {index} is outside 0..{len(records)-1}")
    return records[index]

def require_language(catalog: Catalog, language: int) -> None:
    if f"LIST.{language}" not in catalog.tables: raise ValueError(f"language table LIST.{language} is not present")

def name_stem(name: str) -> str:
    if (not name or name in (".", "..") or any(c in name for c in "/\\\x00:") or name.casefold().endswith(".nx")):
        raise ValueError("chart name must be an NX filename stem, without a path or .NX suffix")
    return name

def require_directory(database: Path, song_id: int) -> Path:
    directory, _ = resolve_song_directory(database, song_id)
    if directory is None: raise ValueError(f"song folder is missing for ID 0x{song_id:X}: {database / f'{song_id:X}'}")
    return directory

def require_nx(database: Path, song_id: int, name: str) -> None:
    name_stem(name); directory = require_directory(database, song_id)
    if not file_exists_casefold(directory, (directory.name, f"{name}.NX")):
        raise ValueError(f"chart file is missing: {directory / directory.name / f'{name}.NX'}")

def chart_names(catalog: Catalog, chart: dict) -> set[str]:
    index = chart["name_index"]
    return {table.entries[index].text if 0 <= index < len(table.entries) else "" for table in catalog.tables.values()}

def require_chart_assets(catalog: Catalog, database: Path, chart: dict) -> None:
    source = record_at(catalog.songs, chart["source_song_index"], "resource song")
    require_directory(database, source["id"])
    display_index = chart["display_song_index"]
    if display_index >= 0 and display_index != chart["source_song_index"]:
        require_directory(database, record_at(catalog.songs, display_index, "display song")["id"])
    for name in chart_names(catalog, chart): require_nx(database, source["id"], name)

def bpm_bits(text: str) -> int | None:
    try: numeric = float(text)
    except ValueError: return None
    try: return struct.unpack("<I", struct.pack("<f", numeric))[0]
    except (OverflowError, struct.error) as error: raise ValueError(f"BPM {text!r} cannot be represented as a 32-bit float") from error

def media_warnings(catalog: Catalog, database: Path, indices: set[int]) -> list[str]:
    missing: dict[str, list[str]] = {}
    for index in sorted(indices):
        song = catalog.songs[index]; sid = song["id"]
        if index == 0 or sid in (0, 0xFFFFFFFF): continue
        directory, _ = resolve_song_directory(database, sid); label = f"0x{sid:X}"
        if directory is None: missing.setdefault("song folder", []).append(label)
        for kind, parts in MEDIA.items():
            if not song_media_exists(database, directory, sid, parts)[0]: missing.setdefault(kind, []).append(label)
    nx_missing = []
    for index, chart in enumerate(catalog.charts):
        if index == 0 or chart["source_song_index"] not in indices: continue
        sid = catalog.songs[chart["source_song_index"]]["id"]
        if sid in (0, 0xFFFFFFFF): continue
        directory, _ = resolve_song_directory(database, sid)
        for name in sorted(chart_names(catalog, chart)):
            if directory is None or not file_exists_casefold(directory, (directory.name, f"{name}.NX")):
                nx_missing.append(f"chart {index} (0x{sid:X}/{name}.NX)")
    if nx_missing: missing["NX chart file"] = nx_missing
    return [f"missing {kind}: {len(labels)} ({', '.join(labels[:5])}{f', and {len(labels)-5} more' if len(labels)>5 else ''})" for kind, labels in missing.items()]

def print_warnings(warnings: list[str]) -> None:
    if warnings:
        print(f"Warning summary ({len(warnings)} categories):", file=sys.stderr)
        for warning in warnings: print(f"  {warning}", file=sys.stderr)

def show(catalog: Catalog, args: argparse.Namespace) -> None:
    require_language(catalog, args.language)
    indices = {i for i, song in enumerate(catalog.songs) if args.id is None or song["id"] == args.id}
    if args.records == "songs":
        records = [dict(song, index=i, title=catalog.text(song["title_index"], args.language), artist=catalog.text(song["artist_index"], args.language), bpm=catalog.text(song["bpm_text_index"], args.language)) for i, song in enumerate(catalog.songs) if i in indices]
    else:
        records = [dict(chart, index=i, name=catalog.text(chart["name_index"], args.language), chart_class=chart["byte8"], type=chart[STYLE_FIELD]&3, flags=chart[STYLE_FIELD]&~3) for i, chart in enumerate(catalog.charts) if args.id is None or chart["source_song_index"] in indices or chart["display_song_index"] in indices]
    print(__import__("json").dumps(records, ensure_ascii=False, indent=2))

def add_song(catalog: Catalog, database: Path, args: argparse.Namespace) -> tuple[set[int], list[str]]:
    if not ordinary_id(args.id): raise ValueError("new song ID must be a nonzero ordinary ID (no encoded aliases or special previews)")
    if any(song["id"] == args.id for song in catalog.songs): raise ValueError(f"song ID 0x{args.id:X} already exists")
    if len(catalog.songs) > 0x7FFF: raise ValueError("new song index would exceed the signed 16-bit chart-reference limit")
    require_directory(database, args.id)
    template = args.template if args.template is not None else next((i for i, s in enumerate(catalog.songs) if i and ordinary_id(s["id"])), None)
    if template is None: raise ValueError("no ordinary song template exists; select a record with --template")
    song = record_at(catalog.songs, template, "template song").copy(); song["id"] = args.id
    if song["fallback_song_index"] == template: song["fallback_song_index"] = -1
    song["title_index"] = catalog.append_string(args.title); song["artist_index"] = catalog.append_string(args.artist); song["bpm_text_index"] = catalog.append_string(args.bpm)
    numeric = bpm_bits(args.bpm); warnings = []
    if numeric is not None: song["bpm_bits"] = numeric
    else: warnings.append("BPM text is not numeric; preserved the template's raw bpm_bits")
    index = len(catalog.songs); catalog.songs.append(song); return {index}, warnings

def add_chart(catalog: Catalog, database: Path, args: argparse.Namespace) -> tuple[set[int], list[str]]:
    index = song_index(catalog, args.id)
    if index == 0 or args.id in (0, 0xFFFFFFFF): raise ValueError("cannot add a chart to a dummy song")
    if index > 0x7FFF: raise ValueError("song index exceeds the signed 16-bit chart-reference limit")
    require_nx(database, args.id, args.name)
    if any(index in (c["source_song_index"], c["display_song_index"]) and any(n.casefold() == args.name.casefold() for n in chart_names(catalog, c)) for c in catalog.charts[1:]): raise ValueError(f"song 0x{args.id:X} already has a chart named {args.name!r}")
    named = isinstance(args.type, str); style = STYLES[args.type] if named else args.type & 3
    if named and ((args.type == "co-op") != (args.level >= 50)): raise ValueError("co-op requires raw --level >= 50; single/double require --level < 50")
    if args.flags is not None:
        if not 0 <= args.flags <= 255: raise ValueError("--flags must fit an unsigned byte")
        if named and (args.flags & 3) & ~style: raise ValueError("--flags low bits conflict with the requested named --type")
    candidates = [(i, c) for i, c in enumerate(catalog.charts) if i and c[STYLE_FIELD]&3 == style and (not named or (c["level"] >= 50) == (args.type == "co-op"))]
    template = args.template if args.template is not None else next((i for i, c in candidates if index in (c["source_song_index"], c["display_song_index"])), candidates[0][0] if candidates else None)
    if template is None: raise ValueError("no existing chart has this type; choose an explicit --template index")
    chart = record_at(catalog.charts, template, "template chart").copy(); chart.update(source_song_index=index, display_song_index=index, name_index=catalog.append_string(name_stem(args.name)), level=args.level)
    chart[STYLE_FIELD] = ((chart[STYLE_FIELD] & ~3) | style) if named else args.type
    if args.flags is not None: chart[STYLE_FIELD] |= args.flags
    positions = [i for i, c in enumerate(catalog.charts) if i and c["source_song_index"] == index]; insert_at = max(positions)+1 if positions else len(catalog.charts); catalog.charts[insert_at:insert_at] = [chart]
    return {index}, []

def remove_song(catalog: Catalog, args: argparse.Namespace) -> tuple[set[int], list[str]]:
    index = song_index(catalog, args.id)
    if index == 0: raise ValueError("cannot remove the dummy song at index 0")
    catalog.charts[1:] = [c for c in catalog.charts[1:] if index not in (c["source_song_index"], c["display_song_index"])]
    del catalog.songs[index]
    for chart in catalog.charts[1:]:
        for field in ("source_song_index", "display_song_index"):
            if chart[field] > index: chart[field] -= 1
    for song in catalog.songs[1:]:
        if song["fallback_song_index"] == index: song["fallback_song_index"] = -1
        elif song["fallback_song_index"] > index: song["fallback_song_index"] -= 1
    return set(), []

def remove_chart(catalog: Catalog, args: argparse.Namespace) -> tuple[set[int], list[str]]:
    index = args.index
    if index is None:
        if args.name is None: raise ValueError("--id requires --name; use --index to remove by record index")
        resource = song_index(catalog, args.id); matches = [i for i, c in enumerate(catalog.charts) if resource in (c["source_song_index"], c["display_song_index"]) and any(n.casefold()==args.name.casefold() for n in chart_names(catalog,c))]
        if len(matches) != 1: raise ValueError(f"expected one chart for 0x{args.id:X}/{args.name}, found indices {matches}; use --index")
        index = matches[0]
    elif args.name is not None: raise ValueError("--name can only be used with --id")
    chart = record_at(catalog.charts, index, "chart")
    if index == 0: raise ValueError("cannot remove the dummy chart at index 0")
    affected = {chart["source_song_index"]} if chart["source_song_index"] >= 0 else set()
    del catalog.charts[index]
    return affected, []

def raw_value(record: dict, field: str, value: str) -> int | str:
    if field not in record: raise ValueError(f"unknown raw field {field!r}; fields: {', '.join(record)}")
    if field == "tail_hex": return value
    try: return integer(value)
    except argparse.ArgumentTypeError as error: raise ValueError(str(error)) from error

def set_field(catalog: Catalog, database: Path, args: argparse.Namespace) -> tuple[set[int], list[str]]:
    field, warnings = args.field, []
    if args.song_id is not None:
        index = song_index(catalog, args.song_id); song = catalog.songs[index]; text_fields = {"title":"title_index", "artist":"artist_index", "bpm":"bpm_text_index"}
        if field in text_fields:
            song[text_fields[field]] = catalog.append_string(args.value)
            if field == "bpm":
                numeric = bpm_bits(args.value)
                if numeric is not None: song["bpm_bits"] = numeric
                else: warnings.append("BPM text is not numeric; preserved the song's raw bpm_bits")
        else:
            previous = song.get(field); value = raw_value(song, field, args.value)
            if field == "id" and value != previous:
                if any(i != index and c["id"] == value for i,c in enumerate(catalog.songs)): raise ValueError(f"song ID 0x{value:X} already exists")
                require_directory(database, value)
            if field == "fallback_song_index" and value != previous and value >= 0: require_directory(database, record_at(catalog.songs,value,"fallback song")["id"])
            song[field] = value
            if field == "id" and value != previous:
                for i, chart in enumerate(catalog.charts):
                    if i and index in (chart["source_song_index"], chart["display_song_index"]): require_chart_assets(catalog,database,chart)
        return {index}, warnings
    chart = record_at(catalog.charts, args.chart_index, "chart")
    if field == "name": chart["name_index"] = catalog.append_string(name_stem(args.value))
    else: chart[field] = raw_value(chart, field, args.value)
    if field in ("name", "name_index", "source_song_index", "display_song_index"): require_chart_assets(catalog,database,chart)
    return {chart["source_song_index"]} if chart["source_song_index"] >= 0 else set(), warnings

def write_catalog(catalog: Catalog, warnings: list[str]) -> None:
    preexisting = {catalog.database/n for n in ("LIST", *catalog.tables) if (catalog.database/n).is_file()}; paths = catalog.write(); print_warnings(warnings)
    if not paths: print("No catalog files changed."); return
    for path in paths:
        print(f"Wrote {path}"); backup=path.with_name(path.name+".bak")
        if path in preexisting and backup.is_file(): print(f"  Backup: {backup}")
