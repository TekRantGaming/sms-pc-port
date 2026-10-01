import sys,glob,json,re,os
ROOT=sys.argv[1] if len(sys.argv)>1 else '/home/user/sms-pc-port/decomp'
OUTF=sys.argv[2] if len(sys.argv)>2 else 'vlate.json'
sys.path.insert(0,'.')
import shiparse
# The decomp keeps the game's headers in include/ and the libraries' (JSystem,
# the SDK, MSL) in libs/*/include/; any other root (SHI's include/) is one tree.
ROOTS=[ROOT+'/include']+sorted(glob.glob(ROOT+'/libs/*/include')) if os.path.isdir(ROOT+'/libs') else [ROOT]
out={}
for f in [f for r in ROOTS for f in sorted(glob.glob(r+'/**/*.h*',recursive=True))]:
    try: raw,s,cl=shiparse.classes(f)
    except Exception: continue
    for c in cl:
        st=shiparse.statements(s,c['body'])
        firstv=None
        for i,(a,b,t,k) in enumerate(st):
            if re.search(r'\bvirtual\b',t): firstv=i; break
        if firstv is None: continue
        before=[n for a,b,t,k in st[:firstv] if k=='data' for n in shiparse.declnames(t)]
        if before:
            out.setdefault(c['name'],{'file':f.split('/include/')[1],'before':before,'template':c['template']})
json.dump(out,open(OUTF,'w'),indent=1)
for n,v in sorted(out.items()): print(n, v['file'], v['before'][:4], 'T' if v['template'] else '')
print(len(out))
