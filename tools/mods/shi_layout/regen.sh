#!/bin/bash
# regen.sh WORK_DIR: regenerate platform/mods/eclipse/shi-layout.patch (see README.md).
# Needs the Eclipse sources fetched by an SMS_ECLIPSE configure (SMS_ECLIPSE_SRC_DIR, default
# build-ecl/eclipse-src), and the plain 32- and 64-bit port builds (./build.sh's build/linux-32
# and build/linux-64; SMS_LAYOUT_BUILD32 and SMS_LAYOUT_BUILD64 to use others).
set -e
W=$(realpath "$1"); mkdir -p "$W"; export SHI_LAYOUT_WORK=$W
HERE=$(cd "$(dirname "$0")" && pwd); PORT=$(cd "$HERE/../../.." && pwd)
SRC=${SMS_ECLIPSE_SRC_DIR:-$PORT/build-ecl/eclipse-src}
E=$SRC; P=$PORT/platform/mods; X=$E/shi/include
# 1. SunshineHeaderInterface with the mechanical fixups only (not the layout patch)
git -C "$E/shi" checkout -q -- . && rm -f "$E/shi/.sms_port_shi-layout.patch"
# (and fixup_sources.py must redo every fixup at the end, even if the patch comes out the same)
rm -f "$E/.sms_port_fixups"
python3 -c "
import importlib.util
s=importlib.util.spec_from_file_location('fx','$P/eclipse/fixup_sources.py'); fx=importlib.util.module_from_spec(s); s.loader.exec_module(fx)
fx.apply('$E/shi', fx.SHI_FIXES)"
# 2. SHI's layouts as the mods compile it (-malign-double: the GameCube's 8-byte alignment)
mkdir -p "$W/ov/SMS/Camera"; (echo "#pragma once"; cat "$X/SMS/Camera/CubeManagerBase.hxx") > "$W/ov/SMS/Camera/CubeManagerBase.hxx"
sed 's/ \*probe.*//' "$HERE/shi_probe.cpp" | grep -v "^#" | grep . > "$W/probe_names.txt"
probe() {  # probe INCLUDE_ROOT OVERLAY OUT_PREFIX
  for a in 32 64; do ex=; [ $a = 32 ] && ex=-malign-double
    clang++ -m$a $ex -std=gnu++20 -c -g -fstandalone-debug -w -fms-extensions -I$P/eclipse/shim -I$P/include \
      -I$E/bse/include/BetterSMS -I$E/bse/include -I$2 -I$1 -I$1/JSystem -I$1/Dolphin -I$1/SMS -I$1/Kamek -I$1/Kuribo \
      -DNTSCU -DKURIBO_NO_TYPES -include sms_mod_prelude.h "$HERE/shi_probe.cpp" -o "$W/$3$a.o"
    (cd "$W" && gdb -q -batch -ex "py names_file='probe_names.txt'; out_file='$3$a.json'" -x "$HERE/export.py" "$3$a.o" | tail -1)
  done
}
probe "$X" "$W/ov" shi
# 3. the port's layouts (plain builds), for SHI's classes and for all classes
python3 - "$W" <<'PY'
import json,sys,os
sys.path.insert(0,os.environ.get('HERE_DIR','.'))
W=sys.argv[1]; d=json.load(open(W+'/shi32.json'))
alias=['TMapObjData','TMapObjCollisionInfo','TMapObjCollisionData','TMapObjSinkData','TMapObjSoundData','TMapObjSoundInfo','TMapObjAnimData','TMapObjHitDataTable','TMapObjHitInfo','TMapObjPhysicalData','TMapObjPhysicalInfo']
open(W+'/port_names.txt','w').write('\n'.join(sorted(set(d['types'])|set(alias))))
PY
for a in 32 64; do b=${SMS_LAYOUT_BUILD32:-$PORT/build/linux-32}/sms; [ $a = 64 ] && b=${SMS_LAYOUT_BUILD64:-$PORT/build/linux-64}/sms
  (cd "$W" && gdb -q -batch -ex "py names_file='port_names.txt'; out_file='port$a.json'" -x "$HERE/export.py" "$b" | tail -1)
  (cd "$W" && gdb -q -batch -ex "py out_file='names_port$a.txt'" -x "$HERE/allnames.py" "$b" | tail -1)
  (cd "$W" && gdb -q -batch -ex "py names_file='names_port$a.txt'; out_file='port${a}_all.json'" -x "$HERE/export.py" "$b" | tail -1)
done
# 4. classes whose CodeWarrior vtable pointer follows their first members
python3 "$HERE/vlate.py" "$PORT/decomp" "$W/vlate.json" > /dev/null
python3 "$HERE/vlate.py" "$X" "$W/vlate_shi.json" > /dev/null
# 5. the members the mods use, and the SHI classes they derive from
(cd "$W" && python3 "$HERE/used_members.py" "$X" "$E/bse" "$E/moveset" "$E/eclipse" > used_all.json)
python3 - "$W" "$E" <<'PY'
import json,sys,glob,re
W,E=sys.argv[1],sys.argv[2]; S=json.load(open(W+'/shi32.json'))['types']; b=set()
for f in glob.glob(E+'/*/include/**/*.h*',recursive=True)+glob.glob(E+'/*/src/**/*.[ch]*',recursive=True):
    if '/shi/' in f: continue
    for m in re.finditer(r'\b(?:class|struct)\s+\w+\s*(?:final\s*)?:\s*((?:public|private|protected)?\s*[\w:<>, *]+?)\s*\{',open(f,errors='replace').read()):
        for x in re.split(r',(?![^<]*>)',m.group(1)):
            x=re.sub(r'^(public|private|protected)\s+','',x.strip())
            if x in S: b.add(x)
json.dump(sorted(b),open(W+'/shi_bases.json','w'))
PY
# 6. the re-laid-out headers, checked
python3 "$HERE/gen_layout.py" "$X" "$W/shi_gl/include" > "$W/gen_layout.log"; head -1 "$W/gen_layout.log"
mkdir -p "$W/ov2/SMS/Camera"; (echo "#pragma once"; cat "$W/shi_gl/include/SMS/Camera/CubeManagerBase.hxx") > "$W/ov2/SMS/Camera/CubeManagerBase.hxx"
probe "$W/shi_gl/include" "$W/ov2" gl
python3 "$HERE/verify.py" 32 gl32.json | tail -8; python3 "$HERE/verify.py" 64 gl64.json | tail -8
# 7. the patch (LF), then the full fixups again
python3 - "$X" "$W/shi_gl/include" "$P/eclipse/shi-layout.patch" <<'PY'
import os,glob,sys,difflib
A,B,out=sys.argv[1:4]; parts=[]
for f in sorted(glob.glob(B+'/**/*',recursive=True)):
    if os.path.isdir(f): continue
    rel=os.path.relpath(f,B)
    a=open(os.path.join(A,rel),'rb').read().replace(b'\r\n',b'\n').decode('utf-8','surrogateescape')
    b=open(f,'rb').read().replace(b'\r\n',b'\n').decode('utf-8','surrogateescape')
    if a!=b:
        parts+=[l if l.endswith('\n') else l+'\n\\ No newline at end of file\n' for l in difflib.unified_diff(a.splitlines(True),b.splitlines(True),'a/include/'+rel,'b/include/'+rel,n=3)]
open(out,'w',encoding='utf-8',errors='surrogateescape').write(''.join(parts))
print('shi-layout.patch:',sum(1 for p in parts if p.startswith('+++')),'files')
PY
python3 "$P/eclipse/fixup_sources.py" "$E/eclipse" "$E/bse" "$E/shi" "$E/moveset"
