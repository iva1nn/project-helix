"""Static recompiler: PS-X EXE (+ overlays) -> C.

    python tools/recomp/recomp.py config/fe2.json --disc <cue> --out gen

Reads the exe and overlays from the user's disc image, finds functions, and writes one C function per
MIPS function plus a dispatch table. The output is the game's code: it goes to gen/ (git-ignored).
See NOTES.md "Recompiler design".
"""
import argparse
import bisect
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from disc import Disc  # noqa: E402
from mips import Insn, translate, branch_cond, R  # noqa: E402

FUNCS_PER_FILE = 150


def insn_cycles(i):
    """Rough R3000A cost: RAM has no data cache, so loads stall; stores go through a write buffer."""
    if i is None:
        return 1
    if i.kind == "load":
        return 5
    if i.kind == "store":
        return 2
    if i.name in ("mult", "multu"):
        return 7
    if i.name in ("div", "divu"):
        return 36
    if i.name == "cop2":
        return 10
    return 1


class Image:
    """One contiguous piece of code+data at a fixed address (main exe or one overlay)."""

    def __init__(self, name, base, data, prefix):
        self.name, self.base, self.data, self.prefix = name, base, data, prefix
        self.end = base + len(data)
        self.code_lo = self.code_hi = None
        self.insns = {}

    def word(self, a):
        o = a - self.base
        if 0 <= o <= len(self.data) - 4:
            return struct.unpack_from("<I", self.data, o)[0]
        return None

    def insn(self, a):
        i = self.insns.get(a)
        if i is None:
            w = self.word(a)
            if w is None:
                return None
            i = self.insns[a] = Insn(a, w)
        return i

    def in_code(self, a):
        return self.code_lo <= a < self.code_hi

    def fname(self, a):
        return "%s_%08X" % (self.prefix, a)


def find_code_range(img, start_hint=None):
    """Code = first function prologue (addiu sp,sp,-N) .. last 'jr ra' + delay slot."""
    n = len(img.data) // 4
    words = struct.unpack_from("<%dI" % n, img.data)
    lo = None
    if start_hint is not None:
        lo = start_hint
    else:
        for k, w in enumerate(words):
            if (w >> 16) == 0x27BD and (w & 0x8000):
                lo = img.base + k * 4
                break
    hi = None
    for k in range(n - 1, -1, -1):
        if words[k] == 0x03E00008:
            hi = img.base + k * 4 + 8
            break
    return lo, hi


def overlay_code_start(img, main_refs):
    """An overlay starts with a header (its id word, file names) and may have leaf functions (no stack frame) before
    its first prologue, reached through pointers: its own (FE2 LOCE 800AF4D8, FE1 LOCD / LOCF 800AF14C) or the main
    exe's fixed overlay slots (FE2 800AF0F0, LOCD's, called from the room change; FE1 800AF198). The code start moves
    back to the lowest such address from where every word up to the first prologue decodes as an instruction (not
    a pointer into the overlay: a jump table's words decode as lb) and a return is among them."""
    lo = img.code_lo
    jal, consts, dataptr = scan_seeds(img, [])
    cands = sorted(a for a in jal | consts | dataptr | main_refs if img.base + 4 <= a < lo and a & 3 == 0)
    def code(x):  # an instruction, not a pointer into the overlay (a jump table's words decode as lb)
        w = img.word(x)
        return img.insn(x).kind != "bad" and not (img.base <= w < img.end)
    for c in cands:  # code, with at least one return (jr ra) in it: not a stray string or table
        if all(code(x) for x in range(c, lo, 4)) and any(img.word(x) == 0x03E00008 for x in range(c, lo, 4)):
            return c
    return lo


