#!/usr/bin/env python3
"""The inputs of decomp-patches/fma/, and a check that the patch is current.

tools/fmacontract/fmastamp.py [--check] [--patch FILE] [--decomp DIR]

fmarewrite.py generates decomp-patches/fma/0001-mwcc-fused-multiply-adds.patch
from the decomp's source (src/, include/ and libs/ as they are on disk, so an
uncommitted edit counts) with the other decomp-patches/*.patch applied in
name order. The patch's first line records a hash of those inputs:

  fmarewrite-inputs: <sha256>

With --check, this recomputes the hash and exits 1, saying how to regenerate
the patch, when it was generated from other inputs. cmake/patches.cmake runs
it at configure time and the sms_fma_check target at every build when
SMS_FMA_CONTRACT is on, and tools/update-decomp.sh after moving the decomp.
Without --check it prints the hash and the manifest it is computed from.
Needs Python 3 only (no libclang, no git).

A change to the rewriter itself is not an input here: whoever changes its
model regenerates the patch.
"""
import argparse, glob, hashlib, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PATCH = os.path.join(ROOT, 'decomp-patches', 'fma', '0001-mwcc-fused-multiply-adds.patch')
TAG = 'fmarewrite-inputs: '
# the decomp's source trees the port compiles and includes
TREES = ('src', 'include', 'libs')
# editor and patch(1) leftovers are not source
SKIP = ('~', '.orig', '.rej', '.bak', '.swp')


def sha256_file(path):
    with open(path, 'rb') as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def port_patches():
    """The patches fmarewrite.py's input tree is made of, in the order
    cmake/patches.cmake applies them (decomp-patches/fma/ not included)."""
    return sorted(glob.glob(os.path.join(ROOT, 'decomp-patches', '*.patch')))


def tree_hash(decomp):
    """Every source file under the decomp's TREES, by path and content."""
    h = hashlib.sha256()
    n = 0
    for top in TREES:
        for d, dirs, files in os.walk(os.path.join(decomp, top)):
            dirs[:] = sorted(x for x in dirs if not x.startswith('.'))
            for f in sorted(files):
                if f.startswith('.') or f.endswith(SKIP):
                    continue
                p = os.path.join(d, f)
                rel = os.path.relpath(p, decomp).replace(os.sep, '/')
                with open(p, 'rb') as fh:
                    data = fh.read()
                h.update(b'%s\0%d\0' % (rel.encode(), len(data)))
                h.update(data)
                n += 1
    return h.hexdigest(), n


def manifest(decomp):
    decomp = os.path.realpath(decomp)
    th, n = tree_hash(decomp)
    if n == 0:
        sys.exit('fmastamp: no source under %s; the fma patch cannot be checked '
                 '(git submodule update --init decomp)' % decomp)
    lines = ['decomp %s (%d files)' % (th, n)]
    for p in port_patches():
        lines.append('patch %s %s' % (os.path.basename(p), sha256_file(p)))
    return lines


def inputs_hash(decomp):
    lines = manifest(decomp)
    return hashlib.sha256(('\n'.join(lines) + '\n').encode()).hexdigest(), lines


def recorded(patch):
    try:
        with open(patch, 'rb') as fh:
            first = fh.readline().decode('utf-8', 'replace').strip()
    except OSError:
        return None
    return first[len(TAG):] if first.startswith(TAG) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--patch', default=PATCH)
    ap.add_argument('--decomp', default=os.path.join(ROOT, 'decomp'))
    args = ap.parse_args()
    h, lines = inputs_hash(args.decomp)
    if not args.check:
        print(TAG + h)
        for l in lines:
            print('  ' + l)
        return
    rec = recorded(args.patch)
    if rec == h:
        return
    why = 'has no %s line' % TAG.strip() if rec is None else 'was generated from another decomp or other decomp-patches'
    sys.stderr.write(
        'error: %s is stale: it %s.\n'
        'Regenerate it (needs libclang 18: pip install clang==18.1.8, and the linux-32 and linux-64 builds configured):\n'
        '  tools/fmacontract/fmarewrite.py --patch decomp-patches/fma/0001-mwcc-fused-multiply-adds.patch\n'
        'or build without the fused multiply-adds: cmake -DSMS_FMA_CONTRACT=OFF.\n'
        % (os.path.relpath(args.patch, ROOT), why))
    sys.exit(1)


if __name__ == '__main__':
    main()
