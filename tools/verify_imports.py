"""Verify x64 PE imports and resolve each imported symbol on the current Windows host."""
import argparse
import ctypes
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "comctl32.dll", "dwmapi.dll", "gdi32.dll",
    "gdiplus.dll", "kernel32.dll",
    "ole32.dll", "shell32.dll", "user32.dll", "winhttp.dll",
}
INVALID_APIS = {"InterlockedCompareExchange", "InterlockedExchange"}


def pe_imports(data):
    def unpack(fmt, offset):
        return struct.unpack_from("<" + fmt, data, offset)
    def c_string(offset):
        return data[offset:data.index(b"\0", offset)].decode("ascii")

    if data[:2] != b"MZ":
        raise ValueError("Missing DOS signature")
    (pe,) = unpack("I", 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Missing PE signature")
    machine, count, _, _, _, optional_size, _ = unpack("HHIIIHH", pe + 4)
    optional = pe + 24
    if machine != 0x8664 or unpack("H", optional)[0] != 0x20B:
        raise ValueError("Expected a Windows AMD64 PE32+ executable")
    if unpack("H", optional + 68)[0] != 2:
        raise ValueError("Expected a Windows GUI executable")
    for entry in (13, 14):
        if unpack("II", optional + 112 + 8 * entry) != (0, 0):
            raise ValueError("Unexpected delay imports or CLR header")
    sections = [
        unpack("IIII", optional + optional_size + index * 40 + 8)
        for index in range(count)
    ]

    def offset(rva):
        for _, start, size, raw in sections:
            if start <= rva < start + size:
                return rva - start + raw
        raise ValueError(f"Unmapped PE RVA {rva:#x}")

    imports = defaultdict(set)
    rva, size = unpack("II", optional + 120)
    if not rva or size < 40:
        raise ValueError("Missing PE import table")
    for descriptor in range(offset(rva), offset(rva) + size, 20):
        lookups, _, _, dll_name, iat = unpack("5I", descriptor)
        if not (dll_name or iat):
            break
        dll = c_string(offset(dll_name)).lower()
        if not re.fullmatch(r"[a-z0-9_.-]+", dll):
            raise ValueError(f"Invalid import DLL {dll!r}")
        thunk = offset(lookups or iat)
        while True:
            (slot,) = unpack("Q", thunk)
            if not slot:
                break
            if slot >> 63:
                imports[dll].add(slot & 0xFFFF)
            else:
                imports[dll].add(c_string(offset(slot) + 2))
            thunk += 8
    return imports


def verify(binary):
    imports = pe_imports(binary.read_bytes())
    if not imports or set(imports) - SYSTEM_DLLS:
        raise ValueError(f"Unexpected runtime DLLs: {set(imports) - SYSTEM_DLLS}")
    kernel = ctypes.WinDLL("kernel32.dll", winmode=0x800, use_last_error=True)
    resolve = kernel.GetProcAddress
    resolve.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    resolve.restype = ctypes.c_void_p
    total = 0
    for dll, symbols in sorted(imports.items()):
        library = ctypes.WinDLL(dll, winmode=0x800, use_last_error=True)
        if INVALID_APIS & symbols:
            raise ValueError(f"Invalid atomic import in {dll}")
        for symbol in symbols:
            name = ctypes.c_char_p(symbol.encode("ascii")) if isinstance(symbol, str) else None
            address = ctypes.cast(name, ctypes.c_void_p) if name is not None else ctypes.c_void_p(symbol)
            if not resolve(library._handle, address):
                raise ValueError(f"Missing export {dll}!{symbol}")
        print(f"{dll}: {len(symbols)} resolved")
        total += len(symbols)
    print(f"PASS: AMD64 Windows GUI, {total} resolved imports, {len(imports)} system DLLs")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    try:
        verify(args.executable)
    except (ValueError, OSError, struct.error) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
