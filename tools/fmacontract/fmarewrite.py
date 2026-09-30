#!/usr/bin/env python3
"""MWCC's multiply-add contraction, applied to the decomp's source as explicit calls.

tools/fmacontract/fmarewrite.py [options]

MWCC 1.2.5 (-fp_contract on) fuses a multiplication into an addition or a
subtraction of which it is a direct operand, within one expression, and never
fuses a product that its common-subexpression elimination (CSE) or its
loop-invariant code motion (LICM) turns into a temporary. The port's compilers
cannot reproduce that choice (docs/64-BIT.md, item 16), so this pass makes it
in the source: it parses every game and JSystem unit of the port's build with
libclang (the command in compile_commands.json), models MWCC's rule in each
function body, and writes each fused site as a call to the port's exact
helpers (src/port_fmac.h):

  a * b + c, c + a * b, c += a * b     port_fmaf(a, b, c)    fmadds
  a * b - c                            port_fmsf(a, b, c)    fmsubs
  c - a * b, c -= a * b, -(a * b) + c  port_fnmsf(a, b, c)   fnmsubs
  (-(a * b + c) needs nothing: negating a fused result is exact, as fnmadds is)

with port_fmad/port_fmsd/port_fnmsd in double, and port_fmat/port_fmst/
port_fnmst in template code whose types depend on a parameter.

The model (every clause measured with the decomp's MWCC on test cases, see
docs/64-BIT.md):
- a product fuses when it is an operand of + or - (through parentheses, not
  through a cast or a call); in a * b + c * d the left one, else the right;
  -(a * b) + c is fnmsubs and c - -(a * b) fmadds; x / 2^k (k >= 1) is a
  product (MWCC multiplies by 2^-k); a compound assignment fuses too, to an
  integer as well (s16 += a * b computes in float);
- CSE: within a function, an arithmetic expression or a memory load (a
  member through a pointer or a reference, an array element, a global, a
  dereference) that is computed again while its operands are unchanged is
  computed once. Its first occurrence dominates the others (the same
  statement, later statements, the branches and loops that follow; not a
  sibling branch). A product that is computed once this way is never fused;
  and when one statement uses the same expression twice, the smallest
  expression containing both becomes a comma expression, so a product that
  contains both (m->x * m->x, (a - b) * (a - b)) is not fused either.
  Register variables (scalar locals and parameters whose address is not
  taken, and the fields of local structs whose address is not taken) are no
  expressions to share, so a * a fuses; int-to-float conversions and
  negations are not shared (they are leaves here). A store to memory or a
  call ends the sharing of memory loads; an assignment ends the sharing of
  what used the variable.
- LICM: a product whose operands do not change in the innermost loop around
  it is computed before the loop and not fused.
- `#pragma fp_contract off` switches it off to the end of the file (or to a
  later `on`, or to the `#pragma pop` of an enclosing `push`).
- -inline auto: a call to an inline function that neither stores nor calls
  ends no sharing, and a local struct stays in registers unless its address
  reaches a function that is not inline (with --inline off, every call ends
  the sharing of loads and takes the address of its arguments).
- An inline function's body is its own context (its sites are the same
  wherever it is inlined); a site in a template is written as port_fmat &c.;
  a site in a macro argument (ABS(a * b + c)) is rewritten where the
  argument is written.

A site whose operands call a function (other than a pure inline one) or
assign keeps their evaluation order: they are evaluated in source order in a
statement expression, since a call's arguments are evaluated in an order the
compiler chooses.

Needs libclang 18 and its Python bindings (pip install clang==18.1.8), and
the linux-32 and linux-64 builds configured (their compile_commands.json and
patched/ trees); the generated patch goes to decomp-patches/fma/ and is
regenerated when the decomp or decomp-patches change:

  tools/fmacontract/fmarewrite.py --patch decomp-patches/fma/0001-mwcc-fused-multiply-adds.patch

Output: --out DIR receives the rewritten copy of every changed file (paths
relative to the decomp root), --patch FILE a unified diff against the port's
patched tree (decomp + decomp-patches), and --sites FILE a TSV of every
site.
"""
import argparse, collections, concurrent.futures, ctypes, difflib, json, math, os, re, shlex, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    import clang.cindex as ci
except ImportError:
    sys.exit('fmarewrite: needs the libclang Python bindings (pip install clang==18.1.8)')

CK = ci.CursorKind
FUNC_KINDS = {CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR, CK.DESTRUCTOR,
              CK.CONVERSION_FUNCTION, CK.FUNCTION_TEMPLATE}
SCOPE_KINDS = {CK.NAMESPACE, CK.CLASS_DECL, CK.STRUCT_DECL, CK.UNION_DECL, CK.CLASS_TEMPLATE,
               CK.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION, CK.UNEXPOSED_DECL, CK.LINKAGE_SPEC}
TK = ci.TypeKind
INT_KINDS = {TK.BOOL, TK.CHAR_U, TK.UCHAR, TK.CHAR16, TK.CHAR32, TK.USHORT, TK.UINT, TK.ULONG,
             TK.ULONGLONG, TK.CHAR_S, TK.SCHAR, TK.WCHAR, TK.SHORT, TK.INT, TK.LONG, TK.LONGLONG,
             TK.ENUM}
# clang's BinaryOperatorKind / UnaryOperatorKind (libclang 17+)
BOP = {3: '*', 4: '/', 5: '%', 6: '+', 7: '-', 8: '<<', 9: '>>', 11: '<', 12: '>', 13: '<=', 14: '>=',
       15: '==', 16: '!=', 17: '&', 18: '^', 19: '|', 20: '&&', 21: '||', 22: '=', 23: '*=', 24: '/=',
       25: '%=', 26: '+=', 27: '-=', 28: '<<=', 29: '>>=', 30: '&=', 31: '^=', 32: '|=', 33: ','}
UOP = {1: 'x++', 2: 'x--', 3: '++x', 4: '--x', 5: '&', 6: '*', 7: '+', 8: '-', 9: '~', 10: '!'}
# MWCC's -inline setting the model follows: 'auto' as the game is built;
# 'off' to compare with MWCC -inline off (tools/fmacount --mwcc)
INLINE = 'auto'
ARITH = {'*', '/', '%', '+', '-', '<<', '>>', '&', '^', '|'}
ASSIGN = {'=', '*=', '/=', '%=', '+=', '-=', '<<=', '>>=', '&=', '^=', '|='}


