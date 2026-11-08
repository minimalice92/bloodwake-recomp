"""Run-time fixes for known xboxrecomp v0.16.0 bugs (D-010, docs/TOOLKIT_ISSUES.md).

The toolkit checkout stays exactly v0.16.0, and preflight and the runner both
check that. These fixes are applied in memory, only inside our own pipeline
runs. Each one refuses to apply unless the toolkit code it fixes is exactly the
code it was written against (a SHA-256 of that function's source). So a
toolkit upgrade stops the pipeline until every fix has been re-checked: dropped
if upstream fixed the bug, rewritten if not.

    py -3 tools/xbr_patches.py --toolkit ../xboxrecomp list
    py -3 tools/xbr_patches.py --toolkit ../xboxrecomp recomp <tools.recomp args>

The runner uses the second form for its recomp step (cwd = the toolkit).
Other scripts can `import xbr_patches; xbr_patches.apply()` after putting the
toolkit on sys.path.
"""
import builtins
import hashlib
import inspect
import io
import os
import runpy
import sys


class _AsCmov:
    """An FCMOVcc seen through the carry check as the CMOVcc it behaves like.

    Only `mnemonic` changes. Every other attribute reads through to the real
    instruction, so the check sees operands and op_str unchanged."""
    __slots__ = ("_insn", "mnemonic")

    def __init__(self, insn):
        self._insn = insn
        self.mnemonic = "cmov" + insn.mnemonic[len("fcmov"):]

    def __getattr__(self, name):
        return getattr(self._insn, name)


def _tk2_target():
    from tools.recomp.translator import FunctionTranslator
    return FunctionTranslator.__dict__["_function_needs_cf"].__func__


def _tk2_apply(original):
    """TK-2: an FCMOVcc that reads CF must make its function declare and
    compute _cf, exactly as a CMOVcc does. v0.16.0's check only matches
    mnemonics starting "cmov", so `sub ...; fcmovnb ...` lifted to `!_cf`
    with _cf never declared (compile error C2065)."""
    from tools.recomp.translator import FunctionTranslator

    def _function_needs_cf(instructions):
        return original([_AsCmov(i) if i.mnemonic.startswith("fcmov") else i
                         for i in instructions])

    _function_needs_cf.__doc__ = original.__doc__
    _function_needs_cf.xbr_fix = "TK-2"
    FunctionTranslator._function_needs_cf = staticmethod(_function_needs_cf)


# TK-33 (agent X1, 2026-10-27; docs/remaster/x1_loading_crash.md): one-operand
# MUL/IMUL/DIV/IDIV lifted at their real width. Every edition (a port bug).

# ── The C this fix emits ─────────────────────────────────────
#
# SRC is the operand as the lifter formats it (LO8(ebx), MEM8(ebp + -20),
# LO16(ecx), ...). It is read into a temporary first: `imul al`, `mul dl`,
# `div dh` and `div dx` all name a register the instruction also writes.
# Division by zero and quotient overflow (#DE on hardware) are left as the
# 32-bit lifting already leaves them: unchecked.

