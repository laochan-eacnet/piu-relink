#!/usr/bin/env python3
"""Merge package defaults or publish the standard-library LAN server and card inputs."""

import argparse
from pathlib import Path
import re
import shutil
import sys


SECTION = re.compile(r"^\ufeff?\s*\[([^]]+)\]\s*(?:[#;].*)?$")
OPTION = re.compile(r"^\s*([^#;=\s][^=]*?)\s*=")


def sections(lines):
    result = []
    for index, line in enumerate(lines):
        match = SECTION.match(line.rstrip("\r\n"))
        if match:
            if result:
                result[-1][2] = index
            result.append([match[1].strip().casefold(), index, len(lines)])
    return result


def merge_ini(existing, defaults):
    """Keep existing text/values; insert only absent default sections and options."""
    if not existing:
        return defaults
    newline = "\r\n" if "\r\n" in existing else "\n"
    defaults = defaults.replace("\r\n", "\n").replace("\n", newline)
    lines = existing.splitlines(keepends=True)
    default_lines = defaults.splitlines(keepends=True)
    present = {}
    ends = {}
    for section, start, end in sections(lines):
        options = present.setdefault(section, set())
        for line in lines[start + 1:end]:
            match = OPTION.match(line)
            if match:
                options.add(match[1].strip().casefold())
        # Insert before trailing comments/blank lines so notes for the next
        # section stay with it, and retain every original line unchanged.
        while end > start + 1 and (not lines[end - 1].strip()
                                  or lines[end - 1].lstrip().startswith(("#", ";"))):
            end -= 1
        ends[section] = end

    insertions = {}
    for section, start, end in sections(default_lines):
        if section not in present:
            # A new section brings the comment lines directly above its header and leaves its
            # trailing comments/blank lines, which introduce the following default section.
            insertion = len(lines)
            first = start
            while first and default_lines[first - 1].lstrip().startswith(("#", ";")):
                first -= 1
            while end > start + 1 and (not default_lines[end - 1].strip()
                                      or default_lines[end - 1].lstrip().startswith(("#", ";"))):
                end -= 1
            addition = "".join(default_lines[first:end])
        else:
            insertion = ends[section]
            addition_lines = []
            pending = []
            for line in default_lines[start + 1:end]:
                pending.append(line)
                match = OPTION.match(line)
                if match:
                    if match[1].strip().casefold() not in present[section]:
                        addition_lines.extend(pending)
                    pending = []
            addition = "".join(addition_lines)
        if addition:
            if not addition.startswith(("\r", "\n")):
                addition = newline + addition
            if not addition.endswith("\n"):
                addition += newline
            insertions[insertion] = insertions.get(insertion, "") + addition
    for index in sorted(insertions, reverse=True):
        if index and not lines[index - 1].endswith("\n"):
            lines[index - 1] += newline
        lines.insert(index, insertions[index])
    return "".join(lines)


def merge_config(path, defaults):
    """Merge defaults and migrate the former separate runtime INI, if present."""
    if path.exists():
        with path.open(encoding="utf-8", newline="") as source:
            existing = source.read()
    else:
        existing = ""
    legacy = path.with_name("lxrt.ini")
    merged = existing
    if legacy.exists():
        with legacy.open(encoding="utf-8", newline="") as source:
            # Explicit unified settings win, followed by legacy settings, then
            # current defaults. Read before writing so failed migration is safe.
            merged = merge_ini(merged, source.read())
    merged = merge_ini(merged, defaults)
    if merged != existing:
        with path.open("w", encoding="utf-8", newline="") as output:
            output.write(merged)
    legacy.unlink(missing_ok=True)


def publish_server(destination):
    root = Path(__file__).resolve().parent.parent
    server = destination / "server"
    server.mkdir(parents=True, exist_ok=True)
    # Copy code only, never repository databases, assets, tests or bytecode.
    for source in sorted((root / "server").glob("*.py")):
        if not source.name.startswith("test_"):
            shutil.copy2(source, server / source.name)
    shutil.copy2(root / "tools" / "start-server.cmd", destination / "start-server.cmd")
    # Empty means unconfigured. Existing card files, including their contents
    # and timestamps, are never opened for writing during publication.
    for name in ("card1.txt", "card2.txt"):
        try:
            (destination / name).open("x").close()
        except FileExistsError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--merge-config", action="store_true",
                        help="merge INI defaults from stdin into destination")
    args = parser.parse_args()
    if args.merge_config:
        merge_config(args.destination, sys.stdin.read())
    else:
        publish_server(args.destination)


if __name__ == "__main__":
    main()
