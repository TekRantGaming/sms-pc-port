# gdb -batch -ex "py names_file='...'; out_file='...'" -x export.py BINARY
import gdb, json
names=[l.strip() for l in open(names_file) if l.strip()]
types={}
import re as _re
def tname(t):
    try: s=str(t)
    except Exception: return '?'
    return _re.sub(r'^(class|struct|union) ','',s)
def reg(t):
    t=t.strip_typedefs()
    while t.code==gdb.TYPE_CODE_ARRAY: t=t.target().strip_typedefs()
    if t.code not in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION): return None
    n=tname(t)
    if n in types: return n
    types[n]=None
    fs=[]
    try: fields=t.fields()
    except Exception: fields=[]
    for f in fields:
        if not hasattr(f,'bitpos') or f.bitpos is None: continue  # static / virtual base
        ft=f.type
        st=ft.strip_typedefs()
        dims=[]
        e=st
        while e.code==gdb.TYPE_CODE_ARRAY:
            r=e.range(); dims.append(r[1]-r[0]+1); e=e.target().strip_typedefs()
        ent={'name':f.name,'off':f.bitpos//8,'bitpos':f.bitpos,'bitsize':f.bitsize,'size':ft.sizeof,
             'type':tname(ft),'base':bool(f.is_base_class),'dims':dims,'code':int(e.code),'esize':e.sizeof,
             'artificial':bool(getattr(f,'artificial',False))}
        sub=reg(e)
        if sub: ent['rec']=sub
        fs.append(ent)
    types[n]={'size':t.sizeof,'align':getattr(t,'alignof',0),'union':t.code==gdb.TYPE_CODE_UNION,'fields':fs}
    return n
found={}
for n in names:
    try:
        t=gdb.lookup_type(n)
    except Exception:
        continue
    r=reg(t)
    if r: found[n]=r
json.dump({'found':found,'types':types},open(out_file,'w'))
print('found',len(found),'types',len(types))