def scan_seeds(img, other_images):
    """jal targets and lui/addiu constants in code, data words pointing into code."""
    jal, consts, dataptr = set(), set(), set()
    lui = {}
    for a in range(img.code_lo, img.code_hi, 4):
        i = img.insn(a)
        if i.kind == "jal":
            jal.add(i.jump_target())
        if i.name == "lui":
            lui[i.rt] = (a, i.imm << 16)
        elif i.name in ("addiu", "ori") and i.rs in lui and a - lui[i.rs][0] <= 64:
            v = (lui[i.rs][1] + (i.simm if i.name == "addiu" else i.imm)) & 0xFFFFFFFF
            consts.add(v)
        if i.kind in ("jr", "j", "jal", "jalr", "branch"):
            lui.clear()
    for im in [img] + other_images:
        for a in range(im.base, im.end - 3, 4):
            if im is img and im.in_code(a):
                continue
            w = im.word(a)
            if w is not None and w & 3 == 0:
                dataptr.add(w)
    return jal, consts, dataptr


class Func:
    def __init__(self, img, start, end):
        self.img, self.start, self.end = img, start, end
        self.reach = set()
        self.visited = set()  # walked as an instruction in sequence (delay slots are only 'reach')
        self.labels = set()
        self.tails = set()  # targets outside the range reached by j/branch
        self.has_jr_reg = False
        self.jt_labels = set()
        self.falls_through = False
        self.asm = False          # hand-written asm block: internal jal/jr ra are local gotos
        self.local_rets = set()   # return sites of internal jal (asm blocks)
        self.asm_entries = set()  # entry points called from outside (asm blocks)

    def walk(self, extra_labels=()):
        img = self.img
        work = [self.start] + list(extra_labels)
        self.labels |= set(extra_labels)
        while work:
            a = work.pop()
            while self.start <= a < self.end and a not in self.visited:
                i = img.insn(a)
                if i is None:
                    break
                self.visited.add(a)
                self.reach.add(a)
                k = i.kind
                if k in ("branch", "j", "jal", "jr", "jalr"):
                    self.reach.add(a + 4)  # delay slot
                    if k == "branch":
                        t = i.branch_target()
                        if self.start <= t < self.end:
                            self.labels.add(t)
                            work.append(t)
                        else:
                            self.tails.add(t)
                        a += 8
                        continue
                    if k == "j":
                        t = i.jump_target()
                        if self.start <= t < self.end:
                            self.labels.add(t)
                            work.append(t)
                        else:
                            self.tails.add(t)
                        break
                    if k in ("jal", "jalr"):
                        if self.asm and k == "jal" and self.start <= i.jump_target() < self.end:
                            t = i.jump_target()
                            self.labels.add(t)
                            work.append(t)
                            self.local_rets.add(a + 8)
                            self.labels.add(a + 8)
                        a += 8
                        continue
                    if k == "jr":
                        if i.rs != 31:
                            self.has_jr_reg = True
                        break
                if i.kind == "bad":
                    break
                a += 4
            if a == self.end and a not in self.visited and (a - 4) in self.reach:
                self.falls_through = True