def tkind(t):
    k = t.get_canonical().kind
    if k == TK.FLOAT:
        return 'f'
    if k == TK.DOUBLE or k == TK.LONGDOUBLE:
        return 'd'
    if k in INT_KINDS:
        return 'i'
    if k == TK.POINTER:
        return 'p'
    if k == TK.RECORD:
        return 'r'
    if k in (TK.LVALUEREFERENCE, TK.RVALUEREFERENCE):
        return 'ref'
    if k in (TK.CONSTANTARRAY, TK.INCOMPLETEARRAY, TK.VARIABLEARRAY, TK.DEPENDENTSIZEDARRAY):
        return 'a'
    if k in (TK.DEPENDENT, TK.UNEXPOSED, TK.ELABORATED, TK.TYPEDEF):
        return 'dep'
    return 'o'


class Node(object):
    __slots__ = ('id', 'k', 'op', 't', 's', 'e', 'ch', 'ref', 'name', 'call_inline', 'call_pure',
                 'ok', 'argref')

    def __repr__(self):
        return '<%s %s %s %d-%d>' % (self.k, self.op, self.t, self.s, self.e)


class Decl(object):
    __slots__ = ('id', 'kind', 't', 'local', 'static', 'volatile', 'name')


class Ctx(object):
    """One translation unit."""

    def __init__(self, lib):
        self.lib = lib
        self.nid = 0
        self.decls = {}
        self.fninfo = {}  # callee hash -> (inline, pure)
        self.fcache = {}


def setup_lib():
    lib = ci.conf.lib
    for fn in ('clang_getCursorBinaryOperatorKind', 'clang_getCursorUnaryOperatorKind'):
        f = getattr(lib, fn)
        f.argtypes = [ci.Cursor]
        f.restype = ctypes.c_int
    lib.clang_getFileLocation.argtypes = [ci.SourceLocation, ctypes.POINTER(ci.c_object_p),
                                          ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                                          ctypes.POINTER(ctypes.c_uint)]
    lib.clang_getFileLocation.restype = None
    lib.clang_Cursor_isFunctionInlined.argtypes = [ci.Cursor]
    lib.clang_Cursor_isFunctionInlined.restype = ctypes.c_uint
    return lib


def decl_info(ctx, c):
    h = c.hash
    d = ctx.decls.get(h)
    if d is not None:
        return d
    d = Decl()
    d.id = h
    d.kind = c.kind
    d.name = c.spelling
    t = c.type
    d.t = tkind(t)
    d.volatile = t.is_volatile_qualified()
    sp = c.semantic_parent
    d.local = c.kind == CK.PARM_DECL or (c.kind == CK.VAR_DECL and sp is not None and sp.kind in FUNC_KINDS)
    d.static = c.kind == CK.VAR_DECL and c.storage_class == ci.StorageClass.STATIC
    ctx.decls[h] = d
    return d


def callee_info(ctx, callee):
    """(inline, pure) of a called function: MWCC inlines inline functions;
    a pure one (no stores, no calls) ends no sharing."""
    if callee is None or callee.kind not in FUNC_KINDS or INLINE == 'off':
        return False, False
    h = callee.hash
    if h in ctx.fninfo:
        return ctx.fninfo[h]
    ctx.fninfo[h] = (False, False)
    d = callee.get_definition()
    inline = False
    pure = False
    if d is not None:
        inline = bool(ctx.lib.clang_Cursor_isFunctionInlined(d)) or (
            d.lexical_parent is not None and d.lexical_parent.kind in (CK.CLASS_DECL, CK.STRUCT_DECL, CK.CLASS_TEMPLATE))
        if inline:
            pure = True
            for x in d.walk_preorder():
                if x.kind == CK.CALL_EXPR:
                    pure = False
                    break
                if x.kind in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR):
                    if BOP.get(ctx.lib.clang_getCursorBinaryOperatorKind(x)) in ASSIGN:
                        ch = list(x.get_children())
                        tgt = strip_c(ch[0]) if ch else None
                        if not (tgt is not None and tgt.kind == CK.DECL_REF_EXPR and tgt.referenced is not None
                                and tgt.referenced.kind == CK.VAR_DECL):
                            pure = False
                            break
                elif x.kind == CK.UNARY_OPERATOR and ctx.lib.clang_getCursorUnaryOperatorKind(x) in (1, 2, 3, 4):
                    pure = False
                    break
    ctx.fninfo[h] = (inline, pure)
    return inline, pure


def file_loc(ctx, loc):
    lib = ctx.lib
    f = ci.c_object_p()
    line, col, off = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
    lib.clang_getFileLocation(loc, ctypes.byref(f), ctypes.byref(line), ctypes.byref(col), ctypes.byref(off))
    key = ctypes.cast(f, ctypes.c_void_p).value
    if not key:
        return None, off.value
    name = ctx.fcache.get(key)  # per unit: a CXFile lives as long as its unit
    if name is None:
        name = ci.File(f).name
        ctx.fcache[key] = name
    return name, off.value


def strip_c(c):
    while c.kind in (CK.PAREN_EXPR, CK.UNEXPOSED_EXPR):
        ch = list(c.get_children())
        if len(ch) != 1:
            break
        c = ch[0]
    return c


def build(ctx, c, fname):
    """Converts a cursor subtree to Nodes. ok is False where the extent is not
    plain text of this file (macro expansions)."""
    n = Node()
    ctx.nid += 1
    n.id = ctx.nid
    n.k = c.kind
    n.op = None
    n.ref = None
    n.name = None
    n.call_inline = n.call_pure = False
    n.argref = None
    # where the text is written: a macro argument's own tokens (ABS(a * b + c)),
    # a macro body's the expansion point (then s == e: not rewritable)
    ext = c.extent
    fs, s = file_loc(ctx, ext.start)
    fe, e = file_loc(ctx, ext.end)
    n.s, n.e = s, e
    n.ok = fs == fname and fe == fname and s < e
    k = c.kind
    if k in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR):
        n.op = BOP.get(ctx.lib.clang_getCursorBinaryOperatorKind(c), '?')
    elif k == CK.UNARY_OPERATOR:
        n.op = UOP.get(ctx.lib.clang_getCursorUnaryOperatorKind(c), '?')
    n.t = tkind(c.type) if k != CK.COMPOUND_STMT else None
    if k in (CK.DECL_REF_EXPR, CK.MEMBER_REF_EXPR, CK.VAR_DECL):
        r = c if k == CK.VAR_DECL else c.referenced
        n.name = c.spelling
        if r is not None and r.kind in (CK.VAR_DECL, CK.PARM_DECL):
            n.ref = decl_info(ctx, r)
        elif r is not None and r.kind == CK.FIELD_DECL:
            n.name = 'F%d' % r.hash
        elif k == CK.MEMBER_REF_EXPR:
            n.name = None  # a dependent member: named from the text (Func.member)
        elif r is not None and r.kind == CK.ENUM_CONSTANT_DECL:
            n.k = CK.INTEGER_LITERAL
    elif k in (CK.INTEGER_LITERAL, CK.FLOATING_LITERAL):
        n.name = None
    elif k == CK.CALL_EXPR:
        callee = c.referenced
        n.call_inline, n.call_pure = callee_info(ctx, callee)
        if callee is not None and callee.kind in FUNC_KINDS:
            try:
                n.argref = [tkind(a.type) == 'ref' for a in callee.get_arguments()]
            except Exception:
                n.argref = None
    n.ch = [build(ctx, x, fname) for x in c.get_children()
            if x.kind not in (CK.TYPE_REF, CK.TEMPLATE_REF, CK.NAMESPACE_REF, CK.OVERLOADED_DECL_REF)]
    if k in (CK.INTEGER_LITERAL, CK.FLOATING_LITERAL) and n.ok:
        n.name = None  # filled from the source text by the caller
    return n


