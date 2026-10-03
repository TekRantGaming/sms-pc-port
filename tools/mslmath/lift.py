#!/usr/bin/env python3
"""lift.py BINUTILS OBJ SYMBOL NAME [OBJ SYMBOL NAME ...] > lifted.s

Copies functions out of the DOL's objects (the decomp's split of the original,
build/GMSE01/obj) into an assembly file for tools/mslmath, under new names:
the weak copies the game calls (std::fmodf in wireTrap.o, std::sqrtf in
MAnmSound.o, JGeometry::TUtil<f32>::mod in koopajr.o) sit in objects that
cannot be linked on their own. Each function is its instruction words,
unchanged; its relocations are re-emitted against the same symbols (calls to
the runtime, which check.sh links from runtime.o) or against copies of the
small-data constants it loads (the same bytes, in .sdata2), and its absolute
references to the object's .bss (tools/gxmath: GXDrawSphere's saved vertex
state) against a zeroed copy of that section."""
import re, subprocess, sys, tempfile

B = sys.argv[1]
jobs = sys.argv[2:]
assert len(jobs) % 3 == 0


def tool(name, *a):
    return subprocess.check_output([B + '/powerpc-eabi-' + name] + list(a), text=True)


def section_bytes(obj, sec):
    # a file, not /dev/stdout: objcopy writes nothing to a pipe
    with tempfile.NamedTemporaryFile(suffix='.bin') as f:
        subprocess.run([B + '/powerpc-eabi-objcopy', '-O', 'binary', '-j', sec, obj, f.name],
                       capture_output=True, check=True)
        out = open(f.name, 'rb').read()
    assert out, (obj, sec)
    return out


def symbols(obj):
    syms, secnames = {}, {}
    for line in tool('readelf', '-SW', obj).splitlines():
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+\S+\s+\S+\s+([0-9a-f]+)', line)
        if m:
            secnames[m.group(1)] = m.group(2)
            secsizes[(obj, m.group(2))] = int(m.group(3), 16)
    for line in tool('readelf', '-sW', obj).splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(':') and f[6].isdigit():
            syms[f[7]] = (int(f[1], 16), int(f[2]), secnames[f[6]], f[4])
    return syms


print('\t.section .text')
data = []
bss = []
secsizes = {}
for i in range(0, len(jobs), 3):
    obj, sym, name = jobs[i:i + 3]
    syms = symbols(obj)
    addr, size, sec, bind = syms[sym]
    assert sec == '.text' and size > 0, sym
    text = section_bytes(obj, '.text')[addr:addr + size]
    relocs = []
    cur = None
    for line in tool('readelf', '-rW', obj).splitlines():
        m = re.match(r"Relocation section '\.rela(\S+)'", line)
        if m:
            cur = m.group(1)
            continue
        f = line.split()
        if cur == '.text' and len(f) >= 5 and re.match(r'[0-9a-f]{8}$', f[0]):
            off = int(f[0], 16)
            if addr <= off < addr + size:
                target, add = f[4], int(f[6], 16) if len(f) > 6 else 0
                relocs.append((off - addr, f[2], target, add))
    print('\t.globl %s\n\t.balign 4\n%s:' % (name, name))
    for off, typ, target, add in relocs:
        if typ == 'R_PPC_REL24':
            ref = target
        elif typ in ('R_PPC_ADDR16_HA', 'R_PPC_ADDR16_LO') and syms[target][2] == '.bss':
            ref = '%s_bss' % name
            if (ref, secsizes[(obj, '.bss')]) not in bss:
                bss.append((ref, secsizes[(obj, '.bss')]))
            add += syms[target][0]
        elif typ == 'R_PPC_EMB_SDA21':
            t_addr, t_size, t_sec, _ = syms[target]
            assert t_sec == '.sdata2', (target, t_sec)
            ref = '%s_k%d' % (name, len(data))
            data.append((ref, section_bytes(obj, '.sdata2')[t_addr:t_addr + t_size]))
        else:
            sys.exit('%s: relocation %s at %#x not handled' % (sym, typ, off))
        print('\t.reloc %s+%d, %s, %s%s' % (name, off, typ, ref, '+%d' % add if add else ''))
    for j in range(0, size, 4):
        print('\t.4byte 0x%s' % text[j:j + 4].hex())
print('\t.section .sdata2')
for ref, b in data:
    print('\t.balign 8\n%s:\n\t.byte %s' % (ref, ', '.join('0x%02x' % x for x in b)))
if bss:
    print('\t.section .bss')
    for ref, size in bss:
        print('\t.balign 8\n%s:\n\t.space %d' % (ref, size))