_TEMPLATES = {
    ("imul", 1): ("{ int32_t _mx_s = (int8_t)(SRC);"
                  " int32_t _mx_p = (int32_t)(int8_t)LO8(eax) * _mx_s;"
                  " eax = (eax & 0xFFFF0000u) | ((uint32_t)_mx_p & 0xFFFFu); }"
                  "  /* imul r/m8: AX = AL * src; EDX untouched (TK-33) */"),
    ("mul", 1): ("{ uint32_t _mx_s = (uint8_t)(SRC);"
                 " uint32_t _mx_p = (uint32_t)LO8(eax) * _mx_s;"
                 " eax = (eax & 0xFFFF0000u) | (_mx_p & 0xFFFFu); }"
                 "  /* mul r/m8: AX = AL * src; EDX untouched (TK-33) */"),
    ("imul", 2): ("{ int32_t _mx_s = (int16_t)(SRC);"
                  " int32_t _mx_p = (int32_t)(int16_t)LO16(eax) * _mx_s;"
                  " eax = (eax & 0xFFFF0000u) | ((uint32_t)_mx_p & 0xFFFFu);"
                  " edx = (edx & 0xFFFF0000u) | (((uint32_t)_mx_p >> 16) & 0xFFFFu); }"
                  "  /* imul r/m16: DX:AX = AX * src (TK-33) */"),
    ("mul", 2): ("{ uint32_t _mx_s = (uint16_t)(SRC);"
                 " uint32_t _mx_p = (uint32_t)LO16(eax) * _mx_s;"
                 " eax = (eax & 0xFFFF0000u) | (_mx_p & 0xFFFFu);"
                 " edx = (edx & 0xFFFF0000u) | (_mx_p >> 16); }"
                 "  /* mul r/m16: DX:AX = AX * src (TK-33) */"),
    ("div", 1): ("{ uint32_t _mx_s = (uint8_t)(SRC);"
                 " uint32_t _mx_n = eax & 0xFFFFu;"
                 " uint32_t _mx_q = _mx_n / _mx_s, _mx_r = _mx_n % _mx_s;"
                 " eax = (eax & 0xFFFF0000u) | ((_mx_r & 0xFFu) << 8) | (_mx_q & 0xFFu); }"
                 "  /* div r/m8: AL = AX / src, AH = AX % src (TK-33) */"),
    ("idiv", 1): ("{ int32_t _mx_s = (int8_t)(SRC);"
                  " int32_t _mx_n = (int16_t)(eax & 0xFFFFu);"
                  " int32_t _mx_q = _mx_n / _mx_s, _mx_r = _mx_n % _mx_s;"
                  " eax = (eax & 0xFFFF0000u) | (((uint32_t)_mx_r & 0xFFu) << 8)"
                  " | ((uint32_t)_mx_q & 0xFFu); }"
                  "  /* idiv r/m8: AL = AX / src, AH = AX % src (TK-33) */"),
    ("div", 2): ("{ uint32_t _mx_s = (uint16_t)(SRC);"
                 " uint32_t _mx_n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu);"
                 " uint32_t _mx_q = _mx_n / _mx_s, _mx_r = _mx_n % _mx_s;"
                 " eax = (eax & 0xFFFF0000u) | (_mx_q & 0xFFFFu);"
                 " edx = (edx & 0xFFFF0000u) | (_mx_r & 0xFFFFu); }"
                 "  /* div r/m16: AX = DX:AX / src, DX = DX:AX % src (TK-33) */"),
    ("idiv", 2): ("{ int32_t _mx_s = (int16_t)(SRC);"
                  " int32_t _mx_n = (int32_t)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu));"
                  " int32_t _mx_q = _mx_n / _mx_s, _mx_r = _mx_n % _mx_s;"
                  " eax = (eax & 0xFFFF0000u) | ((uint32_t)_mx_q & 0xFFFFu);"
                  " edx = (edx & 0xFFFF0000u) | ((uint32_t)_mx_r & 0xFFFFu); }"
                  "  /* idiv r/m16: AX = DX:AX / src, DX = DX:AX % src (TK-33) */"),
}


def _narrow(mnemonic, ops):
    """C for a one-operand 8/16-bit form, or None to keep the toolkit's."""
    from tools.recomp import lifter as L
    if len(ops) != 1:
        return None
    width = L._operand_width(ops[0]) or 4
    tpl = _TEMPLATES.get((mnemonic, width))
    if tpl is None:
        return None
    return [tpl.replace("SRC", L._fmt_operand_read(ops[0]))]


# ── xbr_patches entries ──────────────────────────────────────

def _tk33_imul_target():
    from tools.recomp.lifter import Lifter
    return Lifter.__dict__["_lift_imul"]


def _tk33_imul_apply(original):
    from tools.recomp.lifter import Lifter

    def _lift_imul(self, insn, ops):
        return _narrow("imul", ops) or original(self, insn, ops)

    _lift_imul.__doc__ = original.__doc__
    _lift_imul.xbr_fix = "TK-33"
    Lifter._lift_imul = _lift_imul