def strip(n):
    """Parentheses and conversions that keep the type (lvalue-to-rvalue)."""
    while True:
        if n.k == CK.PAREN_EXPR and len(n.ch) == 1:
            n = n.ch[0]
        elif n.k == CK.UNEXPOSED_EXPR and len(n.ch) == 1 and n.ch[0].t == n.t:
            n = n.ch[0]
        elif n.k == CK.UNARY_OPERATOR and n.op == '+' and len(n.ch) == 1:
            n = n.ch[0]
        else:
            return n


def walk(n):
    st = [n]
    while st:
        x = st.pop()
        yield x
        st.extend(reversed(x.ch))


def pow2_divisor(n, src):
    """The literal text of x / 2^k (k >= 1): MWCC multiplies by 2^-k, a
    product that fuses like any other (a / 3.0f and a / 0.5f do not)."""
    if not (n.k == CK.BINARY_OPERATOR and n.op == '/' and len(n.ch) == 2):
        return None
    d = strip(n.ch[1])
    if d.k != CK.FLOATING_LITERAL or not d.ok:
        return None
    txt = src[d.s:d.e]
    try:
        v = float(txt.rstrip(b'fFlL').decode())
    except ValueError:
        return None
    if v >= 2.0 and math.frexp(v)[0] == 0.5:
        return txt
    return None


def is_mul(n, src):
    return n.k == CK.BINARY_OPERATOR and len(n.ch) == 2 and (n.op == '*' or pow2_divisor(n, src) is not None)


