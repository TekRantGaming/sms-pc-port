#!/usr/bin/env python3
"""Move the code mods' static constructors out of the ones the process runs.

    macho_ctors.py sms_eclipse_all.o

The Mach-O counterpart of platform/mods/eclipse/lib/mod_ctors.ld: in the mods'
partially linked object, the constructor list (__DATA,__mod_init_func) becomes
the plain data section __DATA,__sms_mod_ctors, which dyld does not run and
platform/mods/modhooks.cpp walks when the modules load (sms_mod_start). Only
the section header changes; its pointers and their relocations stay as they are.
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
SECTION_TYPE = 0xFF
S_REGULAR = 0x0
S_MOD_INIT_FUNC_POINTERS = 0x9
S_INIT_FUNC_OFFSETS = 0x16
S_ATTR_NO_DEAD_STRIP = 0x10000000
NAME = b"__sms_mod_ctors"


def main(path):
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<8I", data, 0)
        if magic != MH_MAGIC_64:
            sys.exit(f"{path}: not a 64-bit Mach-O object")
        moved = 0
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<2I", data, off)
            if cmd == LC_SEGMENT_64:
                nsects = struct.unpack_from("<I", data, off + 64)[0]
                for i in range(nsects):
                    sect = off + 72 + i * 80
                    flags = struct.unpack_from("<I", data, sect + 64)[0]
                    kind = flags & SECTION_TYPE
                    if kind == S_INIT_FUNC_OFFSETS:
                        sys.exit(f"{path}: constructors are image offsets (__init_offsets), not pointers")
                    if kind != S_MOD_INIT_FUNC_POINTERS:
                        continue
                    data[sect:sect + 16] = NAME.ljust(16, b"\0")
                    data[sect + 16:sect + 32] = b"__DATA".ljust(16, b"\0")
                    flags = (flags & ~SECTION_TYPE) | S_REGULAR | S_ATTR_NO_DEAD_STRIP
                    struct.pack_into("<I", data, sect + 64, flags)
                    moved += 1
            off += size
        if moved > 1:
            sys.exit(f"{path}: {moved} constructor sections; expected one after ld -r")
        f.seek(0)
        f.write(data)
    print(f"macho_ctors: {'moved' if moved else 'no'} constructor list in {path}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
