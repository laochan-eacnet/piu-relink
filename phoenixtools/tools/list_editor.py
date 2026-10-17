#!/usr/bin/env python3
"""Inspect and edit Phoenix LIST catalogs without discarding unknown record data."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from song_catalog_common import (
    Catalog, file_exists_casefold, resolve_song_directory, song_media_exists,
)

from list_editor_ops import *


STYLE_FIELD = "byte10"
STYLES = {"single": 0, "double": 2, "co-op": 3}
MEDIA = {
    "full audio": ("A", ".A"),
    "full video": ("V", ".V2"),
    "preview audio": ("P", "A", ".A"),
    "preview video": ("P", "V", ".V2"),
    "preview graphics": ("P", ".G"),
}


def integer(value: str) -> int:
    """Accept decimal, prefixed integers, and unprefixed hexadecimal IDs."""
    try:
        return int(value, 0)
    except ValueError:
        try:
            return int(value, 10)
        except ValueError:
            if value and all(character in "0123456789abcdefABCDEF"
                             for character in value):
                return int(value, 16)
    raise argparse.ArgumentTypeError(f"not an integer: {value!r}")


def chart_style(value: str) -> int | str:
    if value.casefold() in STYLES:
        return value.casefold()
    result = integer(value)
    if not 0 <= result <= 255:
        raise argparse.ArgumentTypeError("chart type must fit an unsigned byte")
    return result


def arguments(argv: list[str] | None = None) -> tuple[argparse.ArgumentParser,
                                                     argparse.Namespace]:
    workspace = Path(__file__).resolve().parent.parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database-root", type=Path,
                        default=workspace / "data" / "D",
                        help="directory containing LIST and LIST.N (default: workspace/data/D)")
    parser.add_argument("--language", type=int, default=0,
                        help="LIST.N language used for displayed text (default: 0)")
    commands = parser.add_subparsers(dest="command", required=True)

    show = commands.add_parser("show", help="show records and resolved text as JSON")
    show.add_argument("records", choices=("songs", "charts"))
    show.add_argument("--id", type=integer, help="filter by song ID")

    export = commands.add_parser("export-json", help="export the complete lossless model")
    export.add_argument("--output", type=Path, help="output file; omit for stdout")
    import_ = commands.add_parser("import-json", help="replace a catalog from a lossless export")
    import_.add_argument("--input", required=True, help="input JSON file, or - for stdin")

    add_song = commands.add_parser("add-song", help="clone a song record and add synchronized text")
    add_song.add_argument("--id", type=integer, required=True)
    add_song.add_argument("--title", required=True)
    add_song.add_argument("--artist", required=True)
    add_song.add_argument("--bpm", required=True, help="display text; also updates float BPM when numeric")
    add_song.add_argument("--template", type=integer, metavar="SONG_INDEX",
                          help="song record index to clone (default: first ordinary song)")

    add_chart = commands.add_parser("add-chart", help="clone a chart record for an existing NX file")
    add_chart.add_argument("--id", type=integer, required=True)
    add_chart.add_argument("--name", required=True, help="NX filename stem, without .NX")
    add_chart.add_argument("--type", type=chart_style, required=True,
                           help="single, double, co-op, or an exact raw byte10 value")
    add_chart.add_argument("--level", type=integer, required=True)
    add_chart.add_argument("--flags", type=integer,
                           help="additional bits ORed into byte10; named type low bits must not conflict")
    add_chart.add_argument("--template", type=integer, metavar="CHART_INDEX",
                           help="chart record index to clone (default: an existing chart of the same type)")

    remove_song = commands.add_parser("remove-song", help="remove a song and every chart referring to it")
    remove_song.add_argument("--id", type=integer, required=True)
    remove_chart = commands.add_parser("remove-chart", help="remove one non-dummy chart")
    selectors = remove_chart.add_mutually_exclusive_group(required=True)
    selectors.add_argument("--index", type=integer)
    selectors.add_argument("--id", type=integer)
    remove_chart.add_argument("--name", help="NX stem; required with --id")

    set_ = commands.add_parser("set", help="edit a raw field or a synchronized text convenience field")
    selectors = set_.add_mutually_exclusive_group(required=True)
    selectors.add_argument("--song-id", type=integer)
    selectors.add_argument("--chart-index", type=integer)
    set_.add_argument("--field", required=True,
                      help="raw model field; songs also support title/artist/bpm, charts support name")
    set_.add_argument("--value", required=True)
    return parser, parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    parser, args = arguments(argv)
    try:
        database = args.database_root.resolve()
        if args.command == "import-json":
            if args.input == "-":
                payload = json.load(sys.stdin)
            else:
                with Path(args.input).open(encoding="utf-8-sig") as source:
                    payload = json.load(source)
            if not isinstance(payload, dict):
                raise ValueError("lossless catalog JSON must be an object")
            catalog = Catalog.from_json(payload, database)
            catalog.validate()
            warnings = media_warnings(catalog, database, set(range(len(catalog.songs))))
            write_catalog(catalog, warnings)
            return 0

        catalog = Catalog.load(database)
        if args.command == "show":
            show(catalog, args)
            return 0
        if args.command == "export-json":
            payload = json.dumps(catalog.to_json(), ensure_ascii=False, indent=2) + "\n"
            if args.output is None:
                sys.stdout.write(payload)
            else:
                args.output.write_text(payload, encoding="utf-8")
                print(f"Wrote {args.output}")
            return 0

        if args.command == "add-song":
            affected, warnings = add_song(catalog, database, args)
        elif args.command == "add-chart":
            affected, warnings = add_chart(catalog, database, args)
        elif args.command == "remove-song":
            affected, warnings = remove_song(catalog, args)
        elif args.command == "remove-chart":
            affected, warnings = remove_chart(catalog, args)
        else:
            affected, warnings = set_field(catalog, database, args)
        catalog.validate()
        warnings.extend(media_warnings(catalog, database, affected))
        write_catalog(catalog, warnings)
        return 0
    except (OSError, ValueError, TypeError, KeyError, struct.error) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