class Program:
    def __init__(self, images, extra_seeds, asm_ranges=()):
        self.images = images
        self.extra = extra_seeds
        self.asm_ranges = list(asm_ranges)
        self.asm_entry_seeds = set()
        self.funcs = {}  # (img.name) -> sorted list of Func

    def image_of(self, a):
        for im in self.images:
            if im.code_lo <= a < im.code_hi:
                return im
        return None

    def analyse(self):
        for img in self.images:
            # pointers into main code may come from any overlay; an overlay's code is only pointed to by
            # its own data (other overlays share the same addresses)
            others = [o for o in self.images if o is not img] if img.prefix == "f" else []
            jal, consts, dataptr = scan_seeds(img, others)
            seeds = {a for a in jal | consts if img.in_code(a) and a & 3 == 0}
            seeds |= {a for a in self.extra if img.in_code(a)}
            seeds.add(img.code_lo)
            if getattr(img, "first_prologue", None):
                seeds.add(img.first_prologue)
            if img.entry is not None:
                seeds.add(img.entry)
            img.dataptr = {a for a in dataptr if img.in_code(a)}
            while True:
                funcs = self.partition(img, seeds)
                new = set()
                for f in funcs:
                    new |= {t for t in f.tails if img.in_code(t) and t not in seeds}
                # data pointers into code no function reaches: function entries reached by pointer
                reached = set()
                for f in funcs:
                    reached |= f.reach
                new |= {a for a in img.dataptr if a not in reached and a not in seeds}
                if not new:
                    break
                seeds |= new
            # functions nothing calls that we can see (dead code, or called through pointers built at
            # run time): runs nobody reaches whose first instructions decode as valid code. Their own branches
            # out of range are not trusted as seeds.
            spec = set()
            while True:
              add = set()
              a = img.code_lo
              while a < img.code_hi:
                if a in reached:
                    a += 4
                    continue
                b = a
                while b < img.code_hi and b not in reached:
                    b += 4
                for x in range(a, b, 4):
                    w = img.word(x)
                    if w == 0:
                        continue
                    ok = all(img.insn(y).kind != "bad" for y in range(x, min(b, x + 32), 4))
                    if ok:
                        add.add(x)
                    break
                a = b
              if not add:
                  break
              spec |= add
              seeds |= add
              funcs = self.partition(img, seeds)
              reached = set()
              for f in funcs:
                  reached |= f.reach
            img.spec = spec
            # hand-written asm blocks become one function; seeds inside are its external entries
            inner = {x for x in seeds for lo, hi in self.asm_ranges if lo < x < hi}
            if inner:
                self.asm_entry_seeds |= inner
                seeds -= inner
                for lo, hi in self.asm_ranges:
                    if img.in_code(lo):
                        seeds.add(lo)
                        seeds.add(hi)
                funcs = self.partition(img, seeds)
            # setjmp: code after a call to the BIOS setjmp stub can be resumed directly (interrupt hooks)
            stubs = set()
            for f in funcs:
                w = [img.word(f.start + 4 * k) for k in range(3)]
                if w[0] == 0x240A00A0 and w[1] == 0x01400008 and w[2] == 0x24090013:
                    stubs.add(f.start)
            for f in funcs:
                f.resume = set()
                for a in f.reach:
                    i = img.insn(a)
                    if i.kind == "jal" and i.jump_target() in stubs and (a + 8) in f.reach:
                        f.resume.add(a + 8)
                        f.labels.add(a + 8)
            # hand-written asm that saves its return address as data and later resumes with 'jr ra'
            # (libpress DecDCTvlc): the return sites of calls into such code become entry points
            starts = {f.start: f for f in funcs}
            saver = set()
            for f in funcs:
                for a in f.reach:
                    i = img.insn(a)
                    if i.name == "sw" and i.rt == 31 and i.rs != 29:
                        saver.add(f.start)
                    if i.name in ("or", "addu") and 31 in (i.rs, i.rt) and 0 in (i.rs, i.rt) and i.rd not in (0, 31):
                        saver.add(f.start)  # ra copied to another register
            changed = True
            while changed:
                changed = False
                for f in funcs:
                    if f.start not in saver and (f.tails & saver or (f.falls_through and f.end in saver)):
                        saver.add(f.start)
                        changed = True
            nres = 0
            for f in funcs:
                for a in f.reach:
                    i = img.insn(a)
                    if i.kind == "jal" and (i.jump_target() in saver or f.start in saver) and (a + 8) in f.reach:
                        f.resume.add(a + 8)
                        f.labels.add(a + 8)
                        nres += 1
            if saver:
                print("%s: %d functions store ra as data; %d resume entries" % (img.name, len(saver), nres))
            for f in funcs:
                if f.asm:
                    f.resume |= f.asm_entries
                    f.labels |= f.asm_entries
            self.funcs[img.name] = funcs
            nreach = sum(len(f.reach) for f in funcs)
            print("%s: code %08X-%08X, %d functions, %d of %d words reached" % (
                img.name, img.code_lo, img.code_hi, len(funcs), nreach, (img.code_hi - img.code_lo) // 4))

    def jump_tables(self, img, f):
        """Targets of switch tables: 'jr rN' where rN was loaded from (constant + index)."""
        out = set()
        for a in sorted(f.reach):
            i = img.insn(a)
            if i.kind != "jr" or i.rs == 31:
                continue
            # forward constant propagation over the straight-line code before the jr
            start = a
            while start - 4 >= f.start and (start - 4) in f.reach and a - (start - 4) <= 96:
                j = img.insn(start - 4)
                if j.kind in ("branch", "j", "jal", "jr", "jalr") and start - 4 != a - 4:
                    break
                start -= 4
            const, based = {0: 0}, {}  # reg -> constant value / reg -> constant base of (base + index)
            table = None
            for b in range(start, a, 4):
                j = img.insn(b)
                n = j.name
                if n == "lui":
                    const[j.rt] = j.imm << 16; based.pop(j.rt, None)
                elif n in ("addiu", "ori") and j.rs in const:
                    const[j.rt] = (const[j.rs] + (j.simm if n == "addiu" else j.imm)) & 0xFFFFFFFF
                    based.pop(j.rt, None)
                elif n == "addu":
                    x, y = j.rs, j.rt
                    if x in const and y in const:
                        const[j.rd] = (const[x] + const[y]) & 0xFFFFFFFF; based.pop(j.rd, None)
                    elif x in const or y in const:
                        based[j.rd] = const[x] if x in const else const[y]; const.pop(j.rd, None)
                    elif x in based or y in based:
                        based[j.rd] = based.get(x, based.get(y)); const.pop(j.rd, None)
                    else:
                        const.pop(j.rd, None); based.pop(j.rd, None)
                elif n == "lw" and j.rt == i.rs:
                    if j.rs in based:
                        table = (based[j.rs] + j.simm) & 0xFFFFFFFF
                    elif j.rs in const:
                        table = (const[j.rs] + j.simm) & 0xFFFFFFFF
                    const.pop(j.rt, None); based.pop(j.rt, None)
                else:
                    from mips import writes_reg
                    r = writes_reg(j)
                    if r is not None:
                        const.pop(r, None); based.pop(r, None)
            if table is None:
                continue
            # the table is read from this function's own image first: the overlays share one load address, so
            # the first image covering the address was another overlay (GSHELL) and its words were read instead
            look = [img] + [im for im in self.images if im is not img]
            # the bounds check before the table ('sltiu rT, rIdx, N' + beq/bne on rT): the table has exactly N
            # entries, and entries may point outside this function (case blocks that became functions of their
            # own because the table's words are data pointers). Without it, an entry outside the function ended
            # the scan, so later entries inside it were lost: FE2 LOCA 800B4E2C, whose table starts with
            # 800B506C, jumped to its own label 800B4EB8 through dispatch -> FATAL (the EMP crash, NOTES.md).
            n_bound = None
            b = start - 4
            while b >= f.start and b in f.reach and start - b <= 16:
                j = img.insn(b)
                if j.name == "sltiu" and 0 < j.imm <= 1024:
                    n_bound = j.imm
                    break
                b -= 4
            if n_bound is not None:
                words = []
                for k in range(n_bound):
                    w = None
                    for im in look:
                        if im.base <= table + 4 * k < im.end:
                            w = im.word(table + 4 * k)
                            break
                    if w is None or w & 3 or not img.in_code(w):
                        words = None
                        break
                    words.append(w)
                if words is not None:
                    out |= {w for w in words if f.start <= w < f.end}
            # the scan without a bound (kept as before, so no label is lost where the bound is not found)
            k = 0
            while k < 1024:
                w = None
                for im in look:
                    if im.base <= table + 4 * k < im.end:
                        w = im.word(table + 4 * k)
                        break
                if w is None or not (f.start <= w < f.end) or w & 3:
                    break
                out.add(w)
                k += 1
        return out

    def partition(self, img, seeds):
        s = sorted(seeds)
        funcs = []
        for k, a in enumerate(s):
            end = s[k + 1] if k + 1 < len(s) else img.code_hi
            f = Func(img, a, end)
            for lo, hi in self.asm_ranges:
                if a == lo:
                    f.asm = True
                    f.asm_entries = {x for x in self.asm_entry_seeds if lo < x < hi}
            f.walk(sorted(f.asm_entries))
            if f.has_jr_reg:
                jt = self.jump_tables(img, f)
                if jt:
                    f.walk(sorted(jt))
                    # case blocks may contain further switches
                    jt |= self.jump_tables(img, f)
                    f.walk(sorted(jt))
                f.jt_labels = jt
            funcs.append(f)
        return funcs

    # ---------------------------------------------------------------- emission

    def callee(self, img, t):
        """C call expression for a direct call/jump to t from code in img."""
        for im in [img] + [m for m in self.images if m.prefix == "f"]:
            if im.in_code(t):
                funcs = self.funcs[im.name]
                k = bisect.bisect_right([f.start for f in funcs], t) - 1
                if k >= 0 and funcs[k].start == t:
                    return "%s()" % im.fname(t)
        return "dispatch_call(0x%08Xu)" % t

    def state_sites(self, f):
        """Return sites of the function's calls (a C call that pushes the shadow stack): save states rebuild the
        guest call chain by resuming each frame there (NOTES.md "Save states")."""
        img = f.img
        out = set()
        for a in f.reach:
            if (a + 8) not in f.reach:
                continue
            i = img.insn(a)
            if i.kind == "jal":
                t = i.jump_target()
            elif i.kind == "branch" and i.name.endswith("al"):
                t = i.branch_target()
            elif i.kind == "jalr":
                t = None
            else:
                continue
            if t is not None and f.asm and f.start <= t < f.end:
                continue  # a local call inside a hand-written asm block: no C call
            d = img.insn(a + 4)
            if d is not None and d.kind in ("branch", "j", "jal", "jr", "jalr"):
                continue
            out.add(a + 8)
        return out

    def emit_func(self, f, out):
        img = f.img
        resume = sorted(getattr(f, "resume", ()))
        f.state_sites = self.state_sites(f)
        f.labels |= f.state_sites
        if resume:
            out.append("static void %s_impl(uint32_t entry) {" % img.fname(f.start))
        else:
            out.append("void %s(void) {" % img.fname(f.start))
        out.append("    uint32_t tgt; int cond; (void)tgt; (void)cond;")
        # save states: the driver (runtime/state.c) enters a frame at its function start (after the hook point) or
        # at the return site of the call it was in
        out.append("    if (__builtin_expect(state_resume_ra != 0, 0)) {")
        out.append("        uint32_t e_ = state_resume_ra; state_resume_ra = 0;")
        out.append("        switch (e_) {")
        out.append("        case 0x%08Xu: goto L_entry_;" % f.start)
        for r in sorted(f.state_sites):
            out.append("        case 0x%08Xu: goto L_%08X;" % (r, r))
        out.append("        }")
        out.append("        state_resume_bad(e_); return;")
        out.append("    }")
        if resume:
            out.append("    switch (entry) {")
            for r in resume:
                out.append("    case 0x%08Xu: goto L_%08X;" % (r, r))
            out.append("    }")
        out.append("    CHECK();")
        out.append("    HOOK_POINT(%d, 0x%08Xu);" % (f.index, f.start))
        out.append("L_entry_:;")
        addrs = sorted(f.reach)
        reach = f.reach
        # block starts for cycle counting
        starts = set(f.labels) | {f.start}
        for a in addrs:
            i = img.insn(a)
            if i.kind in ("branch", "j", "jal", "jr", "jalr"):
                starts.add(a + 8)
        for a in addrs:
            i = img.insn(a)
            if i.kind in ("branch", "j", "jal", "jr", "jalr") and (a + 4) in f.labels:
                f.labels.add(a + 8)
        consumed_delay = set()
        need_after = set()  # labels needed for pc+8 after a branch whose delay slot is a label
        body = []
        prev = None
        for a in addrs:
            if a in consumed_delay and a not in f.labels:
                continue
            if prev is not None and a != prev + 4 and a not in consumed_delay:
                pass  # gap: previous instruction ended in an unconditional transfer
            if a in f.labels:
                body.append("L_%08X:;" % a)
            if a in starts or a in f.labels:
                n = 0
                b = a
                while b in reach:
                    ib = img.insn(b)
                    n += insn_cycles(ib)
                    if ib.kind in ("branch", "j", "jal", "jr", "jalr"):
                        n += insn_cycles(img.insn(b + 4))
                        break
                    b += 4
                    if b in starts:
                        break
                body.append("    CYC(%d);" % n)
            i = img.insn(a)
            if a in consumed_delay:
                # a branch jumps into this delay slot: execute it, then continue at a+4
                body.append("    " + translate(i))
                prev = a
                continue
            k = i.kind
            if k not in ("branch", "j", "jal", "jr", "jalr"):
                body.append("    " + translate(i) + "  /* %08X */" % a)
                prev = a
                continue
            d = img.insn(a + 4)
            if d.kind in ("branch", "j", "jal", "jr", "jalr"):
                body.append("    /* branch in delay slot at %08X */ bad_insn(0x%08Xu, 0x%08Xu);" % (a + 4, a + 4, d.word))
                dcode = ""
            else:
                dcode = translate(d)
            consumed_delay.add(a + 4)
            pc8 = a + 8
            if k == "branch":
                t = i.branch_target()
                link = i.name.endswith("al")
                body.append("    cond = %s;  /* %08X %s */" % (branch_cond(i), a, i.name))
                if link:
                    body.append("    R31 = 0x%08Xu;" % pc8)
                body.append("    " + dcode)
                if link and f.asm and f.start <= t < f.end:
                    body.append("    if (cond) goto L_%08X;" % t)
                elif link:
                    body.append("    if (cond) { CALL(0x%08Xu, %s); }" % (pc8, self.callee(img, t)))
                elif f.start <= t < f.end:
                    if t <= a:
                        body.append("    if (cond) { CHECK(); goto L_%08X; }" % t)
                    else:
                        body.append("    if (cond) goto L_%08X;" % t)
                else:
                    body.append("    if (cond) { %s; return; }" % self.callee(img, t))
            elif k == "j":
                t = i.jump_target()
                body.append("    " + dcode + "  /* %08X j */" % a)
                if f.start <= t < f.end:
                    if t <= a:
                        body.append("    CHECK();")
                    body.append("    goto L_%08X;" % t)
                else:
                    body.append("    %s; return;" % self.callee(img, t))
            elif k == "jal":
                t = i.jump_target()
                body.append("    " + dcode + "  /* %08X jal */" % a)
                if f.asm and f.start <= t < f.end:
                    body.append("    R31 = 0x%08Xu; %sgoto L_%08X;  /* local call */" % (pc8, "CHECK(); " if t <= a else "", t))
                else:
                    body.append("    CALL(0x%08Xu, %s);" % (pc8, self.callee(img, t)))
            elif k == "jalr":
                body.append("    tgt = %s;  /* %08X jalr */" % (R(i.rs), a))
                body.append("    " + dcode)
                if i.rd and i.rd != 31:
                    body.append("    R%d = 0x%08Xu;" % (i.rd, pc8))
                body.append("    CALL(0x%08Xu, dispatch_call(tgt));" % pc8)
            elif k == "jr":
                if i.rs == 31:
                    body.append("    " + dcode + "  /* %08X jr ra */" % a)
                    if f.asm and f.local_rets:
                        body.append("    switch (R31) {")
                        for r in sorted(f.local_rets):
                            body.append("    case 0x%08Xu: goto L_%08X;" % (r, r))
                        body.append("    default: RET();")
                        body.append("    }")
                    else:
                        body.append("    RET();")
                else:
                    body.append("    tgt = %s;  /* %08X jr */" % (R(i.rs), a))
                    body.append("    " + dcode)
                    body.append("    switch (tgt) {")
                    for t in sorted(f.jt_labels):
                        if t in reach:
                            body.append("    case 0x%08Xu: goto L_%08X;" % (t, t))
                    body.append("    default: JUMP_REG(tgt); return;")
                    body.append("    }")
            if (a + 4) in f.labels:
                # delay slot is also a branch target: emitted again below; skip it on this path
                body.append("    goto L_%08X;" % pc8)
                need_after.add(pc8)
            prev = a
        # labels for pc+8 that were never emitted (target outside reach) -> emit stub
        text = "\n".join(body)
        for t in need_after:
            if ("L_%08X:;" % t) not in text:
                text += "\nL_%08X:; dispatch_call(0x%08Xu); return;" % (t, t)
        out.append(text)
        if f.falls_through:
            out.append("    %s; return;  /* falls into the next function */" % self.callee(img, f.end))
        out.append("    return;")
        out.append("}")
        out.append("")
        if resume:
            out.append("void %s(void) { %s_impl(0); }" % (img.fname(f.start), img.fname(f.start)))
            for r in resume:
                out.append("void %s_r%08X(void) { %s_impl(0x%08Xu); }" % (img.fname(f.start), r, img.fname(f.start), r))
            out.append("")

    def emit(self, outdir):
        outdir = Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        allf = []
        for img in self.images:
            allf += self.funcs[img.name]
        for n, f in enumerate(allf):
            f.index = n
        hdr = ["/* generated by tools/recomp/recomp.py - game code, do not commit */",
               "#pragma once", '#include "recomp.h"']
        for f in allf:
            hdr.append("void %s(void);" % f.img.fname(f.start))
            for r in sorted(getattr(f, "resume", ())):
                hdr.append("void %s_r%08X(void);" % (f.img.fname(f.start), r))
        (outdir / "funcs.h").write_text("\n".join(hdr) + "\n")
        files = []
        for k in range(0, len(allf), FUNCS_PER_FILE):
            out = ['/* generated - game code, do not commit */', '#include "funcs.h"', ""]
            for f in allf[k:k + FUNCS_PER_FILE]:
                self.emit_func(f, out)
            name = "code_%03d.c" % (k // FUNCS_PER_FILE)
            (outdir / name).write_text("\n".join(out))
            files.append(name)
        # dispatch tables: one per image
        d = ['/* generated - game code, do not commit */', '#include "funcs.h"', ""]
        for img in self.images:
            d.append("static const FuncEntry tab_%s[] = {" % img.name)
            ents = []
            for f in self.funcs[img.name]:
                ents.append((f.start, f.end, img.fname(f.start)))
                for r in sorted(getattr(f, "resume", ())):
                    ents.append((r, r + 4, "%s_r%08X" % (img.fname(f.start), r)))
            ents.sort()
            for a, e, n in ents:
                d.append("    {0x%08Xu, 0x%08Xu, %s}," % (a, e, n))
            d.append("};")
        d.append("const ImageEntry recomp_images[] = {")
        for img in self.images:
            ident = img.word(img.base) if img.prefix != "f" else 0
            d.append('    {"%s", 0x%08Xu, 0x%08Xu, 0x%08Xu, %d, 0x%08Xu, tab_%s, %d},' % (
                img.name, img.base, img.code_lo, img.code_hi, 0 if img.prefix == "f" else 1, ident or 0,
                img.name, len(self.funcs[img.name]) + sum(len(getattr(f, "resume", ())) for f in self.funcs[img.name])))
        d.append("};")
        d.append("const int recomp_image_count = %d;" % len(self.images))
        d.append("const int recomp_func_count = %d;" % len(allf))
        d.append("uint8_t fn_flags[%d];" % len(allf))
        d.append("const uint32_t fn_addr[%d] = {%s};" % (len(allf), ",".join("0x%08Xu" % f.start for f in allf)))
        d.append("const char *const fn_image[%d] = {%s};" % (len(allf), ",".join('"%s"' % f.img.name for f in allf)))
        d.append("void (*const fn_ptr[%d])(void) = {%s};" % (len(allf), ",".join(f.img.fname(f.start) for f in allf)))
        h = 1469598103934665603
        for b in self.images[0].data:
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        d.append("const uint64_t recomp_exe_hash = 0x%016XULL;" % h)
        # save states: every place a frame can be resumed (function starts and call return sites), per image,
        # sorted by address: {address, function index}
        gid = 1469598103934665603
        for k, img in enumerate(self.images):
            ents = []
            for f in self.funcs[img.name]:
                ents.append((f.start, f.index))
                ents += [(r, f.index) for r in getattr(f, "state_sites", ())]
            ents.sort()
            d.append("static const StateSite sites_%s[] = {" % img.name)
            for a, n in ents:
                d.append("    {0x%08Xu, %d}," % (a, n))
                gid = ((gid ^ a ^ (k << 32)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            d.append("};")
        d.append("const StateSite *const state_sites[] = {%s};" % ", ".join("sites_%s" % img.name for img in self.images))
        d.append("const int state_site_count[] = {%s};" % ", ".join(
            str(len(self.funcs[img.name]) + sum(len(getattr(f, "state_sites", ())) for f in self.funcs[img.name]))
            for img in self.images))
        d.append("const uint64_t recomp_gen_id = 0x%016XULL;" % gid)
        (outdir / "dispatch_table.c").write_text("\n".join(d) + "\n")
        files.append("dispatch_table.c")
        (outdir / "files.txt").write_text("\n".join(files) + "\n")
        print("wrote %d functions in %d files to %s" % (len(allf), len(files), outdir))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config")
    ap.add_argument("--disc", required=True, action="append",
                    help=".cue of the user's disc; repeat for discs 2, 3, ... (overlays with \"disc\": n)")
    ap.add_argument("--out", default="gen")
    args = ap.parse_args()
    cfg = json.loads(Path(args.config).read_text())
    discs = [Disc(d) for d in args.disc]
    disc = discs[0]
    exe = disc.file(cfg["exe"])
    assert exe[:8] == b"PS-X EXE"
    pc0, _, t_addr, t_size = struct.unpack_from("<4I", exe, 0x10)
    main_img = Image("main", t_addr, exe[0x800:0x800 + t_size], "f")
    main_img.entry = pc0
    lo, hi = find_code_range(main_img, int(cfg["code_start"], 16) if "code_start" in cfg else None)
    main_img.code_lo, main_img.code_hi = lo, hi
    images = [main_img]
    jal, consts, dataptr = scan_seeds(main_img, [])
    main_refs = jal | consts | dataptr  # the main exe's pointers (overlay slots among them)
    from mips import PATCHABLE
    PATCHABLE.update(int(x, 16) for x in cfg.get("patchable_imm", []))
    for ov in cfg.get("overlays", []):
        n = ov.get("disc", 1)
        if n > len(discs):
            print("skipping overlay %s: disc %d not given" % (ov["name"], n))
            continue
        data = discs[n - 1].file(ov["file"])
        img = Image(ov["name"], int(ov["base"], 16), data, "o_" + ov["name"])
        img.entry = None
        img.code_lo, img.code_hi = find_code_range(img)
        lo = overlay_code_start(img, main_refs)
        if lo != img.code_lo:
            print("overlay %s: code from %08X (first prologue %08X)" % (img.name, lo, img.code_lo))
            img.first_prologue = img.code_lo  # stays a function start (SWAP's 800AF26C is reached only from there)
            img.code_lo = lo
        images.append(img)
    extra = {int(x, 16) for x in cfg.get("extra_seeds", [])}
    asm = [(int(a, 16), int(b, 16)) for a, b in cfg.get("asm_ranges", [])]
    prog = Program(images, extra, asm)
    prog.analyse()
    prog.emit(args.out)
    # build info for the runtime: which exe this code was made from
    import hashlib
    info = {"exe_sha1": hashlib.sha1(exe).hexdigest(), "entry": "%08X" % pc0}
    (Path(args.out) / "build_info.json").write_text(json.dumps(info, indent=1))


if __name__ == "__main__":
    main()
