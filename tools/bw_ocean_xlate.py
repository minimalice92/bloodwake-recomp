#!/usr/bin/env python3
"""bw_ocean_xlate.py - the native ocean (agent P2a, docs/day1/P2a.md).

Translates the game's own render-vertex functions from the generated C
(src/recomp/gen/recomp_*.c) into native C, and writes the result to
src/recomp/gen/bw_ocean_native.inc. That output is a translation of the
game's code, so it lives in the git-ignored generated folder like every other
generated file; this script and src/bw_ocean_native.c are our own code.

What changes, and what doesn't:
  * The generated code keeps the x87 register stack in a thread-local array
    indexed by a run-time top (g_fp_stack[g_fp_top]). In compiled x87 code the
    stack depth at every instruction is fixed, so here each slot becomes a
    local double (S0..S7) chosen at translation time.
  * The stack frame: every esp-relative access gets its offset from the
    function's entry esp at translation time (pushes, pops, sub/add esp), so
    the frame is addressed as SP + constant. The frame stays in guest memory,
    at the same addresses, so pointers into it (out parameters) work as before.
  * Guest memory is read and written without `volatile`, so the compiler can
    keep values in registers between accesses.
  * x87 precision control: the driver runs this code only with the guest's
    precision control at 24 bits (what the game runs with), where
    RECOMP_FP_PC(x) is recomp_fp_round24(x) = (double)(float)x inside float's
    range. NB_R is that cast; a result outside float's range raises the host's
    overflow/underflow flag, and the driver then throws the native result away
    and runs the game's own function instead (the cast and the long way differ
    only there).
  * Calls between the translated functions are direct C calls; the return
    address is still pushed to the guest stack as before.
Every arithmetic operation, its operands, its order and its rounding are the
generated code's own, so the results are bit-identical by construction; the
driver's verify mode (BW_OCEAN_NATIVE_VERIFY=1) checks that against the game's
function at run time.

Usage: py -3 tools/bw_ocean_xlate.py [--gen src/recomp/gen] [--out FILE]
Fails loudly on any construct it does not know, rather than guessing.
"""
import argparse
import glob
import os
import re
import sys

# Everything below is keyed by guest address, so the translation works with or
# without our function names (names/bw_names.json): the generated C defines
# WaterWorld_GetCell_00097870 with them and sub_00097870 without. main() looks
# the names up in the generated C (gen_names) and fills FUNCS, SPECIAL and
# INSERT_BEFORE with them, so the output is the same text as before either way.

# Translated, in call order (callees first). address -> native name.
FUNCS_AT = [
    (0x00097870, 'nbx_getcell'),     # WaterWorld_GetCell
    (0x00094280, 'nbx_gbp'),         # Water_GetBezierPatch
    (0x00094930, 'nbx_hsa'),         # Water_HeightAndSlopeAt
    (0x00094CD0, 'nbx_gsa'),         # Water_GridSlopeAt
    (0x000961A0, 'nbx_vhn'),         # WaterWorld_VertexHeightNormal
    (0x00098130, 'nbx_batch'),       # WaterWorld_EvaluateVertexBatch's body (hooked: sub_00098130_gen)
]
# Callees handled by hand (in src/bw_ocean_native.c) or not at all.
#   ftol2: consumes ST0, returns eax:edx (crt_ftol2 0xDFB88, MSVC's _ftol2)
#   bail : not translated (the batch's mode-2 query); the driver never enters
#          the translation for mode 2, so reaching it bails to the game's code
SPECIAL_AT = {
    0x000DFB88: dict(kind='ftol2', retpop=4, net=-1),     # crt_ftol2
    0x00095B00: dict(kind='bail', retpop=20, net=1),      # WaterWorld_VertexHeight
}

# Statements that commit to the game's shared state, with a check first:
# Water_GetBezierPatch marks a cell's patch built only after its 16 control
# points are written. The driver's range check runs before that mark, so a
# patch whose arithmetic left float's range is never marked built (the game's
# own function, run after the bail, rebuilds it).
INSERT_BEFORE_AT = {
    0x00094280: [(r'^MEM8\(ecx \+ edx \+ 0x24AB8\) = 1$', 'NB_COMMIT_CHECK();')],
}

# Filled by main() from the generated C, keyed by the names it uses.
FUNCS, SPECIAL, INSERT_BEFORE = [], {}, {}

RE_DEF = re.compile(r'^void (\w*?)([0-9A-F]{8})(_gen)?\(void\)\s*$', re.M)


