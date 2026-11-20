#!/usr/bin/env python3
# From santiquiroz/gears-of-war-2-pc-port (tools/localize_fix.py, commit 00e3085), BSD 3-Clause, unmodified below this
# header. Copyright (c) 2026, Santiago Quiroz Upegui and the gow2-recomp contributors - the licence text is in
# tools/localize_fix.LICENSE.txt and THIRD_PARTY_NOTICES.md. A build-time tool: it is not part of any release.
# Run by tools/build.ps1 after code generation when the manifest sets non_argument_as_local = true.
# Post-codegen pass for the *_as_local codegen options. A register codegen made a C++ local is
# private to each function, but guest code shares registers across calls: CRT funclets take the
# parent's frame in r12, code split into several functions reads its parent's registers, and some
# functions read a non-volatile on a path that never set it, getting whatever an ancestor left.
# This pass runs a liveness analysis over the generated C++ (control flow from labels, gotos,
# returns, switch and if blocks; a call reads what its callee needs; a register a function never
# names passes through it to its callees), puts the registers a function reads from its caller
# back in ctx there, and makes every direct caller copy its locals to ctx around such a call.
# non_volatile_as_local also drops the __save*/__rest* helper calls; where a register still lives
# in ctx, or the codegen copied a local to ctx before a funclet call, the function now saves it
# on entry and restores it on every return, as the helpers did.
# usage: localize_fix.py "<generated/default>/*.cpp" [--dry-run] [--verbose] [--list=N] [--why=FUNC]
import collections
import glob
import pathlib
import re
import sys

FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\) \{$")
DECL = re.compile(r"^\s*PPC\w*Register (\w+)\{\};$")
LABEL = re.compile(r"^(loc_[0-9A-F]{8}):$")
GOTO = re.compile(r"goto (loc_[0-9A-F]{8});")
CALL = re.compile(r"\b(\w+)\(ctx, base\);")
FULL_DEF = re.compile(r"^(\w+)\.(u64|s64|f64)\s*=\s*(.*);$")
METHOD_DEF = re.compile(r"^(\w+)\.(compare(?:<[^>]*>)?|setFromMask)\((.*)\);$")
IGNORED = {"xer", "temp", "vTemp"}  # xer: compares read xer.so, never set by compiled code; temps: codegen scratch
MARK = "// LOCALIZE_FIX"
VERBOSE = "--verbose" in sys.argv


class Func:
    def __init__(self, name, path, start):
        self.name, self.path, self.start = name, path, start
        self.lines = []
        self.locals = []
        self.ctx_regs = set()


# Registers some codegen *_as_local option can make a local. A function that still reads one of
# them from ctx (an SEH funclet keeps the parent's non-volatiles there, a function this pass
# already demoted) needs it from its caller like any live-in local.
LOCALIZABLE = r"(?:r(?:0|2|11|12|1[4-9]|2\d|3[01])|f(?:0|1[4-9]|2\d|3[01])|v(?:1[4-9]|[2-9]\d|1[01]\d|12[0-7])|cr[0-7]|ctr|reserved)"
CTX_REF = re.compile(r"\bctx\.(" + LOCALIZABLE + r")\b")


def ctx_regs_of(func):
    regs = set()
    for line in func.lines:
        regs |= set(CTX_REF.findall(code_of(line)))
    return regs - set(func.locals)


def parse(paths):
    funcs = {}
    for path in paths:
        cur = None
        for i, line in enumerate(open(path, encoding="utf-8", errors="surrogateescape").read().split("\n")):
            m = FUNC.match(line)
            if m:
                cur = Func(m.group(1), path, i)
                funcs[cur.name] = cur
                continue
            if cur is None:
                continue
            if line.startswith("}"):
                cur = None
                continue
            cur.lines.append(line)
            d = DECL.match(line)
            if d and d.group(1) not in IGNORED:
                cur.locals.append(d.group(1))
    for func in funcs.values():
        func.ctx_regs = ctx_regs_of(func)
    return funcs


CODEGEN_FLUSH = re.compile(r"^ctx\.(\w+) = (?:ctx\.)?\1;$")


# Saves and restores this pass wrote only carry a register through unchanged: not a use.
def code_of(line):
    s = line.strip()
    if s.startswith("//") or DECL.match(s):
        return ""
    if MARK in s:
        w = WRAPPED_RETURN.match(line)
        if w:
            return f"if ({w.group(2)}) return;"
        if "lf_in_" in s or "lf_call_" in s:
            return ""
    # Codegen copies every register local to ctx before a call into a funclet, defined or not:
    # the call line already reads what the funclet needs.
    elif CODEGEN_FLUSH.match(s):
        return ""
    return s


def matching_close(lines, i):
    depth = 0
    for j in range(i, len(lines)):
        s = code_of(lines[j])
        depth += s.count("{") - s.count("}")
        if depth <= 0 and j > i:
            return j
    return len(lines) - 1


def build_cfg(lines):
    labels = {}
    for i, line in enumerate(lines):
        m = LABEL.match(line.strip())
        if m:
            labels[m.group(1)] = i
    succ = [[] for _ in lines]
    n = len(lines)
    for i, line in enumerate(lines):
        s = code_of(line)
        targets = [labels[t] for t in GOTO.findall(s) if t in labels]
        ends = s in ("return;",) or re.fullmatch(r"goto loc_[0-9A-F]{8};", s) is not None
        if not ends and i + 1 < n:
            succ[i].append(i + 1)
        succ[i].extend(targets)
        if s.endswith("{") and s.count("{") > s.count("}"):
            close = matching_close(lines, i)
            if close + 1 < n:
                succ[i].append(close + 1)
            if s.startswith("switch"):
                for j in range(i + 1, close):
                    cs = code_of(lines[j])
                    if cs.startswith("case ") or cs.startswith("default:"):
                        succ[i].append(j)
    return succ


LOCAL_REF = re.compile(r"(?<![\w.])([A-Za-z_]\w*)")
STORE = re.compile(r"^(REX_STORE_U(?:8|16|32|64)|simde_mm_store_si128|simde_mm_store_ps)\((.*)\);$")
VEC_DEST = re.compile(r"^(?:\(simde__m128i\*\))?(\w+)\.(?:u8|s8|u16|s16|u32|s32|f32|u64|s64)$")
# Writes treated as whole-register: lwarx fills reserved.u32, stwcx. and mtcrf write cr fields one
# by one and the code after them reads only what they wrote, and `v57 = vTemp;` copies a whole register.
PARTIAL_AS_FULL = re.compile(r"^(reserved|cr\d|v\d+|f\d+|r\d+)(?:\.(?:u32|lt|gt|eq|so))?\s*=\s*(.*);$")
PROLOGUE_SLOT = re.compile(r"^ctx\.r1\.u32 \+ -\d+$")
NON_VOLATILE = re.compile(r"^(r(1[4-9]|2\d|3[01])|f(1[4-9]|2\d|3[01])|v(1[4-9]|2\d|3[01]|6[4-9]|[7-9]\d|1[01]\d|12[0-7]))$")


def split_top(args):
    depth = 0
    for i, c in enumerate(args):
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "," and depth == 0:
            return args[:i], args[i + 1:]
    return args, ""


def use_def(stmt, bit_of, livein_of):
    dfn = 0
    reads = stmt
    spilled = ""
    if stmt.count(";") <= 1:
        m = FULL_DEF.match(stmt) or METHOD_DEF.match(stmt)
        if m and m.group(1) in bit_of:
            dfn = bit_of[m.group(1)]
            reads = m.group(3)
        st = STORE.match(stmt)
        if st:
            addr, value = split_top(st.group(2))
            reg = VEC_DEST.match(addr.strip())
            if reg and reg.group(1) in bit_of:
                dfn = bit_of[reg.group(1)]
                reads = value
            elif PROLOGUE_SLOT.match(addr.strip()):
                # A non-volatile stored below the stack pointer is the prologue saving it
                reads, spilled = addr, value
        pd = PARTIAL_AS_FULL.match(stmt)
        if pd and pd.group(1) in bit_of and not re.search(rf"(?<![\w.]){pd.group(1)}\b", pd.group(2)):
            dfn = bit_of[pd.group(1)]
            reads = pd.group(2)
    use = 0
    for w in set(LOCAL_REF.findall(reads)):
        use |= bit_of.get(w, 0)
    for w in set(LOCAL_REF.findall(spilled)):
        if not NON_VOLATILE.match(w):
            use |= bit_of.get(w, 0)
    for callee in CALL.findall(stmt):
        for reg in livein_of.get(callee, ()):
            use |= bit_of.get(reg, 0)
    return use, dfn


def normalized(func):
    if not func.ctx_regs:
        return [code_of(l) for l in func.lines]
    ref = re.compile(r"\bctx\.(" + "|".join(sorted(func.ctx_regs, key=len, reverse=True)) + r")\b")
    return [ref.sub(r"\1", code_of(l)) for l in func.lines]


def prepare(func):
    tracked = list(func.locals) + sorted(func.ctx_regs)
    func.bit_of = {r: 1 << k for k, r in enumerate(tracked)}
    func.succ = build_cfg(func.lines)
    func.norm = normalized(func)
    func.base_ud = [use_def(s, func.bit_of, {}) for s in func.norm]
    func.calls = [CALL.findall(s) for s in func.norm]


def passed_through(func, livein_of):
    regs = set()
    for callees in func.calls:
        for callee in callees:
            regs |= set(livein_of.get(callee, ()))
    return {r for r in regs if r not in func.bit_of}


# A register this function never names reaches its callees unchanged: their live-ins are its own.
def livein(func, livein_of):
    return tracked_livein(func, livein_of) | passed_through(func, livein_of)


def tracked_livein(func, livein_of):
    if not func.bit_of:
        return set()
    bit_of = func.bit_of
    lines = func.lines
    succ = func.succ
    ud = []
    for (use, dfn), callees in zip(func.base_ud, func.calls):
        for callee in callees:
            for reg in livein_of.get(callee, ()):
                use |= bit_of.get(reg, 0)
        ud.append((use, dfn))
    live_in = [0] * len(lines)
    changed = True
    while changed:
        changed = False
        for i in range(len(lines) - 1, -1, -1):
            out = 0
            for j in succ[i]:
                out |= live_in[j]
            use, dfn = ud[i]
            new = use | (out & ~dfn)
            if new != live_in[i]:
                live_in[i] = new
                changed = True
    entry = live_in[0] if lines else 0
    return {r for r, b in bit_of.items() if entry & b}


def analyse(funcs):
    callers = collections.defaultdict(set)
    for f in funcs.values():
        prepare(f)
        for callees in f.calls:
            for c in callees:
                callers[c].add(f.name)
    livein_of = {}
    work = list(funcs)
    if "--roots" in sys.argv:
        for name in sorted(work):
            odd = sorted(r for r in livein(funcs[name], {}) if r != "r12")
            if odd:
                print(f"  root {name}: {' '.join(odd)}")
    rnd = 0
    while work:
        nxt = set()
        for name in work:
            li = livein(funcs[name], livein_of)
            if li != livein_of.get(name, set()):
                if li:
                    livein_of[name] = li
                else:
                    livein_of.pop(name, None)
                nxt |= callers[name]
        if VERBOSE:
            print(f"round {rnd}: {len(livein_of)} function(s) read a caller's register")
        work = sorted(nxt)
        rnd += 1
    return livein_of


LIST = int(next((a.split("=")[1] for a in sys.argv if a.startswith("--list=")), "0"))
WHY = [a.split("=", 1)[1] for a in sys.argv if a.startswith("--why=")]


def witness(func, reg, livein_of):
    bit = func.bit_of[reg]
    prev = {0: None}
    queue = collections.deque([0])
    while queue:
        i = queue.popleft()
        use, dfn = use_def(func.norm[i], func.bit_of, livein_of)
        if use & bit:
            path = []
            while i is not None:
                path.append(i)
                i = prev[i]
            return path[::-1]
        if dfn & bit:
            continue
        for j in func.succ[i]:
            if j not in prev:
                prev[j] = i
                queue.append(j)
    return []


def explain(funcs, livein_of):
    for name in WHY:
        func = funcs[name]
        for reg in sorted(livein_of.get(name, ())):
            path = witness(func, reg, livein_of)
            print(f"{name} {reg}: {len(path)} lines on the path")
            for i in [k for k in path if LABEL.match(func.lines[k].strip()) or "goto" in func.lines[k] or "(ctx, base)" in func.lines[k]][-8:] + path[-1:]:
                print(f"    {i:5d} {func.lines[i].strip()[:150]}")


def report(livein_of):
    if not (VERBOSE or LIST):
        return
    by_reg = collections.Counter(r for v in livein_of.values() for r in v)
    print("by register:", dict(sorted(by_reg.items(), key=lambda kv: -kv[1])))
    shown = [kv for kv in sorted(livein_of.items()) if not kv[0].startswith("__")]
    for name, regs in shown[:LIST]:
        print(f"  {name}: {' '.join(sorted(regs))}")


def one_pass(paths, dry):
    funcs = parse(paths)
    print(f"{len(funcs)} functions, {sum(1 for f in funcs.values() if f.locals)} with register locals")
    livein_of = analyse(funcs)
    report(livein_of)
    explain(funcs, livein_of)
    if dry:
        return 0
    demoting = sum(1 for f in funcs.values() if demoted_locals(f, livein_of))
    flushes = rewrite(paths, funcs, livein_of)
    print(f"demoted registers in {demoting} function(s), {flushes} call site(s) copy registers through ctx")
    return demoting + flushes


MAX_PASSES = 8


def main():
    paths = sorted(glob.glob(sys.argv[1]))
    dry = "--dry-run" in sys.argv
    # Flushes added by one pass read registers the next pass may find a caller must supply.
    for _ in range(MAX_PASSES):
        if one_pass(paths, dry) == 0:
            return 0
    print(f"localize_fix: still changing after {MAX_PASSES} passes", file=sys.stderr)
    return 1


def demote_line(line, regs):
    if not regs or line.strip().startswith("//"):
        return line
    return re.sub(r"(?<![\w.])(" + "|".join(sorted(regs, key=len, reverse=True)) + r")\b",
                  r"ctx.\1", line)


# The CRT reaches funclets through a pointer with the frame in r12 (sub_82A93144: mtctr r4;
# mr r12,r5; bctr), so an indirect call must see the caller's r12.
INDIRECT_REGS = ("r12",)
INDIRECT = "REX_CALL_INDIRECT_FUNC("
NON_VOLATILE_CR = re.compile(r"^cr[234]$")
SAVE_HELPER = re.compile(r"\{ 0x([0-9A-F]{8}), __save(gprlr|fpr|vmx)_(\d+) \}")
BL_COMMENT = re.compile(r"^\s*// bl 0x([0-9a-f]{8})\s*$")
HELPER_CALL = re.compile(r"\b__(?:save|rest)(?:gprlr|fpr|vmx)_\d+\(ctx, base\);")
FLUSH = re.compile(r"^\s*ctx\.(\w+) = \1;  " + re.escape(MARK) + "$")
SELF_COPY = re.compile(r"^\s*ctx\.(\w+) = ctx\.\1;  " + re.escape(MARK) + "$")
RESTORE_IN = re.compile(r"ctx\.(\w+) = lf_in_\1;")
CALL_SLOT = re.compile(r"\blf_call_(\w+)\b(?!\))")
SLOT_DECL = re.compile(r"^\s*decltype\(ctx\.\w+\) (lf_(?:in|call)_\w+);")
COND_RETURN = re.compile(r"^(\s*)if \((.*)\) return;$")
WRAPPED_RETURN = re.compile(r"^(\s*)if \((.*)\) \{ (.*)return; \}  " + re.escape(MARK) + "$")


def non_volatile(reg):
    return bool(NON_VOLATILE.match(reg) or NON_VOLATILE_CR.match(reg))


def helper_regs(kind, n):
    prefix = {"gprlr": "r", "fpr": "f", "vmx": "v"}[kind]
    top = 128 if kind == "vmx" and n >= 64 else 32
    return {f"{prefix}{k}" for k in range(n, top)}


def save_helpers(gen_dir):
    path = gen_dir / "gow2_init.cpp"
    text = path.read_text(errors="replace") if path.exists() else ""
    return {a.lower(): helper_regs(kind, int(n)) for a, kind, n in SAVE_HELPER.findall(text)}


# non_volatile_as_local drops the calls to __savegprlr_N and __restgprlr_N (and the fpr and vmx
# ones): a register such a function hands back to ctx would no longer be restored on return.
def helper_saved(func, helpers):
    if any(HELPER_CALL.search(line) for line in func.lines):
        return set()
    regs = set()
    for line in func.lines:
        m = BL_COMMENT.match(line)
        if m and m.group(1) in helpers:
            regs |= helpers[m.group(1)]
    return regs


def defined_regs(func, regs):
    return {r for r in regs if any(re.match(rf"^\s*{r}\.(?:u64|s64|u32|s32)\s*=", l) for l in func.lines)}


def flushed_above(out):
    names = set()
    for line in reversed(out):
        if MARK not in line:
            break
        m = FLUSH.match(line)
        if m:
            names.add(m.group(1))
    return names


def last_code(out):
    return next((l.strip() for l in reversed(out) if MARK not in l), "")


def indirect_regs(out, kept, indirect_flush):
    # patch_switch_traps sends an out-of-range switch index to the case address in ctr, which
    # harvest registered as a function of its own: it goes on with every register of this one.
    if last_code(out) == "default:":
        return kept
    return indirect_flush


def mark(indent, stmt):
    return f"{indent}{stmt}  {MARK}"


# Around a call the callee must see these registers in ctx; this function's own caller keeps
# its view of the non-volatile ones, and copy_back returns what the callee left to the locals.
def around_call(indent, line, regs, copy_back, slots):
    nv = sorted(r for r in regs if non_volatile(r))
    slots |= {f"lf_call_{r}" for r in nv}
    before = [mark(indent, f"lf_call_{r} = ctx.{r};") for r in nv]
    before += [mark(indent, f"ctx.{r} = {r};") for r in sorted(regs)]
    after = [mark(indent, f"{r} = ctx.{r};") for r in sorted(regs)] if copy_back else []
    after += [mark(indent, f"ctx.{r} = lf_call_{r};") for r in nv]
    return before + [line] + after


def restored_above(out):
    names = set()
    for line in reversed(out):
        if MARK not in line:
            break
        names |= set(RESTORE_IN.findall(line))
    return names


def with_restores(line, out, entry):
    if not entry:
        return [line]
    m = WRAPPED_RETURN.match(line)
    if m:
        missing = sorted(entry - set(RESTORE_IN.findall(m.group(3))))
        stmts = m.group(3) + "".join(f"ctx.{r} = lf_in_{r}; " for r in missing)
        return [f"{m.group(1)}if ({m.group(2)}) {{ {stmts}return; }}  {MARK}"]
    m = COND_RETURN.match(line)
    if m:
        stmts = "".join(f"ctx.{r} = lf_in_{r}; " for r in sorted(entry))
        return [f"{m.group(1)}if ({m.group(2)}) {{ {stmts}return; }}  {MARK}"]
    if line.strip() == "return;":
        indent = re.match(r"\s*", line).group(0)
        missing = sorted(entry - restored_above(out))
        return [mark(indent, f"ctx.{r} = lf_in_{r};") for r in missing] + [line]
    return [line]


LOCAL_TO_CTX = re.compile(r"^\s*ctx\.(\w+) = \1;$")


def codegen_flushed(func):
    return {m.group(1) for m in map(LOCAL_TO_CTX.match, func.lines) if m}


class FuncRewrite:
    def __init__(self, func, demoted, livein_of, helpers):
        self.demoted = demoted
        self.livein_of = livein_of
        self.kept = set(func.locals) - demoted
        self.indirect_flush = defined_regs(func, set(INDIRECT_REGS) & self.kept)
        # The codegen drops __save*/__rest* calls even where registers stay in ctx (funclets).
        saved = {r for r in (demoted | func.ctx_regs) & helper_saved(func, helpers) if non_volatile(r)}
        # Its own copies into ctx before a funclet call overwrite the caller's value for good.
        flushed = {r for r in codegen_flushed(func) & self.kept if non_volatile(r)}
        self.new_entry = saved | flushed
        self.slots = set()
        self.flushes = 0

    def body(self, lines):
        existing = {m.group(1) for m in map(SLOT_DECL.match, lines) if m}
        self.slots = set(existing)
        saved = {s[len("lf_in_"):] for s in existing if s.startswith("lf_in_")}
        self.new_entry -= saved
        entry = saved | self.new_entry
        self.slots |= {f"lf_in_{r}" for r in self.new_entry}
        out = []
        for line in lines:
            out.extend(self.line(line, out, entry))
        if entry and last_code(out) not in ("return;",) and not last_code(out).startswith("goto "):
            out.extend(mark("\t", f"ctx.{r} = lf_in_{r};") for r in sorted(entry - restored_above(out)))
        return self.prologue(lines, existing) + out

    def prologue(self, lines, existing):
        decls = [mark("\t", f"decltype(ctx.{s.split('_', 2)[2]}) {s};") for s in sorted(self.slots - existing)]
        saves = [mark("\t", f"lf_in_{r} = ctx.{r};") for r in sorted(self.new_entry)]
        return decls + saves

    # Once this function reads R from ctx itself, a callee's change to ctx.R is its change too.
    def stale_call_slot(self, line):
        m = CALL_SLOT.search(line)
        return bool(m and MARK in line and m.group(1) in self.demoted)

    def line(self, line, out, entry):
        d = DECL.match(line)
        if d and d.group(1) in self.demoted:
            return []
        line = demote_line(line, self.demoted)
        if SELF_COPY.match(line) or self.stale_call_slot(line):
            return []
        code = "" if line.strip().startswith("//") else line
        indent = re.match(r"\s*", line).group(0)
        regs = set()
        for callee in CALL.findall(code):
            regs |= set(self.livein_of.get(callee, ())) & self.kept
        regs -= flushed_above(out)
        if regs:
            self.flushes += 1
            return around_call(indent, line, regs, True, self.slots)
        if code.strip().startswith(INDIRECT):
            regs = indirect_regs(out, self.kept, self.indirect_flush) - flushed_above(out)
            self.flushes += bool(regs)
            return around_call(indent, line, regs, False, self.slots) if regs else [line]
        return with_restores(line, out, entry)


def demoted_locals(func, livein_of):
    return set(livein_of.get(func.name, ())) & set(func.locals)


def split_prologue(lines):
    if lines and lines[0].strip() == "REX_FUNC_PROLOGUE();":
        return lines[:1], lines[1:]
    return [], lines


def rewrite_file(path, funcs, livein_of, helpers):
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    out = []
    flushes = 0
    cur = None
    body = []
    for line in text.split("\n"):
        m = FUNC.match(line)
        if m:
            cur, body = funcs.get(m.group(1)), []
            out.append(line)
        elif cur is not None and line.startswith("}"):
            fr = FuncRewrite(cur, demoted_locals(cur, livein_of), livein_of, helpers)
            head, rest = split_prologue(body)
            out.extend(head + fr.body(rest))
            out.append(line)
            flushes += fr.flushes
            cur = None
        elif cur is not None:
            body.append(line)
        else:
            out.append(line)
    new = "\n".join(out)
    if new != text:
        open(path, "w", encoding="utf-8", errors="surrogateescape").write(new)
    return flushes


def rewrite(paths, funcs, livein_of):
    helpers = save_helpers(pathlib.Path(paths[0]).parent) if paths else {}
    return sum(rewrite_file(p, funcs, livein_of, helpers) for p in paths)


if __name__ == "__main__":
    sys.exit(main())