class Func(object):
    """The CSE/LICM model of one function body."""

    def __init__(self, body, params, src):
        self.body = body
        self.src = src
        self.addr = set()      # decl ids whose address is taken
        self.keys = {}
        self.shared = set()    # node ids computed once by CSE (defs and reuses)
        self.wrapped = set()   # node ids that CSE turns into comma expressions
        self.hoisted = set()   # node ids hoisted out of a loop
        self.uses = collections.defaultdict(list)  # def node id -> reuse nodes
        self.nodes = {}
        for x in walk(body):
            self.nodes[x.id] = x
        self.find_addr_taken()

    # ---- register variables ----
    def find_addr_taken(self):
        for x in walk(self.body):
            if x.k == CK.UNARY_OPERATOR and x.op == '&' and x.ch:
                b = strip(x.ch[0])
                while b.k in (CK.MEMBER_REF_EXPR, CK.ARRAY_SUBSCRIPT_EXPR) and b.ch:
                    b = strip(b.ch[0])
                if b.k == CK.DECL_REF_EXPR and b.ref is not None:
                    self.addr.add(b.ref.id)
            elif x.k == CK.CALL_EXPR and not x.call_inline:
                args = [a for a in x.ch]
                # member call on a local struct: this = &v
                if args and args[0].k == CK.MEMBER_REF_EXPR and args[0].ch:
                    b = strip(args[0].ch[0])
                    if b.k == CK.DECL_REF_EXPR and b.ref is not None and b.ref.t == 'r':
                        self.addr.add(b.ref.id)
                if x.argref:
                    tail = args[len(args) - len(x.argref):] if len(args) >= len(x.argref) else []
                    for a, isref in zip(tail, x.argref):
                        if isref:
                            b = strip(a)
                            if b.k == CK.DECL_REF_EXPR and b.ref is not None:
                                self.addr.add(b.ref.id)
            elif x.k == CK.VAR_DECL and x.ref is not None and x.ref.t == 'ref' and x.ch:
                b = strip(x.ch[-1])
                if b.k == CK.DECL_REF_EXPR and b.ref is not None:
                    self.addr.add(b.ref.id)

    def vclass(self, d):
        """'reg' (a register), 'sreg' (a local struct kept in registers) or 'mem'."""
        if d is None or not d.local or d.static or d.volatile or d.id in self.addr:
            return 'mem'
        if d.t in ('f', 'd', 'i', 'p', 'dep', 'o'):
            return 'reg'
        if d.t == 'r' and d.kind == CK.VAR_DECL:
            return 'sreg'
        return 'mem'

    # ---- expression keys ----
    def key(self, n):
        """(key, deps, mem, candidate); key None if the expression is unique."""
        r = self.keys.get(n.id)
        if r is not None:
            return r
        r = self._key(n)
        self.keys[n.id] = r
        return r

    def _key(self, n):
        k = n.k
        if k == CK.PAREN_EXPR or (k == CK.UNEXPOSED_EXPR and len(n.ch) == 1 and n.ch[0].t == n.t) or \
                (k == CK.UNARY_OPERATOR and n.op == '+'):
            return self.key(n.ch[0]) if n.ch else (None, frozenset(), True, False)
        if k in (CK.UNEXPOSED_EXPR, CK.CSTYLE_CAST_EXPR, CK.CXX_FUNCTIONAL_CAST_EXPR, CK.CXX_STATIC_CAST_EXPR) \
                and n.ch:
            kk, dp, mm, _ = self.key(n.ch[-1])
            return (('cv', n.t, kk) if kk is not None else None), dp, mm, False
        if k in (CK.INTEGER_LITERAL, CK.FLOATING_LITERAL, CK.CXX_BOOL_LITERAL_EXPR):
            return ('lit', self.src[n.s:n.e] if n.ok else n.id), frozenset(), False, False
        if k == CK.CXX_THIS_EXPR:
            return ('this',), frozenset(), False, False
        if k == CK.DECL_REF_EXPR:
            d = n.ref
            if d is None:
                return ('fn', n.name), frozenset(), False, False
            vc = self.vclass(d)
            if vc == 'reg':
                return ('r', d.id), frozenset([d.id]), False, False
            if vc == 'sreg':
                return ('r', d.id), frozenset([d.id]), False, False
            return ('g', d.id), frozenset([d.id]), True, True
        if k == CK.MEMBER_REF_EXPR:
            if n.name is None:
                m = re.search(rb'([A-Za-z_]\w*)\s*$', self.src[n.s:n.e]) if n.ok else None
                n.name = 'D' + (m.group(1).decode() if m else str(n.id))
            if not n.ch:
                return ('m', ('this',), n.name), frozenset(), True, True
            b = strip(n.ch[0])
            if b.k == CK.DECL_REF_EXPR and b.ref is not None and b.t == 'r' and self.vclass(b.ref) == 'sreg':
                return ('r', b.ref.id, n.name), frozenset([b.ref.id]), False, False
            bk, dp, mm, _ = self.key(b)
            if bk is None:
                return None, dp, True, False
            return ('m', bk, n.name), dp, True, True
        if k == CK.ARRAY_SUBSCRIPT_EXPR and len(n.ch) == 2:
            a, dpa, _, _ = self.key(n.ch[0])
            b, dpb, _, _ = self.key(n.ch[1])
            if a is None or b is None:
                return None, dpa | dpb, True, False
            return ('ix', a, b), dpa | dpb, True, True
        if k == CK.UNARY_OPERATOR and n.ch:
            a, dp, mm, _ = self.key(n.ch[0])
            if n.op == '*':
                return (('dr', a) if a is not None else None), dp, True, a is not None
            if n.op in ('-', '~', '!'):
                return ((n.op, a) if a is not None else None), dp, mm, False
            return None, dp, mm, False
        if k == CK.BINARY_OPERATOR and n.op in ARITH and len(n.ch) == 2:
            a, dpa, ma, _ = self.key(n.ch[0])
            b, dpb, mb, _ = self.key(n.ch[1])
            if a is None or b is None:
                return None, dpa | dpb, ma or mb, False
            return (n.op, a, b), dpa | dpb, ma or mb, True
        dp = frozenset()
        mm = False
        for c in n.ch:
            _, d2, m2, _ = self.key(c)
            dp |= d2
            mm = mm or m2
        return None, dp, mm, False

    # ---- evaluation ----
    def run(self):
        self.env = {}
        self.loops = []
        self.fe = 0
        self.fe_defs = []
        self.stmt(self.body)

    def full(self, n):
        """One full expression."""
        self.fe += 1
        self.fe_defs = []
        self.ev(n, (n.id,))
        for d, dpath in self.fe_defs:
            same = [p for (u, p, fe) in self.uses[d.id] if fe == self.fe]
            if same:
                lca = dpath
                for p in same:
                    i = 0
                    while i < len(lca) and i < len(p) and lca[i] == p[i]:
                        i += 1
                    lca = lca[:i]
                if lca:
                    self.wrapped.add(lca[-1])

    def kill_var(self, vid):
        for k in [k for k, (d, dp, mm, _) in self.env.items() if vid in dp]:
            del self.env[k]

    def kill_mem(self):
        for k in [k for k, (d, dp, mm, _) in self.env.items() if mm]:
            del self.env[k]

    def kill_target(self, lhs):
        b = strip(lhs)
        if b.k == CK.DECL_REF_EXPR and b.ref is not None and self.vclass(b.ref) in ('reg', 'sreg'):
            self.kill_var(b.ref.id)
            return
        if b.k == CK.MEMBER_REF_EXPR and b.ch:
            bb = strip(b.ch[0])
            if bb.k == CK.DECL_REF_EXPR and bb.ref is not None and self.vclass(bb.ref) == 'sreg':
                self.kill_var(bb.ref.id)
                return
        self.kill_mem()

    def ev_lvalue(self, n, path):
        b = strip(n)
        if b.k == CK.DECL_REF_EXPR:
            return
        if b.k == CK.MEMBER_REF_EXPR:
            if b.ch:
                base = b.ch[0]
                if base.t == 'p':
                    self.ev(base, path + (base.id,))
                else:
                    self.ev_lvalue(base, path + (base.id,))
            return
        if b.k == CK.ARRAY_SUBSCRIPT_EXPR and len(b.ch) == 2:
            base = b.ch[0]
            if base.t == 'a':
                self.ev_lvalue(base, path + (base.id,))
            else:
                self.ev(base, path + (base.id,))
            self.ev(b.ch[1], path + (b.ch[1].id,))
            return
        if b.k == CK.UNARY_OPERATOR and b.op == '*' and b.ch:
            self.ev(b.ch[0], path + (b.ch[0].id,))
            return
        self.ev(b, path)

    def branch(self, fn):
        """Runs fn on a copy of the environment; afterwards, only what was
        available before and survived the branch stays available."""
        save = self.env
        self.env = dict(save)
        fn()
        after = self.env
        self.env = {k: v for k, v in save.items() if k in after}

    def ev(self, n, path):
        k = n.k
        if k in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR) and n.op in ASSIGN and len(n.ch) == 2:
            lhs, rhs = n.ch
            if n.op == '=':
                self.ev(rhs, path + (rhs.id,))
                self.ev_lvalue(lhs, path + (lhs.id,))
            else:
                self.ev(lhs, path + (lhs.id,))
                self.ev(rhs, path + (rhs.id,))
            self.kill_target(lhs)
            return
        if k == CK.UNARY_OPERATOR and n.op in ('x++', 'x--', '++x', '--x') and n.ch:
            self.ev(n.ch[0], path + (n.ch[0].id,))
            self.kill_target(n.ch[0])
            return
        if k == CK.BINARY_OPERATOR and n.op in ('&&', '||') and len(n.ch) == 2:
            self.ev(n.ch[0], path + (n.ch[0].id,))
            r = n.ch[1]
            self.branch(lambda: self.ev(r, path + (r.id,)))
            return
        if k == CK.CONDITIONAL_OPERATOR and len(n.ch) == 3:
            self.ev(n.ch[0], path + (n.ch[0].id,))
            a, b = n.ch[1], n.ch[2]
            save = self.env
            self.env = dict(save)
            self.ev(a, path + (a.id,))
            ea = self.env
            self.env = dict(save)
            self.ev(b, path + (b.id,))
            eb = self.env
            self.env = {kk: v for kk, v in save.items() if kk in ea and kk in eb}
            return
        if k == CK.CALL_EXPR:
            for c in n.ch:
                self.ev(c, path + (c.id,))
            if not n.call_pure:
                self.kill_mem()
            return
        if k == CK.UNARY_OPERATOR and n.op == '&' and n.ch:
            self.ev_lvalue(n.ch[0], path + (n.ch[0].id,))
            return
        if len(n.ch) == 1 and strip(n) is not n:
            # parentheses and lvalue-to-rvalue: the expression is the child's
            c = n.ch[0]
            self.ev(c, path + (c.id,))
            return
        key, deps, mem, cand = self.key(n)
        if cand and key is not None:
            hit = self.env.get(key)
            if hit is not None:
                d = hit[0]
                self.uses[d.id].append((n, path, self.fe))
                self.shared.add(n.id)
                self.shared.add(d.id)
                return
        for c in n.ch:
            self.ev(c, path + (c.id,))
        if cand and key is not None:
            self.env[key] = (n, deps, mem, path)
            self.fe_defs.append((n, path))
        if k == CK.BINARY_OPERATOR and is_mul(n, self.src) and self.loops and n.t in ('f', 'd', 'dep'):
            mod, memkill = self.loops[-1]
            if key is not None and not (deps & mod) and not (mem and memkill):
                self.hoisted.add(n.id)

    # ---- statements ----
    def loop_effects(self, parts):
        mod = set()
        memkill = False
        for p in parts:
            for x in walk(p):
                tgt = None
                if x.k in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR) and x.op in ASSIGN and x.ch:
                    tgt = x.ch[0]
                elif x.k == CK.UNARY_OPERATOR and x.op in ('x++', 'x--', '++x', '--x') and x.ch:
                    tgt = x.ch[0]
                elif x.k == CK.VAR_DECL and x.ref is not None:
                    mod.add(x.ref.id)
                elif x.k == CK.CALL_EXPR and not x.call_pure:
                    memkill = True
                if tgt is not None:
                    b = strip(tgt)
                    if b.k == CK.DECL_REF_EXPR and b.ref is not None and self.vclass(b.ref) == 'reg':
                        mod.add(b.ref.id)
                        continue
                    if b.k == CK.MEMBER_REF_EXPR and b.ch:
                        bb = strip(b.ch[0])
                        if bb.k == CK.DECL_REF_EXPR and bb.ref is not None and self.vclass(bb.ref) == 'sreg':
                            mod.add(bb.ref.id)
                            continue
                    memkill = True
        return mod, memkill

    def loop(self, pre, parts_in_order):
        """pre: run before the loop (for-init); parts_in_order: (node, is_expr)."""
        mod, memkill = self.loop_effects([p for p, _ in parts_in_order])
        for vid in mod:
            self.kill_var(vid)
        if memkill:
            self.kill_mem()
        self.loops.append((frozenset(mod), memkill))

        def body():
            for p, is_expr in parts_in_order:
                if is_expr:
                    self.full(p)
                else:
                    self.stmt(p)
        self.branch(body)
        self.loops.pop()

    def stmt(self, n):
        k = n.k
        if k == CK.COMPOUND_STMT:
            for c in n.ch:
                self.stmt(c)
        elif k == CK.DECL_STMT:
            for v in n.ch:
                if v.k == CK.VAR_DECL:
                    inits = [c for c in v.ch if c.k not in (CK.TYPE_REF,)]
                    if inits:
                        self.full(inits[-1])
                    if v.ref is not None:
                        self.kill_var(v.ref.id)
        elif k == CK.IF_STMT:
            ch = n.ch
            if not ch:
                return
            self.cond(ch[0])
            rest = ch[1:]
            if len(rest) == 1:
                self.branch(lambda: self.stmt(rest[0]))
            elif len(rest) >= 2:
                save = self.env
                self.env = dict(save)
                self.stmt(rest[0])
                ea = self.env
                self.env = dict(save)
                self.stmt(rest[1])
                eb = self.env
                self.env = {kk: v for kk, v in save.items() if kk in ea and kk in eb}
        elif k == CK.WHILE_STMT and len(n.ch) >= 2:
            self.loop(None, [(n.ch[0], True), (n.ch[-1], False)])
        elif k == CK.DO_STMT and len(n.ch) >= 2:
            self.loop(None, [(n.ch[0], False), (n.ch[-1], True)])
        elif k == CK.FOR_STMT:
            init, cond, inc, body = self.for_parts(n)
            if init is not None:
                if init.k == CK.DECL_STMT:
                    self.stmt(init)
                else:
                    self.full(init)
            parts = []
            if cond is not None:
                parts.append((cond, cond.k != CK.DECL_STMT))
            if body is not None:
                parts.append((body, False))
            if inc is not None:
                parts.append((inc, True))
            self.loop(None, parts)
        elif k == CK.SWITCH_STMT and n.ch:
            self.cond(n.ch[0])
            body = n.ch[-1]
            entry = dict(self.env)
            _, memkill = self.loop_effects([body])
            mod, _ = self.loop_effects([body])

            def swbody():
                self.switch_stmt(body, entry, mod, memkill)
            self.branch(swbody)
        elif k in (CK.CASE_STMT, CK.DEFAULT_STMT):
            if n.ch:
                self.stmt(n.ch[-1])
        elif k == CK.LABEL_STMT:
            self.env = {}
            for c in n.ch:
                self.stmt(c)
        elif k == CK.RETURN_STMT:
            for c in n.ch:
                self.full(c)
        elif k in (CK.BREAK_STMT, CK.CONTINUE_STMT, CK.GOTO_STMT, CK.NULL_STMT, CK.ASM_STMT,
                   CK.MS_ASM_STMT, CK.INDIRECT_GOTO_STMT):
            pass
        elif k.is_expression() or k == CK.UNEXPOSED_EXPR:
            self.full(n)
        else:
            for c in n.ch:
                if c.k.is_statement():
                    self.stmt(c)

    def switch_stmt(self, body, entry, mod, memkill):
        def reset():
            env = dict(entry)
            self.env = env
            for vid in mod:
                self.kill_var(vid)
            if memkill:
                self.kill_mem()
        items = body.ch if body.k == CK.COMPOUND_STMT else [body]
        for c in items:
            x = c
            while x.k in (CK.CASE_STMT, CK.DEFAULT_STMT):
                reset()
                x = x.ch[-1] if x.ch else None
                if x is None:
                    break
            if x is not None:
                self.stmt(x)

    def cond(self, c):
        if c.k == CK.VAR_DECL or c.k == CK.DECL_STMT:
            self.stmt(c if c.k == CK.DECL_STMT else _wrap_decl(c))
        else:
            self.full(c)

    def for_parts(self, n):
        """libclang omits a for statement's empty parts: sort the children by
        the semicolons of the header."""
        src = self.src
        body = n.ch[-1] if n.ch else None
        heads = n.ch[:-1]
        # the header's two semicolons, outside parentheses
        i = src.index(b'(', n.s)
        depth = 0
        semis = []
        j = i
        while j < n.e:
            ch = src[j:j + 1]
            if ch == b'(':
                depth += 1
            elif ch == b')':
                depth -= 1
                if depth == 0:
                    break
            elif ch == b';' and depth == 1:
                semis.append(j)
            j += 1
        init = cond = inc = None
        if len(semis) != 2:
            return (heads[0] if heads else None), None, None, body
        for h in heads:
            if h.e <= semis[0] + 1:
                init = h
            elif h.s > semis[1]:
                inc = h
            else:
                cond = h
        return init, cond, inc, body


