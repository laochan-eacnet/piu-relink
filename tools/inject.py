#!/usr/bin/env python3
"""Drive the running game with keyboard input posted to its window (WM_KEYDOWN/WM_KEYUP with real
scancodes, the same messages a physical keyboard produces). lxgfx tracks key state from these
messages, so XNextEvent and XQueryKeymap (phoenixtools' virtual LXIO) both see them.

    python tools/inject.py SCRIPT            # waits for a top-level window owned by piu.exe

SCRIPT lines (# comments allowed):
    wait 2.5          sleep seconds
    tap 5             press+release (hold 0.12 s)
    hold S 1.0        press, hold N seconds, release
    repeat 10 tap E 0.4   repeat a tap every 0.4 s
Key names: single characters (Q E S Z C U O K M 5 ...), F1-F12, period, space, Return, Escape, Up, Down, Left, Right.
"""
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
WM_KEYDOWN, WM_KEYUP = 0x100, 0x101
VK = {"F%d" % i: 0x6F + i for i in range(1, 13)}
VK.update({"period": 0xBE, "space": 0x20, "Return": 0x0D, "Escape": 0x1B,
           "Up": 0x26, "Down": 0x28, "Left": 0x25, "Right": 0x27})
EXTENDED = {0x25, 0x26, 0x27, 0x28}


def vk_of(name):
    if name in VK:
        return VK[name]
    if len(name) == 1:
        return ord(name.upper())
    raise SystemExit(f"unknown key {name!r}")


def find_window(exe="piu.exe", timeout=120):
    found = []
    EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if h:
            buf = ctypes.create_unicode_buffer(1024)
            n = wintypes.DWORD(1024)
            if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) and buf.value.lower().endswith(exe):
                found.append(hwnd)
            kernel32.CloseHandle(h)
        return True

    deadline = time.time() + timeout
    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(EnumProc(cb), 0)
        if found:
            return found[0]
        time.sleep(0.5)
    raise SystemExit("game window not found")


def post(hwnd, vk, down):
    scan = user32.MapVirtualKeyW(vk, 0)
    lp = 1 | (scan << 16)
    if vk in EXTENDED:
        lp |= 1 << 24
    if not down:
        lp |= (1 << 30) | (1 << 31)
    user32.PostMessageW(hwnd, WM_KEYDOWN if down else WM_KEYUP, vk, lp)


def run(script):
    hwnd = find_window()
    print(f"inject: window {hwnd:#x}", flush=True)
    for raw in open(script):
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        cmd, args = line[0], line[1:]
        if cmd == "wait":
            time.sleep(float(args[0]))
        elif cmd == "tap":
            vk = vk_of(args[0])
            post(hwnd, vk, True); time.sleep(0.12); post(hwnd, vk, False); time.sleep(0.12)
        elif cmd == "hold":
            vk = vk_of(args[0])
            post(hwnd, vk, True); time.sleep(float(args[1])); post(hwnd, vk, False)
        elif cmd == "repeat":
            n, key, every = int(args[0]), args[2], float(args[3])
            vk = vk_of(key)
            for _ in range(n):
                post(hwnd, vk, True); time.sleep(0.08); post(hwnd, vk, False); time.sleep(max(0.0, every - 0.08))
        else:
            raise SystemExit(f"bad script line: {raw!r}")
        print(f"inject: {' '.join(line)}", flush=True)


if __name__ == "__main__":
    run(sys.argv[1])
