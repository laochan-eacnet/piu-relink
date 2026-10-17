#!/usr/bin/env python3
"""Run a Windows program with a timeout, capture stdout+stderr, print the full exit code.
    python tools/run.py [-t SECONDS] [-o LOGFILE] [-C DIR] [-s SECTION.KEY=VALUE ...] [-i INPUT_SCRIPT] program [args...]
Exit code is printed as hex (NTSTATUS crashes are 0xC000xxxx); on timeout the process is killed.
-i runs tools/inject.py INPUT_SCRIPT concurrently (its log goes to LOGFILE.inject).
-s temporarily overrides phoenixtools.ini beside the executable; repeated settings use the last value.
The original INI bytes (or its absence) are restored after child cleanup, including on exceptions
and handled termination signals. Forcibly terminating Python itself cannot run this cleanup.
"""
import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import re
import signal
import subprocess
import sys


SECTION = re.compile(r"^\ufeff?\s*\[([^]]+)\]\s*(?:[#;].*)?$")
OPTION = re.compile(r"^\s*([^#;=\s][^=]*?)\s*=")


def setting(text):
    name, equals, value = text.partition("=")
    section, dot, key = name.partition(".")
    section, key = section.strip(), key.strip()
    if (not equals or not dot or not section or not key
            or any(c in text for c in "\r\n\0")
            or any(c in section for c in "[]")
            or key.startswith(("#", ";", "["))):
        raise argparse.ArgumentTypeError("expected section.key=value")
    return section, key, value


def override_ini(original, settings):
    """Change only requested assignments, retaining unrelated lines verbatim."""
    text = original.decode("utf-8", errors="surrogateescape")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)
    for section, key, value in settings:
        wanted = section.casefold()
        current = ""
        insertion = None
        found = False
        for index, line in enumerate(lines):
            header = SECTION.match(line.rstrip("\r\n"))
            if header:
                if current == wanted:
                    insertion = index
                current = header[1].strip().casefold()
                if current == wanted:
                    insertion = len(lines)
                continue
            if current != wanted:
                continue
            option = OPTION.match(line)
            if not option:
                continue
            name = option[1].strip()
            # These sections use case-sensitive guest paths as their keys.
            matches = name == key if wanted in ("mounts", "serial") else name.casefold() == key.casefold()
            if matches:
                ending = line[len(line.rstrip("\r\n")):]
                lines[index] = line[:option.end()] + " " + value + ending
                found = True
        if found:
            continue
        if insertion is None:
            if lines:
                if not lines[-1].endswith(("\r", "\n")):
                    lines[-1] += newline
                lines.append(newline)
            lines.extend((f"[{section}]{newline}", f"{key} = {value}{newline}"))
        else:
            if insertion and not lines[insertion - 1].endswith(("\r", "\n")):
                lines[insertion - 1] += newline
            lines.insert(insertion, f"{key} = {value}{newline}")
    return "".join(lines).encode("utf-8", errors="surrogateescape")


def stop(process):
    if process is not None:
        try:
            if process.poll() is None:
                process.kill()
        finally:
            process.wait()


def terminate(signum, frame):
    raise SystemExit(128 + signum)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-t", type=float, default=60)
    ap.add_argument("-o", default=None)
    ap.add_argument("-C", default=None)
    ap.add_argument("-s", action="append", type=setting, default=[], metavar="SECTION.KEY=VALUE")
    ap.add_argument("-i", default=None)
    ap.add_argument("prog", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    if not a.prog:
        ap.error("a program is required")

    # Resolve before applying -C, just as the original runner did.
    exe = os.path.abspath(a.prog[0])
    ini = Path(exe).with_name("phoenixtools.ini")
    original = None
    replacement = None
    if a.s:
        try:
            original = ini.read_bytes()
        except FileNotFoundError:
            pass
        replacement = override_ini(original if original is not None else b"", a.s)

    managed_signals = [signal.SIGINT, signal.SIGTERM]
    if hasattr(signal, "SIGBREAK"):
        managed_signals.append(signal.SIGBREAK)
    handlers = {signum: signal.getsignal(signum) for signum in managed_signals}
    touched = False
    p = inj = None
    try:
        for signum in managed_signals:
            if signum != signal.SIGINT:
                signal.signal(signum, terminate)
        try:
            with ExitStack() as files:
                try:
                    if replacement is not None:
                        # Mark before writing so even a partial write is restored.
                        touched = True
                        ini.write_bytes(replacement)
                    out = files.enter_context(open(a.o, "wb")) if a.o else None
                    p = subprocess.Popen([exe] + a.prog[1:], cwd=a.C,
                                         stdout=out, stderr=subprocess.STDOUT if out else None)
                    if a.i:
                        inj_out = files.enter_context(open((a.o or "run") + ".inject", "w"))
                        inj = subprocess.Popen(
                            [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "inject.py"), a.i],
                            stdout=inj_out, stderr=subprocess.STDOUT)
                    try:
                        rc = p.wait(timeout=a.t)
                        print(f"exit code 0x{rc & 0xffffffff:08X}")
                    except subprocess.TimeoutExpired:
                        stop(p)
                        print(f"timeout after {a.t:g}s (killed)")
                finally:
                    # A second Ctrl-C/termination request must not interrupt restoration.
                    for signum in managed_signals:
                        signal.signal(signum, signal.SIG_IGN)
                    try:
                        stop(inj)
                    finally:
                        stop(p)
        finally:
            # All children have stopped and all log files have closed at this point.
            if touched:
                if original is not None:
                    ini.write_bytes(original)
                else:
                    ini.unlink(missing_ok=True)
    finally:
        for signum, handler in handlers.items():
            signal.signal(signum, handler)


if __name__ == "__main__":
    main()
