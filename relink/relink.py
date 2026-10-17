#!/usr/bin/env python3
"""relink.py — convert an i386 ELF32 ET_EXEC/ET_DYN into a PE32 image for lxrt.dll.

EXE: ImageBase 0x08040000, every ELF vaddr kept identical (RVA = vaddr - ImageBase), relocs stripped.
DLL: RVA = vaddr + 0x1000, load bias = ImageBase + 0x1000, R_386_RELATIVE pre-applied and emitted
     as IMAGE_REL_BASED_HIGHLOW; all symbolic relocations are left to lxrt's binder.
Both: `%gs:0x14` stack-canary accesses are rewritten to an absolute per-module slot.
Appended sections: .lxdata (descriptor, imports, exports), .lxtext (entry stub), .reloc.
"""
import argparse
import os
import re
import struct
import subprocess
import sys

PAGE = 0x1000
FILE_ALIGN = 0x200
DLL_RVA_SHIFT = 0x1000

PT_LOAD, PT_DYNAMIC, PT_TLS = 1, 2, 7
PF_X, PF_W, PF_R = 1, 2, 4
DT_NULL, DT_NEEDED, DT_STRTAB, DT_REL, DT_RELSZ, DT_SONAME, DT_TEXTREL, DT_JMPREL, DT_PLTRELSZ = 0, 1, 5, 17, 18, 14, 22, 23, 2
DT_RELA = 7
R_386_RELATIVE, R_386_IRELATIVE = 8, 42

SCN_CODE, SCN_IDATA, SCN_UDATA = 0x20, 0x40, 0x80
SCN_DISCARD, SCN_X, SCN_R, SCN_W = 0x02000000, 0x20000000, 0x40000000, 0x80000000

DEFAULT_SYSTEM = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "ld-linux.so.2",
    "libGL.so.1", "libGLU.so.1", "libX11.so.6", "libXxf86vm.so.1", "libasound.so.2",
    "libusb-0.1.so.4", "libusb-1.0.so.0",
}


def die(msg):
    sys.exit(f"relink: error: {msg}")


