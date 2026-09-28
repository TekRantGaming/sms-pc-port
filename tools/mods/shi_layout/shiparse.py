"""Find class definitions and their top-level statements in SHI headers."""
import re,glob,os
def strip_comments(s):
    # replace comments and string contents by spaces, keep length/newlines
    out=list(s); i=0; n=len(s)
    while i<n:
        c=s[i]
        if s.startswith('//',i):
            j=s.find('\n',i); j=n if j<0 else j
            for k in range(i,j): out[k]=' '
            i=j
        elif s.startswith('/*',i):
            j=s.find('*/',i+2); j=n if j<0 else j+2
            for k in range(i,j):
                if s[k]!='\n': out[k]=' '
            i=j
        elif c in '"\'':
            j=i+1
            while j<n and s[j]!=c:
                j+=2 if s[j]=='\\' else 1
            for k in range(i+1,min(j,n)): 
                if s[k]!='\n': out[k]=' '
            i=j+1
        elif c=='#':
            # preprocessor line: blank it (keep newline)
            j=i
            while True:
                e=s.find('\n',j); e=n if e<0 else e
                if e>0 and s[e-1]=='\\': j=e+1; continue
                break
            if s[max(0,s.rfind('\n',0,i)+1):i].strip()=='':
                for k in range(i,e): out[k]=' '
                i=e
            else: i+=1
        else: i+=1
    return ''.join(out)
def match_brace(s,i):
    d=0
    for j in range(i,len(s)):
        if s[j]=='{': d+=1
        elif s[j]=='}':
            d-=1
            if d==0: return j
    return -1
HEAD=re.compile(r'\b(class|struct|union)\s+(?:__attribute__\s*\(\([^)]*\)\)\s*)?(?:alignas\s*\([^)]*\)\s*)?([A-Za-z_]\w*)\s*(?:final\s*)?(:[^;{}]*)?\{')
def classes(path):
    raw=open(path,errors='surrogateescape').read()
    s=strip_comments(raw)
    # SMS_PACKED_CLASS(Name) / SMS_PACKED_STRUCT(Name): same length, as a plain head
    s=re.sub(r'SMS_PACKED_(CLASS|STRUCT)\s*\(\s*(\w+)\s*\)',lambda m:('%s %s'%(m.group(1).lower(),m.group(2))).ljust(len(m.group(0))),s)
    out=[]
    def scan(lo,hi,scope):
        i=lo
        while i<hi:
            m=re.compile(r'\bnamespace\s+(\w+)\s*\{|'+HEAD.pattern).search(s,i,hi)
            if not m: break
            if m.group(1):
                e=match_brace(s,m.end()-1); scan(m.end(),e,scope+[m.group(1)]); i=e+1; continue
            # template check
            pre=s[max(lo,m.start()-400):m.start()]
            is_tmpl=bool(re.search(r'template\s*<[^;{}]*>\s*$',pre))
            b=m.end()-1; e=match_brace(s,b)
            name='::'.join(scope+[m.group(3)])
            out.append(dict(name=name,file=path,body=(b+1,e),template=is_tmpl,kind=m.group(2)))
            scan(b+1,e,scope+[m.group(3)])
            i=e+1
    scan(0,len(s),[])
    return raw,s,out
KW=re.compile(r'^\s*(typedef|using|friend|static|enum|template|virtual|explicit|inline|operator|constexpr|public|private|protected|~|static_assert)\b')
def statements(s,body):
    """top-level statements of a class body: list of (start,end,text) ; end is index after ';' or '}'"""
    lo,hi=body; out=[]; i=lo; start=lo; dp=0
    while i<hi:
        c=s[i]
        if c in '([': dp+=1
        elif c in ')]': dp-=1
        elif c=='{' and dp==0:
            e=match_brace(s,i)
            # nested type def (class X {...};) or function body
            txt=s[start:i]
            if re.search(r'\b(class|struct|union|enum)\b[^;()]*$',txt):
                j=s.find(';',e)
                tail=s[e+1:j+1]
                if re.search(r'\w',tail): out.append((start,j+1,'__inline_type__ '+tail.strip(),None))
                else: out.append((start,j+1,s[start:j+1],'type'))
                i=j+1; start=i; continue
            out.append((start,e+1,s[start:e+1],'func')); i=e+1; start=i; continue
        elif c==';' and dp==0:
            out.append((start,i+1,s[start:i+1],None)); i+=1; start=i; continue
        elif c==':' and dp==0:
            t=s[start:i].strip()
            if t in ('public','private','protected') and s[i+1]!=':':
                start=i+1
        i+=1
    res=[]
    for a,b,t,kind in out:
        tt=re.sub(r'\s+',' ',t).strip()
        tt=re.sub(r'^(public|private|protected)\s*:\s*','',tt)
        if kind: res.append((a,b,tt,kind)); continue
        if not tt or tt==';': continue
        if KW.match(tt) or re.search(r'\bstatic\b',tt.split('(')[0]):
            res.append((a,b,tt,'other')); continue
        if '(' in tt and not re.search(r'\(\s*(\w+::)*\*\s*\w+\s*\)',tt):
            res.append((a,b,tt,'func')); continue
        res.append((a,b,tt,'data'))
    return res
def declnames(tt):
    t=re.sub(r'=.*?(,|;)',r'\1',tt)
    m=re.search(r'\(\s*(?:\w+::)*\*\s*(\w+)\s*\)',t)
    if m: return [m.group(1)]
    t=re.sub(r'\[[^\]]*\]','',t)
    t=re.sub(r':\s*\d+\s*(?=[,;])','',t)
    t=t.rstrip(';')
    parts=t.split(',')
    names=[]
    for p in parts:
        m=re.search(r'(\w+)\s*$',p.strip())
        if m: names.append(m.group(1))
    return names