def gen_names(gen_dir):
    """address -> the name the generated C defines there (sub_X_gen for a hooked body)."""
    out = {}
    for path in sorted(glob.glob(os.path.join(gen_dir, 'recomp_*.c'))):
        with open(path, encoding='utf-8', errors='replace') as f:
            for m in RE_DEF.finditer(f.read()):
                if m.group(1) == '' or m.group(1).endswith('_'):
                    out.setdefault(int(m.group(2), 16), m.group(1) + m.group(2) + (m.group(3) or ''))
    return out


def resolve_names(gen_dir):
    names = gen_names(gen_dir)
    missing = [a for a in [a for a, _ in FUNCS_AT] + list(SPECIAL_AT) if a not in names]
    if missing:
        raise XlateError('no function defined at %s in %s'
                         % (', '.join('0x%08X' % a for a in missing), gen_dir))

    def called(a):                    # a call site names the wrapper, not the _gen body
        return names[a][:-4] if names[a].endswith('_gen') else names[a]
    FUNCS[:] = [(names[a], nat) for a, nat in FUNCS_AT]
    SPECIAL.clear()
    SPECIAL.update({called(a): v for a, v in SPECIAL_AT.items()})
    INSERT_BEFORE.clear()
    INSERT_BEFORE.update({names[a]: v for a, v in INSERT_BEFORE_AT.items()})

RE_LABEL = re.compile(r'^\s*(loc_[0-9A-F]+):\s*;\s*$')
RE_ITAIL = re.compile(r'RECOMP_ITAIL\(')


class XlateError(Exception):
    pass


def find_function(gen_dir, name):
    for path in sorted(glob.glob(os.path.join(gen_dir, 'recomp_*.c'))):
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        m = re.search(r'^void ' + re.escape(name) + r'\(void\)\s*\n\{\n', text, re.M)
        if not m:
            continue
        start = m.end()
        end = text.index('\n}\n', start)
        return path, text[start:end].split('\n')
    raise XlateError('function %s not found in %s' % (name, gen_dir))


def split_code_comment(line):
    """Code and the trailing comments (kept for reading)."""
    comments = re.findall(r'/\*.*?\*/', line)
    code = re.sub(r'/\*.*?\*/', ' ', line)
    return code.rstrip(), ' '.join(comments)


def split_statements(code):
    """Top-level statements of one line: ';' outside parentheses; '{' and '}'
    are statements of their own."""
    out, cur, depth = [], [], 0
    for ch in code:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if depth == 0 and ch in ';{}':
            s = ''.join(cur).strip()
            if s:
                out.append(s)
            cur = []
            if ch in '{}':
                out.append(ch)
            continue
        cur.append(ch)
    s = ''.join(cur).strip()
    if s:
        out.append(s)
    if depth != 0:
        raise XlateError('unbalanced parentheses: ' + code)
    return out


def balanced_arg(s, i):
    """s[i] is '(' : return (inner, index after the closing ')')."""
    assert s[i] == '('
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '(':
            depth += 1
        elif s[j] == ')':
            depth -= 1
            if depth == 0:
                return s[i + 1:j], j + 1
    raise XlateError('unbalanced: ' + s)


class Fn:
    def __init__(self, gen, nat):
        self.gen, self.nat = gen, nat
        self.retpop = None
        self.net = None