def _wrap_decl(v):
    d = Node()
    d.id = -v.id
    d.k = CK.DECL_STMT
    d.ch = [v]
    d.op = None
    d.t = None
    d.s, d.e = v.s, v.e
    d.ok = v.ok
    return d


# ---- sites ----
FORMS = {'fma': 'port_fma', 'fms': 'port_fms', 'fnms': 'port_fnms'}
SUFFIX = {'f': 'f', 'd': 'd', 'dep': 't'}


def has_side_effects(n):
    for x in walk(n):
        if x.k == CK.CALL_EXPR:
            return True
        if x.k in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR) and x.op in ASSIGN:
            return True
        if x.k == CK.UNARY_OPERATOR and x.op in ('x++', 'x--', '++x', '--x'):
            return True
    return False


def find_sites(F, stats):
    sites = {}

    def isprod(x, t):
        return x.k == CK.BINARY_OPERATOR and x.op == '*' and len(x.ch) == 2 and x.t == t

    def fusable(x):
        why = None
        if x.id in F.shared:
            why = 'cse'
        elif x.id in F.wrapped:
            why = 'comma'
        elif x.id in F.hoisted:
            why = 'licm'
        return why

    def negprod(x, t):
        if x.k == CK.UNARY_OPERATOR and x.op == '-' and x.ch:
            y = strip(x.ch[0])
            if is_mul(y, F.src):
                return y
        return None

    for n in walk(F.body):
        # a compound assignment to an integer (s16 += a * b) is fused in float
        if n.t not in ('f', 'd', 'dep') and not (n.k == CK.COMPOUND_ASSIGNMENT_OPERATOR and n.t == 'i'):
            continue
        cands = []
        if n.k == CK.BINARY_OPERATOR and n.op in ('+', '-') and len(n.ch) == 2:
            L, R = strip(n.ch[0]), strip(n.ch[1])
            if n.op == '+':
                cands = [(L, 'fma', R, False), (negprod(L, n.t), 'fnms', R, False),
                         (R, 'fma', L, False), (negprod(R, n.t), 'fnms', L, False)]
            else:
                cands = [(L, 'fms', R, False), (R, 'fnms', L, False), (negprod(R, n.t), 'fma', L, False)]
        elif n.k == CK.COMPOUND_ASSIGNMENT_OPERATOR and n.op in ('+=', '-=') and len(n.ch) == 2:
            R = strip(n.ch[1])
            cands = [(R, 'fma' if n.op == '+=' else 'fnms', n.ch[0], True)]
        for P, form, C, comp in cands:
            if P is None or not is_mul(P, F.src):
                continue
            # the product has the sum's type (a float product in a double sum
            # is converted first, and not fused); in a template, a dependent
            # sum may take a float product (port_fmat checks the types)
            if comp:
                if P.t not in ('f', 'd', 'dep'):
                    continue
            elif not (P.t == n.t or (n.t == 'dep' and P.t in ('f', 'd'))):
                continue
            if comp and P.t not in ('f', 'd', 'dep'):
                continue
            why = fusable(P)
            if why:
                stats['unfused_' + why] += 1
                continue
            if comp and has_side_effects(C):
                stats['skip_sidefx_lhs'] += 1
                break
            if not all(x.ok for x in (n, P, P.ch[0], P.ch[1], C)):
                stats['skip_macro'] += 1
                break
            sites[n.id] = (n, form, P, C, comp, P.t if comp else n.t)
            break
    # a reuse of a shared expression must compute what its definition does
    for did, uses in F.uses.items():
        d = F.nodes.get(did)
        if d is None:
            continue
        inner = [x for x in walk(d) if x.id in sites]
        if not inner:
            continue
        for (u, _, _) in uses:
            mp = {}
            if not parallel(d, u, mp):
                stats['skip_mirror'] += 1
                continue
            for x in inner:
                n, form, P, C, comp, t = sites[x.id]
                try:
                    sites.setdefault(mp[n.id].id, (mp[n.id], form, mp[P.id], mp[C.id], comp, t) + ('mirror',))
                    stats['mirrored'] += 1
                except KeyError:
                    stats['skip_mirror'] += 1
    return sites


