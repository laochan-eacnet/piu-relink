#!/usr/bin/env python3
"""Dump EIP + heuristic stack backtrace of every thread of a running 32-bit (WOW64/Prism) process.
    python tools/stacks.py <pid | exe name>
Return-address candidates are stack words that point into a loaded module's image; guest ELF
vaddrs are printed for relinked modules (vaddr = addr - image_base - 0x1000 for DLLs, = addr for
the EXE)."""
import ctypes
import sys
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

TH32CS_SNAPTHREAD, TH32CS_SNAPPROCESS = 0x4, 0x2


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD), ("tpBasePri", wintypes.LONG),
                ("tpDeltaPri", wintypes.LONG), ("dwFlags", wintypes.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", wintypes.LONG), ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]


class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [("ContextFlags", wintypes.DWORD), ("Dr", wintypes.DWORD * 6), ("FloatSave", ctypes.c_byte * 112),
                ("SegGs", wintypes.DWORD), ("SegFs", wintypes.DWORD), ("SegEs", wintypes.DWORD), ("SegDs", wintypes.DWORD),
                ("Edi", wintypes.DWORD), ("Esi", wintypes.DWORD), ("Ebx", wintypes.DWORD), ("Edx", wintypes.DWORD),
                ("Ecx", wintypes.DWORD), ("Eax", wintypes.DWORD), ("Ebp", wintypes.DWORD), ("Eip", wintypes.DWORD),
                ("SegCs", wintypes.DWORD), ("EFlags", wintypes.DWORD), ("Esp", wintypes.DWORD), ("SegSs", wintypes.DWORD),
                ("ExtendedRegisters", ctypes.c_byte * 512)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            return e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    raise SystemExit(f"no process {name}")


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("GlblcntUsage", wintypes.DWORD), ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wintypes.DWORD), ("hModule", ctypes.c_void_p), ("szModule", wintypes.WCHAR * 256),
                ("szExePath", wintypes.WCHAR * 260)]


def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(0x8 | 0x10, pid)  # TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32
    e = MODULEENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    mods = []
    ok = k32.Module32FirstW(snap, ctypes.byref(e))
    while ok:
        base = e.modBaseAddr or 0
        if base < 1 << 32:
            mods.append((base, base + e.modBaseSize, e.szModule))
        ok = k32.Module32NextW(snap, ctypes.byref(e))
    return mods


def describe(mods, a):
    for lo, hi, n in mods:
        if lo <= a < hi:
            if n.endswith(".exe"):
                return f"{n} vaddr {a:#x}"
            if ".so" in n:
                return f"{n} vaddr {a - lo - 0x1000:#x}"
            return f"{n}+{a - lo:#x}"
    return None


def main():
    arg = sys.argv[1]
    pid = int(arg) if arg.isdigit() else find_pid(arg)
    hproc = k32.OpenProcess(0x1F0FFF, False, pid)
    mods = modules(pid)
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            ht = k32.OpenThread(0x1FFFFF, False, te.th32ThreadID)
            k32.Wow64SuspendThread(ht)
            ctx = WOW64_CONTEXT()
            ctx.ContextFlags = 0x10007  # WOW64_CONTEXT_FULL
            if k32.Wow64GetThreadContext(ht, ctypes.byref(ctx)):
                print(f"thread {te.th32ThreadID}: eip={ctx.Eip:#x} {describe(mods, ctx.Eip) or '?'} esp={ctx.Esp:#x} ebp={ctx.Ebp:#x}")
                buf = (wintypes.DWORD * 4096)()
                got = ctypes.c_size_t()
                k32.ReadProcessMemory(hproc, ctypes.c_void_p(ctx.Esp), buf, ctypes.sizeof(buf), ctypes.byref(got))
                shown = 0
                for i in range(got.value // 4):
                    d = describe(mods, buf[i])
                    if d and shown < 80:
                        print(f"    [esp+{i * 4:#x}] {d}")
                        shown += 1
            else:
                print(f"thread {te.th32ThreadID}: Wow64GetThreadContext failed ({ctypes.get_last_error()})")
            k32.ResumeThread(ht)
            k32.CloseHandle(ht)
        ok = k32.Thread32Next(snap, ctypes.byref(te))


if __name__ == "__main__":
    main()
