"""Retail (CodeWarrior) layouts of the port's classes: the port's 32-bit layout with each
vtable pointer moved to where CodeWarrior puts it, after the data members a class declares
before its first virtual function (vlate.json, from the decomp headers)."""
import json,copy,re
def build(P32,vlate):
    R=copy.deepcopy(P32)
    def base_name(n): return n.split('<')[0]
    moved=[]
    for T,t in R.items():
        info=vlate.get(T) or vlate.get(base_name(T))
        if not info: continue
        vp=[g for g in t['fields'] if g['name'] and re.match(r'_vptr[.$]',g['name']) and g['off']==0 and base_name(g['name'][6:])==base_name(T.split('::')[-1])]
        if not vp: continue
        vp=vp[0]
        before=set(info['before'])
        bf=[g for g in t['fields'] if g['name'] in before and not g['base']]
        if not bf: continue
        for g in bf: g['off']-=4; g['bitpos']=g['off']*8
        end=max(g['off']+g['size'] for g in bf)
        vp['off']=(end+3)//4*4; vp['bitpos']=vp['off']*8
        moved.append(T)
    return R,moved