def parallel(a, b, mp):
    if a.k != b.k or len(a.ch) != len(b.ch) or a.op != b.op:
        return False
    mp[a.id] = b
    for x, y in zip(a.ch, b.ch):
        if not parallel(x, y, mp):
            return False
    return True


def impure(n):
    """A call that is not a pure inline function, an assignment or an increment."""
    for x in walk(n):
        # (a call a template leaves unresolved, such as this->at(i, j), is
        # taken for an accessor)
        if x.k == CK.CALL_EXPR and not x.call_pure and x.argref is not None:
            return True
        if x.k in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR) and x.op in ASSIGN:
            return True
        if x.k == CK.UNARY_OPERATOR and x.op in ('x++', 'x--', '++x', '--x'):
            return True
    return False


def render_fn(F, sites, ordered=None):
    src = F.src
    hs = {}
    if ordered is None:
        ordered = []

    def has(x):
        r = hs.get(x.id)
        if r is None:
            r = x.id in sites or any(has(c) for c in x.ch)
            hs[x.id] = r
        return r

    def render(x):
        if x.id in sites:
            return rsite(sites[x.id])
        if not has(x):
            return src[x.s:x.e]
        out = []
        pos = x.s
        for c in sorted(x.ch, key=lambda c: c.s):
            if has(c):
                if not (x.s <= c.s and c.e <= x.e and c.s >= pos):
                    raise ValueError('overlap')
                out.append(src[pos:c.s])
                out.append(render(c))
                pos = c.e
        out.append(src[pos:x.e])
        return b''.join(out)

    def rsite(s):
        n, form, P, C, comp, t = s[:6]
        name = (FORMS[form] + SUFFIX[t]).encode()
        ops = [P.ch[0], P.ch[1], C]
        a, b, c = [render(x) for x in ops]
        div = pow2_divisor(P, src)
        if div is not None:
            b = b'(1 / ' + div + b')'  # exact: a power of two
        if any(impure(x) for x in ops):
            # a call's side effects keep their order: the operands are
            # evaluated in the order the source writes them, as before (the
            # compiler may evaluate a call's arguments in any order, and the
            # 32-bit and 64-bit builds do not agree)
            order = sorted(range(3), key=lambda i: ops[i].s)
            txt = [a, b, c]
            tag = b'port_fmac_%d_%d_' % (n.s, n.e)  # the same in every unit
            decl = b''.join(b'__typeof__(' + txt[i] + b') ' + tag + b'%d = ' % i + txt[i] + b'; ' for i in order)
            call = b'__extension__({ ' + decl + name + b'(' + tag + b'0, ' + tag + b'1, ' + tag + b'2); })'
            ordered.append(n.id)
        else:
            call = name + b'(' + a + b', ' + b + b', ' + c + b')'
        if comp:
            return c + b' = ' + call
        return call

    edits = []

    def top(x):
        if x.id in sites:
            edits.append((x.s, x.e, render(x)))
            return
        for c in x.ch:
            if has(c):
                top(c)
    top(F.body)
    return edits


