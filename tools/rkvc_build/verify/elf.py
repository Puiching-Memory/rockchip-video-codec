"""Pure-stdlib ELF inspection: architecture, interpreter, RPATH, SONAME,
DT_NEEDED closure and GNU symbol-version (``.gnu.version_r``) requirements.

No external tooling (``readelf``/``objdump``) and no target execution.  This is
the low level model consumed by ``verify.policy``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import re
import struct

ELF_MAGIC = b"\x7fELF"

EI_CLASS = 4
EI_DATA = 5
ELFCLASS32 = 1
ELFCLASS64 = 2
ELFDATA2LSB = 1
ELFDATA2MSB = 2

EM_MACHINE = {
    3: "x86",
    40: "ARM",
    62: "x86-64",
    183: "AArch64",
    243: "RISC-V",
}

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_GNU_STACK = 0x6474E551

PF_X = 1
PF_W = 2

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10
DT_SONAME = 14
DT_RPATH = 15
DT_RUNPATH = 29
DT_FLAGS = 30
DT_FLAGS_1 = 0x6FFFFFFB

SHT_STRTAB = 3
SHT_DYNSYM = 11


class ElfError(ValueError):
    """Raised when an ELF file is malformed or unreadable."""


@dataclass
class Dynamic:
    needed: list[str] = field(default_factory=list)
    soname: str | None = None
    rpath: list[str] = field(default_factory=list)
    runpath: list[str] = field(default_factory=list)
    flags: int = 0
    flags_1: int = 0


@dataclass
class ElfFile:
    path: str
    bits: int
    endian: str
    machine: str
    interpreter: str | None
    dynamic: Dynamic
    load_wx: bool
    stack_exec: bool
    #: Version strings scanned directly from ``.dynstr``.  Every version name a
    #: required symbol binds to is necessarily present in ``.dynstr``, so this
    #: is a robust source for the glibc baseline gate.
    dynstr_versions: list[str] = field(default_factory=list)

    @property
    def max_glibc(self) -> tuple[int, int, int] | None:
        """Highest ``GLIBC_x.y.z`` version referenced, or ``None``."""
        best: tuple[int, int, int] | None = None
        for name in self.dynstr_versions:
            if not name.startswith("GLIBC_"):
                continue
            version = _parse_version_string(name)
            if version is None:
                continue
            if best is None or version > best:
                best = version
        return best

    @property
    def all_versions(self) -> list[str]:
        return self.dynstr_versions


def _read_exact(stream, size: int, path: str) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise ElfError(f"{path}: truncated while reading {size} bytes")
    return data


def _read_string(dynstr: bytes, offset: int, path: str) -> str:
    if offset >= len(dynstr):
        raise ElfError(f"{path}: string offset {offset} out of range")
    end = dynstr.find(b"\x00", offset)
    if end < 0:
        end = len(dynstr)
    return dynstr[offset:end].decode("utf-8", "replace")


def _parse_version_string(name: str) -> tuple[int, int, int] | None:
    for prefix in ("GLIBC_", "GLIBCXX_", "CXXABI_", "GCC_"):
        if name.startswith(prefix):
            parts = name[len(prefix):].split(".")
            nums: list[int] = []
            for part in parts:
                try:
                    nums.append(int(part))
                except ValueError:
                    return None
            while len(nums) < 3:
                nums.append(0)
            return (nums[0], nums[1], nums[2])
    return None


_VERSION_RE = re.compile(rb"[A-Z][A-Z0-9]+_[0-9]+(?:\.[0-9]+)+")


def _scan_dynstr_versions(dynstr: bytes) -> list[str]:
    """Collect ``GLIBC_*``/``GLIBCXX_*``/``CXXABI_*``/``GCC_*`` strings from
    ``.dynstr``.  Strings are NUL-terminated, so match whole tokens only."""
    found: list[str] = []
    seen: set[str] = set()
    for token in _VERSION_RE.findall(dynstr):
        try:
            text = token.decode("ascii")
        except UnicodeDecodeError:
            continue
        if text in seen:
            continue
        seen.add(text)
        found.append(text)
    return found


def _parse_dynamic(section: bytes, bits: int, endian: str,
                   dynstr: bytes, path: str) -> Dynamic:
    dyn = Dynamic()
    if bits == ELFCLASS64:
        tag_fmt = endian + "qQ"
    else:
        tag_fmt = endian + "iI"
    size = 16 if bits == ELFCLASS64 else 8
    for off in range(0, len(section) - size + 1, size):
        tag, val = struct.unpack_from(tag_fmt, section, off)
        if tag == DT_NULL:
            break
        if tag == DT_NEEDED:
            dyn.needed.append(_read_string(dynstr, val, path))
        elif tag == DT_SONAME:
            dyn.soname = _read_string(dynstr, val, path)
        elif tag == DT_RPATH:
            dyn.rpath = _read_string(dynstr, val, path).split(":")
        elif tag == DT_RUNPATH:
            dyn.runpath = _read_string(dynstr, val, path).split(":")
        elif tag == DT_FLAGS:
            dyn.flags = int(val)
        elif tag == DT_FLAGS_1:
            dyn.flags_1 = int(val)
    return dyn


def read_elf(path: str) -> ElfFile:
    """Parse an ELF file at ``path``; raise ``ElfError`` if it is not ELF."""
    with open(path, "rb") as stream:
        ident = stream.read(16)
        if len(ident) < 16 or ident[:4] != ELF_MAGIC:
            raise ElfError(f"{path}: not an ELF file")

        bits = ident[EI_CLASS]
        if bits not in (ELFCLASS32, ELFCLASS64):
            raise ElfError(f"{path}: unsupported ELF class {bits}")
        endian = "<" if ident[EI_DATA] == ELFDATA2LSB else ">"

        if bits == ELFCLASS64:
            header = _read_exact(stream, 48, path)
            (e_type, e_machine, _v, _e, e_phoff, e_shoff, _f, _eh, e_phentsize,
             e_phnum, e_shentsize, e_shnum, _si) = struct.unpack_from(
                endian + "HHIQQQIHHHHHH", header, 0)
            phdr_fmt = endian + "IIQQQQQQ"
            phdr_size = 56
            shdr_fmt = endian + "IIQQQQIIQQ"
            shdr_size = 64
        else:
            header = _read_exact(stream, 36, path)
            (e_type, e_machine, _v, _e, e_phoff, e_shoff, _f, _eh, e_phentsize,
             e_phnum, e_shentsize, e_shnum, _si) = struct.unpack_from(
                endian + "HHIIIIIHHHHHH", header, 0)
            phdr_fmt = endian + "IIIIIIII"
            phdr_size = 32
            shdr_fmt = endian + "IIIIIIIIII"
            shdr_size = 40

        # ---- program headers ----
        interpreter: str | None = None
        dyn_sec: bytes | None = None
        load_wx: bool = False
        stack_exec: bool = False
        for i in range(e_phnum):
            stream.seek(e_phoff + i * e_phentsize)
            phdr = _read_exact(stream, phdr_size, path)
            if bits == ELFCLASS64:
                p_type, p_flags, p_offset, _va, _pa, p_filesz, _m, _al = \
                    struct.unpack_from(phdr_fmt, phdr, 0)
            else:
                p_type, p_offset, _va, _pa, p_filesz, _m, p_flags, _al = \
                    struct.unpack_from(phdr_fmt, phdr, 0)
            if p_type == PT_INTERP:
                stream.seek(p_offset)
                interpreter = stream.read(p_filesz).rstrip(b"\x00") \
                    .decode("utf-8", "replace")
            elif p_type == PT_DYNAMIC:
                stream.seek(p_offset)
                dyn_sec = stream.read(p_filesz)
            elif p_type == PT_LOAD:
                if (p_flags & PF_W) and (p_flags & PF_X):
                    load_wx = True
            elif p_type == PT_GNU_STACK:
                if p_flags & PF_X:
                    stack_exec = True

        # ---- section header scan ----
        # Collect (type, offset, size, link) by index.  The dynamic string table
        # is the SHT_STRTAB referenced by sh_link of the SHT_DYNSYM section --
        # selecting it by size is wrong when .strtab is larger than .dynstr.
        sections: list[tuple[int, int, int, int]] = []  # (type, off, size, link)
        dynsym_link: int = -1
        if e_shnum:
            for j in range(e_shnum):
                stream.seek(e_shoff + j * e_shentsize)
                shdr = _read_exact(stream, shdr_size, path)
                _n, sh_type, _fl, _ad, sh_offset, sh_size, sh_link, _inf, _al, _en = \
                    struct.unpack_from(shdr_fmt, shdr, 0)
                sections.append((sh_type, sh_offset, sh_size, sh_link))
                if sh_type == SHT_DYNSYM and dynsym_link < 0:
                    dynsym_link = sh_link

        dynstr: bytes = b""
        if dynsym_link >= 0:
            st_type, st_off, st_size, _lk = sections[dynsym_link]
            if st_type == SHT_STRTAB:
                stream.seek(st_off)
                dynstr = stream.read(st_size)

        dyn = _parse_dynamic(dyn_sec, bits, endian, dynstr, path) \
            if dyn_sec is not None else Dynamic()

        machine = EM_MACHINE.get(e_machine, f"emachine-{e_machine}")
        return ElfFile(
            path=path,
            bits=bits,
            endian=endian,
            machine=machine,
            interpreter=interpreter,
            dynamic=dyn,
            load_wx=load_wx,
            stack_exec=stack_exec,
            dynstr_versions=_scan_dynstr_versions(dynstr),
        )
