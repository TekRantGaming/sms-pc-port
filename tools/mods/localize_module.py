#!/usr/bin/env python3
"""Make a code mod's own definitions local, as Kuribo keeps each module apart.

    localize_module.py OBJCOPY module.o

Eclipse and the moveset both define gSettingsGroup, gSaveBnr... Linked into
one program those would collide, so each module's partially linked object
keeps none of its own definitions global. COMDAT definitions (inline functions,
vtables, typeinfo the module shares with BetterSunshineEngine and the game)
stay as they are: the linker keeps one copy of each group, and a local symbol
left in a discarded copy would be an error.

ELF: hidden definitions (the modules are built with -fvisibility=hidden).
COFF has no visibility: every external definition. Mach-O needs neither:
ld64 -r already makes hidden definitions local.
"""
import struct
import subprocess
import sys


def elf_names(data):
    bits = data[4]
    if data[5] != 1:
        sys.exit("big-endian ELF")
    if bits == 2:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
        hdr, sym, symsize = "<IIQQQQIIQQ", "<IBBHQQ", 24
    else:
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
        hdr, sym, symsize = "<IIIIIIIIII", "<IIIBBH", 16
    sections = [struct.unpack_from(hdr, data, shoff + i * shentsize) for i in range(shnum)]
    # (name, type, flags, addr, offset, size, link, info, align, entsize)
    grouped = set()
    for s in sections:
        if s[1] == 17:  # SHT_GROUP: a flag word, then member section indices
            grouped.update(struct.unpack_from("<%dI" % (s[5] // 4), data, s[4])[1:])
    names = []
    for s in sections:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[s[6]]
        for i in range(s[7], s[5] // symsize):  # globals follow the locals
            f = struct.unpack_from(sym, data, s[4] + i * symsize)
            if bits == 2:
                name, info, other, shndx = f[0], f[1], f[2], f[3]
            else:
                name, info, other, shndx = f[0], f[3], f[4], f[5]
            if (other & 3) != 2 or shndx == 0 or shndx >= 0xFF00 or shndx in grouped:
                continue  # not hidden, undefined, special, or in a COMDAT group
            end = data.index(b"\0", strtab[4] + name)
            names.append(data[strtab[4] + name:end].decode())
    return names


def coff_names(data):
    if struct.unpack_from("<HH", data, 0) == (0, 0xFFFF):  # /bigobj
        nsections, symptr, nsyms = struct.unpack_from("<III", data, 44)
        sechdr, symsize, symfmt = 56, 20, "<iHBB"
    else:
        nsections, _, symptr, nsyms, opthdr = struct.unpack_from("<HIIIH", data, 2)
        sechdr, symsize, symfmt = 20 + opthdr, 18, "<hHBB"
    comdat = set()
    for i in range(nsections):
        flags, = struct.unpack_from("<I", data, sechdr + i * 40 + 36)
        if flags & 0x1000:  # IMAGE_SCN_LNK_COMDAT
            comdat.add(i + 1)
    strings = symptr + nsyms * symsize
    names = []
    i = 0
    while i < nsyms:
        off = symptr + i * symsize
        raw = data[off:off + 8]
        section, _, storage, aux = struct.unpack_from(symfmt, data, off + 12)
        if storage == 2 and section > 0 and section not in comdat:  # IMAGE_SYM_CLASS_EXTERNAL, defined
            if raw[:4] == b"\0\0\0\0":
                at = strings + struct.unpack_from("<I", raw, 4)[0]
                names.append(data[at:data.index(b"\0", at)].decode())
            else:
                names.append(raw.rstrip(b"\0").decode())
        i += 1 + aux
    return names


def main(objcopy, path):
    data = open(path, "rb").read()
    names = elf_names(data) if data[:4] == b"\x7fELF" else coff_names(data)
    listing = path + ".local"
    with open(listing, "w") as f:
        f.write("".join(n + "\n" for n in names))
    subprocess.run([objcopy, "--localize-symbols=" + listing, path], check=True)
    print("localize_module: %d definitions made local in %s" % (len(names), path))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
