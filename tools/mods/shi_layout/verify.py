import json,os,sys
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import gen_layout as G
W=G.W
arch=int(sys.argv[1]); L=json.load(open(os.path.join(W,sys.argv[2])))['types']
used=[tuple(x) for x in json.load(open(os.path.join(W,'used_all.json')))]
bases=json.load(open(os.path.join(W,'shi_bases.json')))
D=G.P[arch]; bad=[]
for c,n in used:
    if G.pn(c) not in D or c not in L or c not in G.S[32]: bad.append('%s.%s: not comparable'%(c,n)); continue
    f32=[f for f in G.G32[c]['fields'] if f['name']==n]; fl=[f for f in L[c]['fields'] if f['name']==n]
    if not f32 or not fl: bad.append('%s.%s: missing'%(c,n)); continue
    o=G.tmap(G.pn(c),f32[0]['off'],D)
    if o is None: bad.append('%s.%s: no position (retail %#x)'%(c,n,f32[0]['off'])); continue
    if fl[0]['off']!=o: bad.append('%s.%s: at %#x, port %#x'%(c,n,fl[0]['off'],o))
def allb(c,acc):
    t=L.get(c)
    if not t: return
    acc.add(c)
    for g in t['fields']:
        if g['base'] and g.get('rec'): allb(g['rec'],acc)
B=set()
for b in bases: allb(b,B)
for c in sorted(B):
    if G.pn(c) not in D: bad.append('%s: base not in port'%c); continue
    if L[c]['size']!=D[G.pn(c)]['size'] or G.dataend(c,L)!=G.dataend(G.pn(c),D):
        bad.append('%s: size %d/dataend %d, port %d/%d'%(c,L[c]['size'],G.dataend(c,L),D[G.pn(c)]['size'],G.dataend(G.pn(c),D)))
print('\n'.join(bad)); print('arch',arch,'bad',len(bad),'of',len(used),'uses,',len(B),'bases')