class Translator:
    def __init__(self, funcs_done):
        self.funcs = funcs_done      # gen name -> Fn (already translated)
        self.cover = []              # coverage points: 'function label'

    # ---- expressions (no stack change allowed) ----
    def slot(self, d, k, ctx):
        i = d - 1 - k
        if i < 0 or i > 7:
            raise XlateError('x87 slot ST(%d) at depth %d out of range: %s' % (k, d, ctx))
        return 'S%d' % i

    def xexpr(self, e, d, off):
        ctx = e
        if 'fp_push' in e or 'fp_pop' in e:
            raise XlateError('stack change inside an expression: ' + e)
        e = re.sub(r'g_fp_stack\[\(g_fp_top \+ (\d+)\) & 7\]',
                   lambda m: self.slot(d, int(m.group(1)), ctx), e)
        e = re.sub(r'g_fp_stack\[g_fp_top\]', lambda m: self.slot(d, 0, ctx), e)
        e = re.sub(r'fp_st\((\d+)\)', lambda m: self.slot(d, int(m.group(1)), ctx), e)
        e = re.sub(r'fp_st1\(\)', lambda m: self.slot(d, 1, ctx), e)
        e = re.sub(r'fp_top\(\)', lambda m: self.slot(d, 0, ctx), e)
        if 'g_fp_stack' in e:
            raise XlateError('unknown x87 stack access: ' + ctx)
        e = re.sub(r'\bg_fp_top\b', '((TOP - %du) & 7u)' % d, e)
        e = re.sub(r'\besp\b', '(SP + (%d))' % off, e)
        e = re.sub(r'\b(SMEM8|SMEM16|SMEM32|SMEM64|MEM8|MEM16|MEM32|MEMF|MEMD)\(', r'N\1(', e)
        e = re.sub(r'\bRECOMP_FP_PC\(', 'NB_R(', e)
        e = re.sub(r'\bg_fp_control_word\b', 'CW', e)
        e = re.sub(r'\bg_fp_cmp\b', 'fcmp', e)
        e = re.sub(r'\bg_fp_cc\b', 'fcc', e)
        e = re.sub(r'\bg_seh_ebp\b', 'gsebp', e)
        e = re.sub(r'\bg_ebp\b', 'gebp', e)
        if re.search(r'\bg_[a-z]', e) and not re.search(r'\bg_xbox_mem_offset\b', e):
            raise XlateError('unknown global in: ' + ctx)
        if 'recomp_fp_trig_in_range' in e or 'RECOMP_ITAIL' in e or 'RECOMP_ABI_CALL' in e:
            raise XlateError('unexpected construct: ' + ctx)
        return e

    # ---- one function ----
    def translate(self, fn, body):
        # drop the per-function fp macros (#define spanning lines with '\')
        lines, skip = [], False
        for l in body:
            s = l.strip()
            if skip:
                skip = s.endswith('\\')
                continue
            if s.startswith('#define') or s.startswith('#undef'):
                skip = s.endswith('\\')
                continue
            lines.append(l)

        labels = {}            # label -> (d, off)
        # Fixed point: walk until no new label state is found.
        for _ in range(20):
            before = dict(labels)
            out = self.walk(fn, lines, labels, emit=False)
            if labels == before:
                break
        self.inserted = 0
        out = self.walk(fn, lines, labels, emit=True)
        if fn.retpop is None:
            raise XlateError(fn.gen + ': no return found')
        want = len(INSERT_BEFORE.get(fn.gen, []))
        if self.inserted != want:
            raise XlateError('%s: %d of %d commit checks placed' % (fn.gen, self.inserted, want))
        # every reachable line must have been translated: count labels never reached
        unreached = [l for l in re.findall(r'^\s*(loc_[0-9A-F]+):', '\n'.join(lines), re.M)
                     if l not in labels]
        if unreached:
            sys.stderr.write('bw_ocean_xlate: %s: %d labels never reached (dead code?): %s\n'
                             % (fn.gen, len(unreached), ' '.join(unreached[:8])))
        head = ('/* %s (generated by tools/bw_ocean_xlate.py from %s) */\n'
                'static double %s(NbCtx *NBC, NbRegs *NBR, uint32_t SP, unsigned TOP)\n{\n'
                '    NB_ENTER();\n' % (fn.nat, fn.gen, fn.nat))
        return head + '\n'.join(out) + '\n    NB_UNREACHABLE();\n}\n'

    def note(self, labels, lab, st, where):
        if lab in labels and labels[lab] != st:
            raise XlateError('%s: inconsistent state at %s: %s vs %s'
                             % (where, lab, labels[lab], st))
        labels[lab] = st

    def walk(self, fn, lines, labels, emit):
        out = []
        st = (0, 0)            # (x87 depth, esp offset); None = unreachable
        jt_open = False
        for raw in lines:
            m = RE_LABEL.match(raw)
            if m:
                lab = m.group(1)
                if st is not None:
                    self.note(labels, lab, st, fn.gen)
                st = labels.get(lab)
                if emit and st is not None:
                    # basic-block coverage (verify mode only: COV is NULL otherwise)
                    out.append('%s: ; NB_COVER(%d);' % (lab, len(self.cover)))
                    self.cover.append('%s %s' % (fn.gen, lab))
                else:
                    out.append('%s: ;' % lab)
                continue
            code, comment = split_code_comment(raw)
            if not code.strip():
                continue
            if st is None:
                continue        # unreachable for now (or dead code)
            d, off = st
            res = []
            stmts = split_statements(code)
            i = 0
            while i < len(stmts):
                s = stmts[i]
                i += 1
                if st is None:
                    break
                d, off = st
                # x87 trig with its range test (conditional push on the x87:
                # out of range leaves the stack as it was; here, bail)
                if s.startswith('if (recomp_fp_trig_in_range('):
                    if i + 4 <= len(stmts) and stmts[i] == '{' and stmts[i + 1] == 'double _c' \
                            and re.match(r'fp_top\(\) = recomp_x87_sincos\(fp_top\(\), &_c\)$', stmts[i + 2]) \
                            and stmts[i + 3] == 'fp_push(_c)' and stmts[i + 4] == '}':
                        t = self.slot(d, 0, s)
                        if d + 1 > 8:
                            raise XlateError('x87 overflow at fsincos')
                        res.append('if (!NB_TRIG_OK(%s)) NB_BAIL(); fcc &= (uint16_t)~0x0400u; '
                                   '{ double _c; %s = recomp_x87_sincos(%s, &_c); S%d = _c; }'
                                   % (t, t, t, d))
                        st = (d + 1, off)
                        i += 5
                        continue
                    mm = re.match(r'if \(recomp_fp_trig_in_range\(fp_top\(\)\)\) fp_top\(\) = (recomp_x87_(sin|cos))\(fp_top\(\)\)$', s)
                    if mm:
                        t = self.slot(d, 0, s)
                        res.append('if (!NB_TRIG_OK(%s)) NB_BAIL(); fcc &= (uint16_t)~0x0400u; %s = %s(%s);'
                                   % (t, t, mm.group(1), t))
                        continue
                    raise XlateError('unknown trig form: ' + code)
                if s in ('{', '}'):
                    res.append(s)
                    continue
                if s.startswith('fp_push('):
                    arg, j = balanced_arg(s, len('fp_push'))
                    if s[j:].strip():
                        raise XlateError('fp_push tail: ' + s)
                    if d + 1 > 8:
                        raise XlateError('x87 overflow: ' + code)
                    res.append('S%d = (%s);' % (d, self.xexpr(arg, d, off)))
                    st = (d + 1, off)
                    continue
                if s == 'fp_pop()':
                    if d < 1:
                        raise XlateError('x87 underflow: ' + code)
                    st = (d - 1, off)
                    continue
                m2 = re.match(r'PUSH32\(esp, (.*)\)$', s)
                if m2:
                    v = self.xexpr(m2.group(1), d, off)
                    res.append('{ uint32_t _pv = (uint32_t)(%s); NMEM32(SP + (%d)) = _pv; }' % (v, off - 4))
                    st = (d, off - 4)
                    continue
                m2 = re.match(r'POP32\(esp, (\w+)\)$', s)
                if m2:
                    if m2.group(1) == 'esp':
                        raise XlateError('pop esp')
                    res.append('%s = NMEM32(SP + (%d));' % (m2.group(1), off))
                    st = (d, off + 4)
                    continue
                m2 = re.match(r'esp = esp ([-+]) (0x[0-9A-Fa-f]+|\d+)$', s) or \
                    re.match(r'esp ([-+])= (0x[0-9A-Fa-f]+|\d+)$', s)
                if m2:
                    k = int(m2.group(2), 0)
                    st = (d, off + (k if m2.group(1) == '+' else -k))
                    continue
                if s == 'return':
                    if fn.retpop is not None and (fn.retpop != off or fn.net != d):
                        raise XlateError('%s: returns differ (%s,%s) vs (%s,%s)'
                                         % (fn.gen, fn.retpop, fn.net, off, d))
                    fn.retpop, fn.net = off, d
                    if d not in (0, 1):
                        raise XlateError('%s returns with depth %d' % (fn.gen, d))
                    res.append('NB_LEAVE(); return %s;' % ('S0' if d == 1 else '0.0'))
                    st = None
                    continue
                m2 = re.match(r'RECOMP_ABI_CALL\(0x([0-9A-Fa-f]+)u, (\w+)\)$', s)
                if m2:
                    name = m2.group(2)
                    top = '((TOP - %du) & 7u)' % d
                    if name in self.funcs:
                        cf = self.funcs[name]
                        res.append('NB_SAVE(); { double _r = %s(NBC, NBR, SP + (%d), %s); (void)_r; NB_LOAD();'
                                   % (cf.nat, off, top))
                        if cf.net == 1:
                            res.append(' S%d = _r;' % d)
                        res.append(' }')
                        st = (d + cf.net, off + cf.retpop)
                    elif name in SPECIAL:
                        sp = SPECIAL[name]
                        if sp['kind'] == 'ftol2':
                            res.append('NB_SAVE(); nbx_ftol2(NBC, NBR, SP + (%d), %s); NB_LOAD();'
                                       % (off, self.slot(d, 0, s)))
                        else:
                            res.append('NB_BAIL(); /* %s: not translated */' % name)
                        st = (d + sp['net'], off + sp['retpop'])
                    else:
                        raise XlateError('call to untranslated %s' % name)
                    continue
                if RE_ITAIL.search(s):
                    res.append('NB_BAIL();')
                    # the `return` that follows is dead
                    if i < len(stmts) and stmts[i] == 'return':
                        i += 1
                    st = None
                    # the closing brace of the jump-table block still prints
                    while i < len(stmts):
                        if stmts[i] == '}':
                            res.append('}')
                        i += 1
                    continue
                m2 = re.match(r'goto (loc_[0-9A-F]+)$', s)
                if m2:
                    self.note(labels, m2.group(1), st, fn.gen)
                    res.append(s + ';')
                    st = None
                    continue
                m2 = re.match(r'if \((.*)\) goto (loc_[0-9A-F]+)$', s)
                if m2:
                    self.note(labels, m2.group(2), st, fn.gen)
                    res.append('if (%s) goto %s;' % (self.xexpr(m2.group(1), d, off), m2.group(2)))
                    continue
                if re.search(r'\besp\s*([-+&|]?=)(?!=)', s) and not re.search(r'\besp\s*==', s):
                    raise XlateError('unknown esp write: ' + s)
                for pat, ins in INSERT_BEFORE.get(fn.gen, []):
                    if re.match(pat, s):
                        res.append(ins)
                        self.inserted += 1
                res.append(self.xexpr(s, d, off) + ';')
            if emit and res:
                txt = '    ' + ' '.join(res)
                if comment:
                    txt += ' ' + comment
                out.append(txt)
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(__file__), '..', 'src', 'recomp', 'gen'))
    ap.add_argument('--out', default=None)
    ap.add_argument('--stamp', default=None,
                    help='a file touched after every run, for the build to tell it is up to date')
    a = ap.parse_args()
    gen = os.path.normpath(a.gen)
    out_path = a.out or os.path.join(gen, 'bw_ocean_native.inc')
    done = {}
    parts = ['/* bw_ocean_native.inc - GENERATED by tools/bw_ocean_xlate.py from the\n'
             ' * generated C in this folder; a translation of the game\'s code, so it\n'
             ' * stays out of git like the rest of src/recomp/gen. Included by\n'
             ' * src/bw_ocean_native.c, which defines the NB_* macros. */\n']
    tr = Translator(done)
    srcs = []
    try:
        resolve_names(gen)
    except XlateError as e:
        sys.stderr.write('bw_ocean_xlate: %s\n' % e)
        return 2
    for gen_name, nat in FUNCS:
        path, body = find_function(gen, gen_name)
        srcs.append('%s (%s)' % (gen_name, os.path.basename(path)))
        fn = Fn(gen_name, nat)
        try:
            parts.append(tr.translate(fn, body))
        except XlateError as e:
            sys.stderr.write('bw_ocean_xlate: %s\n' % e)
            return 2
        parts.append('#define %s_RETPOP %d\n#define %s_NET %d\n\n' % (nat.upper(), fn.retpop, nat.upper(), fn.net))
        done[gen_name] = fn
    parts.insert(1, '/* from: ' + ', '.join(srcs) + ' */\n\n')
    nl = chr(10)
    parts.append('#ifndef NB_COVER_TABLE' + nl + '#define NB_COVER_TABLE' + nl
                 + '#define NB_COVER_N %d' % len(tr.cover) + nl
                 + 'static const char *const nb_cover_names[NB_COVER_N] = {' + nl
                 + (',' + nl).join('    "%s"' % c for c in tr.cover) + nl
                 + '};' + nl + '#endif' + nl)
    text = ''.join(parts)
    old = None
    if os.path.exists(out_path):
        with open(out_path, encoding='utf-8') as f:
            old = f.read()
    if old != text:
        with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    if a.stamp:
        # The build's up-to-date mark, touched on every run. The output itself is
        # rewritten only when it changes, so bw_ocean_native.c recompiles only then.
        with open(a.stamp, 'w', encoding='utf-8') as f:
            f.write('written' if old != text else 'unchanged')
    print('bw_ocean_xlate: %d functions -> %s (%s)' % (len(FUNCS), out_path,
                                                      'written' if old != text else 'unchanged'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