# ---- driver ----
def unit_args(entry):
    args = shlex.split(entry['command'])
    src = entry['file']
    out = []
    skip = False
    for a in args[1:]:
        if skip:
            skip = False
            continue
        if a == '-o':
            skip = True
            continue
        if a in ('-c', src, '-g') or a.startswith(('-fexec-charset', '-finput-charset', '-ffp-contract')):
            continue
        if a in ('-fno-unreachable-traps', '-fpermissive', '-fno-gnu-keywords', '-fno-threadsafe-statics'):
            continue
        out.append(a)
    if src.endswith('.c') and not any(a == 'c++' and b == '-x' for b, a in zip(out, out[1:])):
        out = ['-x', 'c'] + out
    return out + ['-fms-extensions', '-Wno-everything', '-ferror-limit=0']


def pragma_off_ranges(src):
    """Byte ranges where `#pragma fp_contract off` holds (push/pop kept)."""
    ranges = []
    state = True
    stack = []
    start = None
    for m in re.finditer(rb'^[ \t]*#[ \t]*pragma[ \t]+(push|pop|fp_contract[ \t]+(\w+))', src, re.M):
        if m.group(1) == b'push':
            stack.append(state)
            continue
        if m.group(1) == b'pop':
            new = stack.pop() if stack else True
        else:
            new = m.group(2) != b'off' if m.group(2) != b'reset' else True
        if state and not new:
            start = m.start()
        elif not state and new:
            ranges.append((start, m.start()))
        state = new
    if not state:
        ranges.append((start, len(src)))
    return ranges


class Rewriter(object):
    def __init__(self, roots):
        self.roots = roots  # absolute tree roots (patched first, then decomp)
        self.done = set()
        self.srcs = {}
        self.edits = collections.defaultdict(dict)  # relpath -> {(s, e): text}
        self.sitelog = []
        self.stats = collections.Counter()
        self.conflicts = []

    def rel(self, path):
        for r in self.roots:
            if path.startswith(r + '/'):
                return path[len(r) + 1:]
        return None

    def wanted(self, rel):
        return rel is not None and (rel.startswith(('src/', 'include/')) or rel.startswith('libs/JSystem/'))

    def unit(self, entry, lib):
        ctx = Ctx(lib)
        idx = ci.Index.create()
        tu = idx.parse(entry['file'], args=unit_args(entry),
                       options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD * 0)
        errs = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        if errs:
            self.stats['units_with_errors'] += 1
            self.sitelog.append(('#error', entry['file'], str(errs[0])))
        self.scan(ctx, tu.cursor)
        self.stats['units'] += 1

    def scan(self, ctx, c):
        for x in c.get_children():
            loc = x.location
            f = loc.file
            if f is None:
                continue
            fname = f.name
            rel = self.rel(fname)
            if not self.wanted(rel):
                continue
            if x.kind in SCOPE_KINDS:
                self.scan(ctx, x)
            elif x.kind in FUNC_KINDS and x.is_definition():
                self.function(ctx, x)

    def source(self, fname):
        s = self.srcs.get(fname)
        if s is None:
            s = open(fname, 'rb').read()
            self.srcs[fname] = s
        return s

    def function(self, ctx, c):
        body = None
        inits = []
        for ch in c.get_children():
            if ch.kind == CK.COMPOUND_STMT:
                body = ch
            elif ch.kind.is_expression():
                inits.append(ch)
        if body is None:
            return
        # the body's file (a function whose head a macro writes, such as
        # DEFINE_NERVE, has its location in the macro's header)
        f = body.extent.start.file
        if f is None:
            return
        fname = f.name
        rel = self.rel(fname)
        if not self.wanted(rel):
            return
        key = (rel, body.extent.start.offset)
        if key in self.done:
            return
        self.done.add(key)
        src = self.source(fname)
        off = pragma_off_ranges(src)
        if any(a <= body.extent.start.offset < b for a, b in off):
            self.stats['fn_pragma_off'] += 1
            return
        root = build(ctx, body, fname)
        if inits:
            # constructor initialisers: expressions evaluated before the body
            wrapper = Node()
            wrapper.id = -1
            wrapper.k = CK.COMPOUND_STMT
            wrapper.op = None
            wrapper.t = None
            wrapper.ch = [build(ctx, i, fname) for i in inits] + [root]
            wrapper.s = min(x.s for x in wrapper.ch)
            wrapper.e = root.e
            wrapper.ok = False
            root = wrapper
        F = Func(root, None, src)
        F.run()
        stats = collections.Counter()
        sites = find_sites(F, stats)
        self.stats.update(stats)
        self.stats['functions'] += 1
        if not sites:
            return
        ordered = []
        try:
            edits = render_fn(F, sites, ordered)
        except ValueError:
            self.stats['skip_render'] += len(sites)
            return
        qn = qualified(c)
        for s in sites.values():
            n, form, P, C, comp, t = s[:6]
            line = src.count(b'\n', 0, n.s) + 1
            self.sitelog.append((rel, str(line), qn, form + SUFFIX[t],
                                 ('mirror' if len(s) > 6 else 'site') + ('+ordered' if n.id in ordered else ''),
                                 src[n.s:n.e].decode('utf-8', 'replace').replace('\n', ' ').replace('\t', ' ')[:120]))
            self.stats['sites_' + t] += 1
            if len(s) <= 6:
                self.stats['sites'] += 1
        for s, e, text in edits:
            self.edits[rel][(s, e)] = text
        self.srcfile_of = getattr(self, 'srcfile_of', {})
        self.srcfile_of[rel] = fname


def qualified(c):
    parts = []
    x = c
    while x is not None and x.kind != CK.TRANSLATION_UNIT:
        if x.spelling:
            parts.append(x.spelling)
        x = x.semantic_parent
    return '::'.join(reversed(parts))