def _tk33_muldiv_target():
    from tools.recomp.lifter import Lifter
    return Lifter.__dict__["_lift_muldiv"]


def _tk33_muldiv_apply(original):
    from tools.recomp.lifter import Lifter

    def _lift_muldiv(self, insn, ops, m):
        return _narrow(m, ops) or original(self, insn, ops, m)

    _lift_muldiv.__doc__ = original.__doc__
    _lift_muldiv.xbr_fix = "TK-33"
    Lifter._lift_muldiv = _lift_muldiv


# TK-83 (0.3.3; a finding of the Apple Silicon fork, docs/next/FORK_APPLE_SILICON.md):
# the runtime header's MMX_PMADDWD adds its two int32 products in int32. Each
# product fits, but their sum doesn't for one input, all four words 0x8000
# (2^30 + 2^30 = 2^31): signed overflow, undefined behaviour in C. x86 gives
# 0x80000000 there, and MSVC's code does too in practice (our A/Bs are exact),
# but a compiler may assume it never happens. Summed in int64_t and kept as
# the lane's unsigned 32-bit pattern, every input gives x86's result with
# nothing undefined. The game's 82 uses are libjpeg's MMX IDCTs and colour
# conversion (D3DX's JPEG decoder: the front end's background pictures).
#
# The pipeline copies the header from the toolkit's template
# (BatchTranslator.translate_batch_split: read it, compare it with the copy in
# the generated folder, write it if different). This fix gives that one read
# the corrected text, so the toolkit's own compare-and-write runs as before
# (an unchanged header isn't rewritten, so builds stay incremental). It
# refuses, stopping the pipeline, unless the template's function is exactly
# the text below.

_PMADDWD_OLD = (
    "static inline RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {\n"
    "    RecompMmx r;\n"
    "    r.d[0] = (int32_t)a.w[0] * b.w[0] + (int32_t)a.w[1] * b.w[1];\n"
    "    r.d[1] = (int32_t)a.w[2] * b.w[2] + (int32_t)a.w[3] * b.w[3];\n"
    "    return r;\n"
    "}\n")
_PMADDWD_NEW = (
    "static inline RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {\n"
    "    /* summed in int64_t: all four words 0x8000 sum to 2^31, past int32_t\n"
    "     * (signed overflow); the lane keeps x86's 32-bit pattern (TK-83) */\n"
    "    RecompMmx r;\n"
    "    r.ud[0] = (uint32_t)((int64_t)a.w[0] * b.w[0] + (int64_t)a.w[1] * b.w[1]);\n"
    "    r.ud[1] = (uint32_t)((int64_t)a.w[2] * b.w[2] + (int64_t)a.w[3] * b.w[3]);\n"
    "    return r;\n"
    "}\n")


def _tk83_header(text):
    """The template's text with TK-83's MMX_PMADDWD (refuses any other text)."""
    if text.count(_PMADDWD_OLD) != 1:
        raise RuntimeError(
            "xbr_patches TK-83: templates/runtime/recomp_types.h's MMX_PMADDWD is not the code this fix "
            "was written for. The toolkit changed: re-check this fix against docs/TOOLKIT_ISSUES.md.")
    return text.replace(_PMADDWD_OLD, _PMADDWD_NEW)


def _tk83_target():
    from tools.recomp.translator import BatchTranslator
    return BatchTranslator.__dict__["translate_batch_split"]


def _tk83_apply(original):
    from tools.recomp import translator as T
    from tools.recomp.translator import BatchTranslator
    template = os.path.normcase(os.path.abspath(os.path.join(
        os.path.dirname(T.__file__), "..", "..", "templates", "runtime", "recomp_types.h")))

    def _open(file, mode="r", *args, **kwargs):
        if isinstance(file, str) and "r" in mode and "b" not in mode and "+" not in mode \
                and os.path.normcase(os.path.abspath(file)) == template:
            with builtins.open(file, mode, *args, **kwargs) as f:
                return io.StringIO(_tk83_header(f.read()))
        return builtins.open(file, mode, *args, **kwargs)

    def translate_batch_split(self, *args, **kwargs):
        # the module's own `open` for this call only (its name lookup finds
        # the module's before the builtin); every other file reads as usual
        had = T.__dict__.get("open")
        T.open = _open
        try:
            return original(self, *args, **kwargs)
        finally:
            if had is None:
                del T.open
            else:
                T.open = had

    translate_batch_split.__doc__ = original.__doc__
    translate_batch_split.xbr_fix = "TK-83"
    BatchTranslator.translate_batch_split = translate_batch_split


