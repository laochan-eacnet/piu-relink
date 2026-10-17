#!/usr/bin/env python3
"""Enforce phoenixtools repository source/layout policies."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = (ROOT / "src", ROOT / "tests", ROOT / "tools")
SOURCE_SUFFIXES = {".c", ".h", ".py"}
BUILD_SUFFIXES = {".o", ".a", ".so", ".d", ".pyc"}
HARD_LIMIT = 350
PREFERRED_LIMIT = 200


def relative(path: Path) -> str:
    return str(path.relative_to(ROOT))


def main() -> int:
    errors: list[str] = []
    warnings: list[str] = []

    for directory in SOURCE_ROOTS:
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8")
            line_count = len(text.splitlines())
            if line_count > HARD_LIMIT:
                errors.append(
                    f"{relative(path)}: {line_count} lines exceeds {HARD_LIMIT}"
                )
            elif line_count > PREFERRED_LIMIT:
                warnings.append(
                    f"{relative(path)}: {line_count} lines exceeds preferred "
                    f"{PREFERRED_LIMIT}"
                )
            if path.suffix == ".h":
                first = text.splitlines()[0] if text.splitlines() else ""
                if first != "#pragma once":
                    errors.append(f"{relative(path)}: header must use #pragma once")
                if "#ifndef" in text:
                    errors.append(f"{relative(path)}: traditional header guard found")

    output_scan_roots = (*SOURCE_ROOTS, ROOT / "docs")
    for directory in output_scan_roots:
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix in BUILD_SUFFIXES:
                errors.append(f"{relative(path)}: build output must be inside bin/")
    for path in ROOT.iterdir():
        if path.is_file() and path.suffix in BUILD_SUFFIXES:
            errors.append(f"{relative(path)}: build output must be inside bin/")

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if errors:
        return 1
    print("source audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