def worker(args):
    global INLINE
    entries, roots, libfile, INLINE = args
    if libfile:
        ci.Config.set_library_file(libfile)
    lib = setup_lib()
    rw = Rewriter(roots)
    for e in entries:
        try:
            rw.unit(e, lib)
        except Exception as ex:  # keep going; report
            rw.stats['units_failed'] += 1
            rw.sitelog.append(('#failed', e['file'], repr(ex)[:300]))
    return (dict(rw.edits), rw.sitelog, rw.stats, getattr(rw, 'srcfile_of', {}), rw.done)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--compile-commands', action='append', default=[],
                    help='a build\'s compile_commands.json (repeatable: the sites of every build are merged, '
                         'as a unit can differ between the word sizes); default build/linux-32 and build/linux-64')
    ap.add_argument('--decomp', default=os.path.join(ROOT, 'decomp'))
    ap.add_argument('--libclang', default='')
    ap.add_argument('--only', default='')
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--out', default='')
    ap.add_argument('--patch', default='')
    ap.add_argument('--sites', default='')
    ap.add_argument('--inline', default='auto', choices=('auto', 'off'),
                    help='model MWCC -inline auto (the game) or -inline off (for tools/fmacount --mwcc "-inline off")')
    ap.add_argument('--test', default='', help='print the sites per function of one standalone C++ file')
    args = ap.parse_args()
    libfile = args.libclang
    if not libfile:
        for cand in ('/usr/lib/llvm-18/lib/libclang.so.1', '/usr/lib/x86_64-linux-gnu/libclang-18.so.1'):
            if os.path.exists(cand):
                libfile = cand
                break
    global INLINE
    INLINE = args.inline
    if args.test:
        if libfile:
            ci.Config.set_library_file(libfile)
        lib = setup_lib()
        path = os.path.realpath(args.test)
        rw = Rewriter([os.path.dirname(path)])
        rw.wanted = lambda rel: rel is not None
        rw.unit({'file': path, 'command': 'c++ -x c++ -std=gnu++03 -c ' + path}, lib)
        per = collections.Counter()
        for r in rw.sitelog:
            if not r[0].startswith('#') and r[4].startswith('site'):
                per[r[2]] += 1
            elif r[0].startswith('#'):
                print(r)
        for k in sorted(rw.stats):
            print('#', k, rw.stats[k])
        for fn in sorted(set(r[2] for r in rw.sitelog if not r[0].startswith('#')) | set(per)):
            print('%s: %d' % (fn, per[fn]))
        for r in rw.sitelog:
            if not r[0].startswith('#'):
                print('   ', r[2], r[3], r[4], r[5])
        return
    ccs = args.compile_commands or [p for p in (os.path.join(ROOT, 'build', 'linux-32', 'compile_commands.json'),
                                                os.path.join(ROOT, 'build', 'linux-64', 'compile_commands.json'))
                                    if os.path.exists(p)]
    decomp = os.path.realpath(args.decomp)
    # each build's patched/ tree (decomp + decomp-patches) holds the same text
    roots = [os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(c)), 'patched')) for c in ccs]
    roots += [decomp, os.path.abspath(args.decomp)]
    entries = []
    for cc in ccs:
        for e in json.load(open(cc)):
            f = e['file']
            rel = None
            for r in roots:
                if f.startswith(r + '/'):
                    rel = f[len(r) + 1:]
            if rel is None or not (rel.startswith('src/') or rel.startswith('libs/JSystem/')):
                continue
            if args.only and not re.search(args.only, rel):
                continue
            entries.append(e)
    # big units first, spread over the jobs
    entries.sort(key=lambda e: -os.path.getsize(e['file']))
    chunks = [entries[i::args.jobs] for i in range(args.jobs)]
    edits = collections.defaultdict(dict)
    sitelog = []
    stats = collections.Counter()
    srcfile = {}
    seen_fn = set()
    conflicts = 0
    with concurrent.futures.ProcessPoolExecutor(args.jobs) as ex:
        for ed, log, st, sf, done in ex.map(worker, [(c, roots, libfile, args.inline) for c in chunks]):
            srcfile.update(sf)
            for rel, m in ed.items():
                for rng, text in m.items():
                    old = edits[rel].get(rng)
                    if old is not None and old != text:
                        conflicts += 1
                    edits[rel][rng] = text
            # a function seen by several workers is logged once per worker
            for row in log:
                key = tuple(row[:3]) + tuple(row[4:])
                if row[0].startswith('#') or key not in seen_fn:
                    seen_fn.add(key)
                    sitelog.append(row)
            for k, v in st.items():
                if k in ('units', 'units_failed', 'units_with_errors'):
                    stats[k] += v
    # statistics over the unique sites
    uniq = [r for r in sitelog if not r[0].startswith('#')]
    stats['sites'] = sum(1 for r in uniq if r[4].startswith('site'))
    stats['mirrored'] = sum(1 for r in uniq if r[4].startswith('mirror'))
    stats['ordered'] = sum(1 for r in uniq if r[4].endswith('+ordered'))
    for r in uniq:
        stats['form_' + r[3]] += 1
    stats['conflicts'] = conflicts
    stats['files'] = len(edits)
    # apply
    changed = {}
    for rel, m in edits.items():
        fname = srcfile[rel]
        src = open(fname, 'rb').read()
        rngs = sorted(m)
        out = []
        pos = 0
        for (s, e) in rngs:
            if s < pos:
                stats['overlapping_edits'] += 1
                continue
            out.append(src[pos:s])
            out.append(m[(s, e)])
            pos = e
        out.append(src[pos:])
        changed[rel] = (fname, src, b''.join(out))
    if args.out:
        for rel, (fname, old, new) in changed.items():
            p = os.path.join(args.out, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(new)
    if args.patch:
        with open(args.patch, 'wb') as fh:
            for rel in sorted(changed):
                fname, old, new = changed[rel]
                a = old.decode('utf-8', 'surrogateescape').splitlines(True)
                b = new.decode('utf-8', 'surrogateescape').splitlines(True)
                for line in difflib.unified_diff(a, b, 'a/' + rel, 'b/' + rel, n=2):
                    if not line.endswith('\n'):
                        line += '\n\\ No newline at end of file\n'
                    fh.write(line.encode('utf-8', 'surrogateescape'))
    if args.sites:
        with open(args.sites, 'w') as fh:
            for r in sitelog:
                fh.write('\t'.join(r) + '\n')
    for k in sorted(stats):
        print('%-24s %d' % (k, stats[k]))


if __name__ == '__main__':
    main()