FIXES = [
    {"id": "TK-2",
     "what": "fcmov after add/sub/shift: declare and compute _cf (toolkit misses fcmov in its carry check)",
     "target": "tools.recomp.translator.FunctionTranslator._function_needs_cf",
     "sha256": "b6a9c1f02866c4b79d7795e7b3786d5d3be605356e91d89bbd08014423890ab0",
     "get": _tk2_target,
     "apply": _tk2_apply},
    {"id": "TK-33",
     "what": "one-operand imul r/m8, r/m16 at their own width (v0.16.0 lifts them as EDX:EAX = EAX*src)",
     "target": "tools.recomp.lifter.Lifter._lift_imul",
     "sha256": "1bba046e154f227cd7c4c3dd6b72626ce5600d9102755bd4f01d91f3c428d8e5",
     "get": _tk33_imul_target,
     "apply": _tk33_imul_apply},
    {"id": "TK-33b",
     "what": "one-operand mul/div/idiv r/m8, r/m16 at their own width (same bug as TK-33)",
     "target": "tools.recomp.lifter.Lifter._lift_muldiv",
     "sha256": "1dd6c5ba8621df535e59d1b43c51ac1ccef591af809ff3610b048138e693ddd4",
     "get": _tk33_muldiv_target,
     "apply": _tk33_muldiv_apply},
    {"id": "TK-83",
     "what": "MMX_PMADDWD summed in int64_t, no signed overflow (the runtime header the pipeline copies)",
     "target": "tools.recomp.translator.BatchTranslator.translate_batch_split",
     "sha256": "9fff44b7960da3d2e0533a7d2101c3045f6fe1b94f782e482c23f0442f8ec0c7",
     "get": _tk83_target,
     "apply": _tk83_apply},
]

_applied = []


def _source_sha256(fn):
    return hashlib.sha256(inspect.getsource(fn).encode("utf-8")).hexdigest()


def apply():
    """Apply every fix once. Returns [{id, what, target, sha256}].
    Raises RuntimeError, applying nothing, if any target has changed."""
    if _applied:
        return list(_applied)
    targets = []
    for fix in FIXES:
        fn = fix["get"]()
        got = _source_sha256(fn)
        if got != fix["sha256"]:
            raise RuntimeError(
                "xbr_patches %s: %s is not the code this fix was written for "
                "(sha256 %s, expected %s). The toolkit changed: re-check this fix "
                "against docs/TOOLKIT_ISSUES.md before running the pipeline."
                % (fix["id"], fix["target"], got[:12], fix["sha256"][:12]))
        targets.append((fix, fn))
    for fix, fn in targets:
        fix["apply"](fn)
        _applied.append({k: fix[k] for k in ("id", "what", "target", "sha256")})
    return list(_applied)


def _use_toolkit(toolkit):
    """Import from the toolkit, never from this repo's tools/ folder."""
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path[:] = [p for p in sys.path
                   if os.path.abspath(p or os.curdir) != here]
    sys.path.insert(0, os.path.abspath(toolkit))


def main(argv):
    if len(argv) < 3 or argv[0] != "--toolkit" or argv[2] not in ("list", "recomp"):
        print(__doc__, file=sys.stderr)
        return 2
    _use_toolkit(argv[1])
    try:
        applied = apply()
    except RuntimeError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 3
    for a in applied:
        print("xbr_patches: applied %s (%s)" % (a["id"], a["what"]), flush=True)
    if argv[2] == "list":
        return 0
    sys.argv = ["tools.recomp"] + argv[3:]
    runpy.run_module("tools.recomp", run_name="__main__", alter_sys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