def align(v, a):
    return (v + a - 1) & ~(a - 1)


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            die(f"{path}: not a little-endian ELF32 file")
        (self.type, machine, _, self.entry, self.phoff, self.shoff, _, _, self.phentsize,
         self.phnum, self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        if machine != 3:
            die(f"{path}: e_machine {machine} is not EM_386")
        if self.type not in (2, 3):
            die(f"{path}: e_type {self.type} is neither ET_EXEC nor ET_DYN")
        self.phdrs = [struct.unpack_from("<8I", d, self.phoff + i * self.phentsize) for i in range(self.phnum)]
        self.loads = [p for p in self.phdrs if p[0] == PT_LOAD]
        self.shdrs = [struct.unpack_from("<10I", d, self.shoff + i * self.shentsize) for i in range(self.shnum)] if self.shoff else []
        dyn = [p for p in self.phdrs if p[0] == PT_DYNAMIC]
        self.dynamic = []
        self.dyn_vaddr = 0
        if dyn:
            self.dyn_vaddr = dyn[0][2]
            off = dyn[0][1]
            while True:
                tag, val = struct.unpack_from("<iI", d, off)
                if tag == DT_NULL:
                    break
                self.dynamic.append((tag, val))
                off += 8

    def vaddr_to_off(self, va):
        for p in self.loads:
            if p[2] <= va < p[2] + p[4]:
                return p[1] + va - p[2]
        die(f"{self.path}: vaddr {va:#x} not file-backed")

    def dyn(self, tag):
        return [v for t, v in self.dynamic if t == tag]

    def cstr(self, off):
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode()

    def dynstr(self, idx):
        return self.cstr(self.vaddr_to_off(self.dyn(DT_STRTAB)[0]) + idx)

    def needed(self):
        return [self.dynstr(v) for v in self.dyn(DT_NEEDED)]

    def soname(self):
        s = self.dyn(DT_SONAME)
        return self.dynstr(s[0]) if s else None

    def rels(self):
        """All dynamic REL entries (offset, type, symidx)."""
        out = []
        for tab, size in ((DT_REL, DT_RELSZ), (DT_JMPREL, DT_PLTRELSZ)):
            if self.dyn(tab):
                off = self.vaddr_to_off(self.dyn(tab)[0])
                for i in range(self.dyn(size)[0] // 8):
                    r_off, info = struct.unpack_from("<II", self.data, off + i * 8)
                    out.append((r_off, info & 0xFF, info >> 8))
        if self.dyn(DT_RELA):
            die(f"{self.path}: DT_RELA is not valid for i386")
        return out


class Image:
    """Memory image of the ELF PT_LOAD segments, grouped into page-aligned PE sections."""

    def __init__(self, elf, rva_of):
        self.sections = []  # [start_va, end_va, flags_PF, bytearray]
        for p in sorted(elf.loads, key=lambda p: p[2]):
            _, off, va, _, filesz, memsz, flags, _ = p
            lo, hi = va & ~(PAGE - 1), align(va + memsz, PAGE)
            if self.sections and lo < self.sections[-1][1]:
                s = self.sections[-1]
                s[1] = max(s[1], hi)
                s[2] |= flags
            else:
                self.sections.append([lo, hi, flags, bytearray()])
            s = self.sections[-1]
            need = va - s[0] + filesz
            if len(s[3]) < need:
                s[3].extend(b"\0" * (need - len(s[3])))
            s[3][va - s[0]:need] = elf.data[off:off + filesz]
        self.rva_of = rva_of

    def find(self, va, n=1):
        for s in self.sections:
            if s[0] <= va and va + n <= s[0] + len(s[3]):
                return s
        return None

    def read32(self, va):
        s = self.find(va, 4)
        if not s:
            die(f"read of non file-backed vaddr {va:#x}")
        return struct.unpack_from("<I", s[3], va - s[0])[0]

    def write32(self, va, v):
        s = self.find(va, 4)
        if not s:
            die(f"write to non file-backed vaddr {va:#x}")
        struct.pack_into("<I", s[3], va - s[0], v & 0xFFFFFFFF)

    def mark_writable(self, va):
        for s in self.sections:
            if s[0] <= va < s[1]:
                s[2] |= PF_W
                return
        die(f"relocation target {va:#x} outside any PT_LOAD")


OBJDUMP_LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$")


def find_gs_sites(path, objdump):
    """Linear-sweep disassembly; returns [(vaddr, bytes, text)] for every instruction using %gs."""
    proc = subprocess.run([objdump, "-d", path], capture_output=True, text=True)
    if proc.returncode != 0:
        die(f"{objdump} failed: {proc.stderr.strip()}")
    sites = []
    for line in proc.stdout.splitlines():
        if "%gs" not in line:
            continue
        m = OBJDUMP_LINE.match(line)
        if not m:
            die(f"cannot parse objdump line: {line!r}")
        sites.append((int(m.group(1), 16), bytes.fromhex(m.group(2)), m.group(3).strip()))
    return sites


def rewrite_canary(ins):
    """Return (new_bytes, offset_of_abs32) for a %gs:0x14 access, or None if not a supported form."""
    if len(ins) < 6 or ins[0] != 0x65:
        return None
    body = ins[1:]
    if body[0] in (0xA1, 0xA3) and len(body) == 5 and body[1:5] == b"\x14\0\0\0":
        return lambda a: bytes([body[0]]) + struct.pack("<I", a) + b"\x90", 1
    i = 1 if body[0] == 0x0F else 0
    op = body[i]
    if op in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3):
        return None
    modrm = body[i + 1]
    if modrm & 0xC7 != 0x05 or body[i + 2:i + 6] != b"\x14\0\0\0":
        return None
    head, tail = body[:i + 2], body[i + 6:]
    return lambda a: head + struct.pack("<I", a) + tail + b"\x90", len(head)


def build_idata(base_rva, imports):
    """imports: [(dll_name, [symbol, ...])]. Returns (blob, import_dir_rva, import_dir_size,
    iat_rva, iat_size, {(dll, sym): iat_slot_rva})."""
    n = len(imports)
    desc_size = 20 * (n + 1)
    # Layout: descriptors | ILTs | IATs | hint/name + dll names.
    ilt_rva, off = [], desc_size
    for _, syms in imports:
        ilt_rva.append(off)
        off += 4 * (len(syms) + 1)
    iat_start = off
    iat_rva = []
    for _, syms in imports:
        iat_rva.append(off)
        off += 4 * (len(syms) + 1)
    iat_size = off - iat_start
    strings = bytearray()
    name_rva, hint_rva = [], []
    for dll, syms in imports:
        hint_rva.append([])
        for s in syms:
            if (off + len(strings)) & 1:
                strings += b"\0"
            hint_rva[-1].append(off + len(strings))
            strings += b"\0\0" + s.encode() + b"\0"
    for dll, _ in imports:
        name_rva.append(off + len(strings))
        strings += dll.encode() + b"\0"
    blob = bytearray(off) + strings
    slots = {}
    for k, (dll, syms) in enumerate(imports):
        struct.pack_into("<IIIII", blob, 20 * k, base_rva + ilt_rva[k], 0, 0, base_rva + name_rva[k], base_rva + iat_rva[k])
        for j, s in enumerate(syms):
            struct.pack_into("<I", blob, ilt_rva[k] + 4 * j, base_rva + hint_rva[k][j])
            struct.pack_into("<I", blob, iat_rva[k] + 4 * j, base_rva + hint_rva[k][j])
            slots[(dll, s)] = base_rva + iat_rva[k] + 4 * j
    return blob, base_rva, desc_size, base_rva + iat_start, iat_size, slots


def build_edata(base_rva, dll_name, exports):
    """exports: [(name, rva)] sorted by name."""
    exports = sorted(exports)
    n = len(exports)
    eat, npt, ont = 40, 40 + 4 * n, 40 + 8 * n
    strings_off = ont + 2 * n
    strings = bytearray(dll_name.encode() + b"\0")
    name_offs = []
    for name, _ in exports:
        name_offs.append(strings_off + len(strings))
        strings += name.encode() + b"\0"
    blob = bytearray(strings_off) + strings
    struct.pack_into("<IIHHIIIIIII", blob, 0, 0, 0, 0, 0, base_rva + strings_off, 1, n, n,
                     base_rva + eat, base_rva + npt, base_rva + ont)
    for i, (name, rva) in enumerate(exports):
        struct.pack_into("<I", blob, eat + 4 * i, rva)
        struct.pack_into("<I", blob, npt + 4 * i, base_rva + name_offs[i])
        struct.pack_into("<H", blob, ont + 2 * i, i)
    return blob


def build_reloc(rvas):
    out = bytearray()
    pages = {}
    for r in sorted(set(rvas)):
        pages.setdefault(r & ~(PAGE - 1), []).append(r & (PAGE - 1))
    for page, offs in sorted(pages.items()):
        entries = [(3 << 12) | o for o in offs]
        if len(entries) & 1:
            entries.append(0)
        out += struct.pack("<II", page, 8 + 2 * len(entries)) + struct.pack(f"<{len(entries)}H", *entries)
    return out


def section_chars(pf):
    c = SCN_R
    if pf & PF_X:
        c |= SCN_X | SCN_CODE
    else:
        c |= SCN_IDATA
    if pf & PF_W:
        c |= SCN_W
    return c


def relink(args):
    elf = Elf(args.input)
    is_exe = elf.type == 2
    if is_exe and args.dll:
        die("ET_EXEC input cannot become a DLL")
    if not is_exe and args.exe:
        die("ET_DYN executables (PIE) are not supported")
    # EXE: highest 64 KiB-aligned base that leaves one page for PE headers below the first
    # PT_LOAD (piu: 0x08048000 -> 0x08040000). Vaddrs stay identical.
    first_va = min(p[2] for p in elf.loads) & ~(PAGE - 1)
    image_base = ((first_va - PAGE) & ~0xFFFF) if is_exe else int(args.base, 0)
    if image_base & 0xFFFF:
        die(f"image base {image_base:#x} is not 64 KiB aligned")
    bias = 0 if is_exe else image_base + DLL_RVA_SHIFT
    rva_of = (lambda va: va - image_base) if is_exe else (lambda va: va + DLL_RVA_SHIFT)
    img = Image(elf, rva_of)
    if rva_of(img.sections[0][0]) < PAGE:
        die("first PT_LOAD would overlap the PE headers")
    if not any(p[1] == 0 and p[4] >= elf.phoff + elf.phnum * elf.phentsize for p in elf.loads):
        die("ELF header/phdrs are not mapped by a PT_LOAD (unsupported)")
    hdr_va = next(p[2] for p in elf.loads if p[1] == 0)
    if elf.dyn(DT_TEXTREL):
        die("DT_TEXTREL is not supported")

    base_relocs = []  # VAs (ELF space) needing HIGHLOW fixups

    # 1. Dynamic relocations: pre-apply RELATIVE for DLLs, make symbolic targets writable.
    for r_off, r_type, sym in elf.rels():
        if r_type == R_386_RELATIVE:
            if is_exe:
                die("R_386_RELATIVE in ET_EXEC")
            img.write32(r_off, img.read32(r_off) + bias)
            base_relocs.append(r_off)
        else:
            if r_type == R_386_IRELATIVE:
                die("R_386_IRELATIVE (ifunc) is not supported")
            img.mark_writable(r_off)

    # 2. Canary rewrite. The slot VA is only known after layout; collect sites first.
    protected = []
    for spec in args.protect or []:
        a, n = spec.split(":")
        protected.append((int(a, 0), int(n, 0)))
    canary_sites = []
    for va, ins, text in find_gs_sites(args.input, args.objdump):
        rw = rewrite_canary(ins)
        if rw is None:
            die(f"unsupported %gs access at {va:#x}: {ins.hex(' ')}  {text}")
        for a, n in protected:
            if va < a + n and a < va + len(ins):
                die(f"%gs site {va:#x} overlaps protected window {a:#x}+{n}")
        canary_sites.append((va, ins, rw))

    # 3. Layout appended sections after the highest ELF page.
    end_va = img.sections[-1][1]
    lxdata_rva = rva_of(end_va)
    name = args.name or elf.soname() or os.path.basename(args.input)
    system = DEFAULT_SYSTEM | set(args.system or [])
    guest_deps = [n for n in elf.needed() if n not in system]
    imports = [("lxrt.dll", ["lx_start" if is_exe else "lx_register"])]
    for dep in (args.preload or []) + guest_deps:
        imports.append((dep + ".dll", ["lx_module"]))

    # .lxdata: descriptor (52 bytes) | name | idata | edata
    desc_size = 13 * 4
    name_bytes = name.encode() + b"\0"
    idata_off = align(desc_size + len(name_bytes), 16)
    idata, imp_rva, imp_size, iat_rva, iat_size, slots = build_idata(lxdata_rva + idata_off, imports)
    edata_off = align(idata_off + len(idata), 16)
    edata = b"" if is_exe else build_edata(lxdata_rva + edata_off, os.path.basename(args.output), [("lx_module", lxdata_rva)])
    lxdata = bytearray(align(edata_off + len(edata), 4))
    lxdata[idata_off:idata_off + len(idata)] = idata
    lxdata[edata_off:edata_off + len(edata)] = edata
    lxdata[desc_size:desc_size + len(name_bytes)] = name_bytes
    lxdata_vsize = align(len(lxdata), PAGE)

    def va_of_rva(rva):
        return image_base + rva

    desc_va = va_of_rva(lxdata_rva)
    canary_va = desc_va + 12 * 4
    phdr_va = hdr_va + elf.phoff + bias
    flags = (1 if is_exe else 0) | (2 if args.preload_flag else 0)
    fields = [0x314D584C, 1, flags, image_base, bias, hdr_va + bias, phdr_va, elf.phnum,
              (elf.dyn_vaddr + bias) if elf.dyn_vaddr else 0, (elf.entry + bias) if elf.entry else 0,
              canary_va, desc_va + desc_size, 0]
    struct.pack_into("<13I", lxdata, 0, *fields)
    # Absolute pointers inside the descriptor need fixups in DLLs (indices 3..6, 8..11).
    desc_reloc_rvas = []
    for i in (3, 4, 5, 6, 8, 9, 10, 11):
        if fields[i]:
            desc_reloc_rvas.append(lxdata_rva + 4 * i)

    # .lxtext: entry stub
    lxtext_rva = lxdata_rva + lxdata_vsize
    stub = bytearray()
    stub_relocs = []  # offsets in stub holding absolute VAs
    if is_exe:
        # push desc ; call [IAT lx_start] ; int3
        stub += b"\x68" + struct.pack("<I", desc_va)
        stub += b"\xFF\x15" + struct.pack("<I", va_of_rva(slots[("lxrt.dll", "lx_start")])) + b"\xCC"
    else:
        # DllMain(hinst, reason, reserved): if reason==1 lx_register(&desc); return TRUE
        stub += b"\x83\x7C\x24\x08\x01"           # cmp dword [esp+8], 1
        stub += b"\x75\x0E"                       # jne +14
        stub += b"\x68"; stub_relocs.append(len(stub)); stub += struct.pack("<I", desc_va)
        stub += b"\xFF\x15"; stub_relocs.append(len(stub))
        stub += struct.pack("<I", va_of_rva(slots[("lxrt.dll", "lx_register")]))
        stub += b"\x83\xC4\x04"                   # add esp, 4
        stub += b"\xB8\x01\0\0\0"                 # mov eax, 1
        stub += b"\xC2\x0C\x00"                   # ret 12
        assert stub[6] == 14 and stub[7 + 14] == 0xB8  # jne lands on `mov eax, 1`
    lxtext_vsize = align(len(stub), PAGE)

    # Apply canary rewrites now that the slot address is known.
    for va, ins, (make, abs_off) in canary_sites:
        new = make(canary_va)
        assert len(new) == len(ins)
        s = img.find(va, len(ins))
        if not s:
            die(f"%gs site {va:#x} not in file-backed memory")
        if s[3][va - s[0]:va - s[0] + len(ins)] != ins:
            die(f"%gs site {va:#x}: objdump bytes do not match image")
        s[3][va - s[0]:va - s[0] + len(ins)] = new
        if not is_exe:
            base_relocs.append(va + abs_off)

    reloc_rvas = [rva_of(v) for v in base_relocs] + desc_reloc_rvas + [lxtext_rva + o for o in stub_relocs]
    reloc = build_reloc(reloc_rvas) if not is_exe else b""
    reloc_rva = lxtext_rva + lxtext_vsize

    # 4. Section table (contiguous; padding sections fill gaps).
    secs = []  # (name, rva, vsize, data, chars)
    cursor = PAGE
    for lo, hi, pf, data in img.sections:
        rva = rva_of(lo)
        if rva > cursor:
            secs.append((b".lxpad", cursor, rva - cursor, b"", SCN_UDATA | SCN_R))
        elif rva < cursor:
            die(f"segment at {lo:#x} overlaps previous section")
        name_s = b".text" if pf & PF_X else (b".data" if pf & PF_W else b".rdata")
        secs.append((name_s, rva, hi - lo, bytes(data).rstrip(b"\0"), section_chars(pf)))
        cursor = rva + (hi - lo)
    assert cursor == lxdata_rva
    secs.append((b".lxdata", lxdata_rva, lxdata_vsize, bytes(lxdata), SCN_IDATA | SCN_R | SCN_W))
    secs.append((b".lxtext", lxtext_rva, lxtext_vsize, bytes(stub), SCN_CODE | SCN_R | SCN_X))
    size_of_image = lxtext_rva + lxtext_vsize
    if reloc:
        secs.append((b".reloc", reloc_rva, align(len(reloc), PAGE), bytes(reloc), SCN_IDATA | SCN_R | SCN_DISCARD))
        size_of_image = reloc_rva + align(len(reloc), PAGE)

    # 5. Headers.
    nsec = len(secs)
    hdr_size = 0x40 + 4 + 20 + 224 + 40 * nsec
    size_of_headers = align(hdr_size, FILE_ALIGN)
    if size_of_headers > PAGE:
        die("too many sections for a one-page header")
    raw_ptr = size_of_headers
    sec_table = bytearray()
    body = bytearray()
    for nm, rva, vsize, data, chars in secs:
        raw = align(len(data), FILE_ALIGN)
        ptr = raw_ptr if raw else 0
        sec_table += struct.pack("<8sIIIIIIHHI", nm, vsize, rva, raw, ptr, 0, 0, 0, 0, chars)
        body += data + b"\0" * (raw - len(data))
        raw_ptr += raw

    code = [s for s in secs if s[4] & SCN_CODE]
    size_code = sum(align(len(s[3]), FILE_ALIGN) for s in code)
    size_idata = sum(align(len(s[3]), FILE_ALIGN) for s in secs if s[4] & SCN_IDATA)
    size_udata = sum(s[2] for s in secs if s[4] & SCN_UDATA)
    base_of_code = code[0][1] if code else 0
    base_of_data = next((s[1] for s in secs if not s[4] & SCN_CODE), 0)

    chars = 0x0002 | 0x0100  # EXECUTABLE_IMAGE | 32BIT_MACHINE
    if is_exe:
        chars |= 0x0001  # RELOCS_STRIPPED
    else:
        chars |= 0x2000  # DLL
    dll_chars = 0x0100  # NX_COMPAT
    if not is_exe:
        dll_chars |= 0x0040  # DYNAMIC_BASE
    # Linux code does not probe the stack, so it can jump past a Windows guard page: commit the
    # whole 8 MiB main-thread stack (Linux default RLIMIT_STACK) up front.
    stack_reserve = 8 << 20 if is_exe else 1 << 20
    stack_commit = stack_reserve if is_exe else 0x1000

    dirs = [(0, 0)] * 16
    if edata:
        dirs[0] = (lxdata_rva + edata_off, len(edata))
    dirs[1] = (imp_rva, imp_size)
    if reloc:
        dirs[5] = (reloc_rva, len(reloc))
    dirs[12] = (iat_rva, iat_size)

    opt = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
                      0x10B, 14, 0, size_code, size_idata, size_udata, lxtext_rva, base_of_code, base_of_data,
                      image_base, PAGE, FILE_ALIGN, 6, 0, 0, 0, 6, 0, 0, size_of_image, size_of_headers, 0,
                      args.subsystem, dll_chars, stack_reserve, stack_commit, 1 << 20, 0x1000, 0, 16)
    opt += b"".join(struct.pack("<II", *d) for d in dirs)
    assert len(opt) == 224
    coff = struct.pack("<HHIIIHH", 0x14C, nsec, 0, 0, 0, 224, chars)
    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    headers = bytes(dos) + b"PE\0\0" + coff + opt + bytes(sec_table)
    headers += b"\0" * (size_of_headers - len(headers))

    with open(args.output, "wb") as f:
        f.write(headers + bytes(body))

    if args.verbose:
        print(f"{args.input} -> {args.output}: base {image_base:#x} bias {bias:#x} "
              f"sections {nsec} canary sites {len(canary_sites)} base relocs {len(reloc_rvas) if reloc else 0} "
              f"imports {[d for d, _ in imports]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--exe", action="store_true", help="require ET_EXEC input")
    g.add_argument("--dll", action="store_true", help="require ET_DYN input")
    ap.add_argument("--base", default="0x10000000", help="preferred ImageBase for DLLs")
    ap.add_argument("--name", help="module name (default: DT_SONAME or file name)")
    ap.add_argument("--preload", action="append", help="EXE: preload module soname (imports <soname>.dll)")
    ap.add_argument("--preload-flag", action="store_true", help="mark this DLL as a preload module")
    ap.add_argument("--system", action="append", help="extra DT_NEEDED soname provided natively")
    ap.add_argument("--protect", action="append", help="vaddr:len window the canary rewrite must not touch")
    ap.add_argument("--subsystem", type=int, default=3, help="PE subsystem (3 console, 2 GUI)")
    ap.add_argument("--objdump", default=os.environ.get("OBJDUMP", "llvm-objdump"))
    ap.add_argument("-v", "--verbose", action="store_true")
    relink(ap.parse_args())


if __name__ == "__main__":
    main()
