"""MIPS R3000A (PS1) instruction decoder and C translation of single instructions.

Control-flow instructions (branches, jumps) are only classified here; recomp.py emits them because
they need the delay slot and the function layout.
"""

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class Insn:
    __slots__ = ("pc", "word", "op", "rs", "rt", "rd", "sa", "funct", "imm", "simm", "target",
                 "kind", "name")

    def __init__(self, pc, word):
        self.pc = pc
        self.word = word
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        self.imm = word & 0xFFFF
        self.simm = sx16(self.imm)
        self.target = None
        self.kind, self.name = classify(self)

    def branch_target(self):
        return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF

    def jump_target(self):
        return ((self.pc + 4) & 0xF0000000) | ((self.word & 0x03FFFFFF) << 2)


# kinds: "alu", "load", "store", "branch" (conditional), "j", "jal", "jr", "jalr", "sys", "cop", "bad"
SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr",
           12: "syscall", 13: "break", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo", 24: "mult",
           25: "multu", 26: "div", 27: "divu", 32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and",
           37: "or", 38: "xor", 39: "nor", 42: "slt", 43: "sltu"}
PRIMARY = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu", 10: "slti",
           11: "sltiu", 12: "andi", 13: "ori", 14: "xori", 15: "lui", 32: "lb", 33: "lh", 34: "lwl",
           35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 46: "swr",
           50: "lwc2", 58: "swc2"}


def classify(i):
    if i.op == 0:
        n = SPECIAL.get(i.funct)
        if n is None:
            return "bad", "special?"
        if n == "jr":
            return "jr", n
        if n == "jalr":
            return "jalr", n
        if n in ("syscall", "break"):
            return "sys", n
        return "alu", n
    if i.op == 1:
        link = (i.rt & 0x1E) == 0x10
        n = ("bgez" if i.rt & 1 else "bltz") + ("al" if link else "")
        return "branch", n
    if i.op == 16:
        if i.rs == 0:
            return "cop", "mfc0"
        if i.rs == 4:
            return "cop", "mtc0"
        if i.rs == 16 and i.funct == 16:
            return "cop", "rfe"
        return "bad", "cop0?"
    if i.op == 18:
        if i.rs & 0x10:
            return "cop", "cop2"
        n = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(i.rs)
        return ("cop", n) if n else ("bad", "cop2?")
    n = PRIMARY.get(i.op)
    if n is None:
        return "bad", "op%d" % i.op
    if n == "j":
        return "j", n
    if n == "jal":
        return "jal", n
    if n in ("beq", "bne", "blez", "bgtz"):
        return "branch", n
    if n in ("lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "lwc2"):
        return "load", n
    if n in ("sb", "sh", "swl", "sw", "swr", "swc2"):
        return "store", n
    return "alu", n


def R(n):
    return "0u" if n == 0 else "R%d" % n


def W(n, expr):
    """Assignment to GPR n; writes to $zero are dropped (expression kept only for side effects)."""
    if n == 0:
        return "(void)(%s);" % expr
    return "R%d = %s;" % (n, expr)


def ea(i):
    if i.simm == 0:
        return R(i.rs)
    return "%s + 0x%Xu" % (R(i.rs), i.simm & 0xFFFFFFFF)


def branch_cond(i):
    rs, rt = R(i.rs), R(i.rt)
    n = i.name
    if n == "beq":
        return "%s == %s" % (rs, rt)
    if n == "bne":
        return "%s != %s" % (rs, rt)
    if n == "blez":
        return "(int32_t)%s <= 0" % rs
    if n == "bgtz":
        return "(int32_t)%s > 0" % rs
    if n.startswith("bltz"):
        return "(int32_t)%s < 0" % rs
    if n.startswith("bgez"):
        return "(int32_t)%s >= 0" % rs
    raise ValueError(n)


# instructions whose 16-bit immediate is read from RAM at run time, so that cheat codes that patch
# the instruction (GameShark writes into the code) still take effect (config "patchable_imm")
PATCHABLE = set()


def translate(i):
    """C for a non-control-flow instruction."""
    n, rs, rt, rd = i.name, R(i.rs), R(i.rt), R(i.rd)
    imm, simm = i.imm, i.simm & 0xFFFFFFFF
    if i.pc in PATCHABLE and i.kind == "alu" and i.op != 0:
        live = "mem_r16(0x%08Xu)" % i.pc
        sx = "(uint32_t)(int32_t)(int16_t)" + live
        if n in ("addi", "addiu"):
            return W(i.rt, "%s + %s" % (rs, sx))
        if n == "slti":
            return W(i.rt, "(int32_t)%s < (int32_t)%s" % (rs, sx))
        if n == "sltiu":
            return W(i.rt, "%s < %s" % (rs, sx))
        if n == "andi":
            return W(i.rt, "%s & %s" % (rs, live))
        if n == "ori":
            return W(i.rt, "%s | %s" % (rs, live))
        if n == "xori":
            return W(i.rt, "%s ^ %s" % (rs, live))
        if n == "lui":
            return W(i.rt, "%s << 16" % live)
    if i.word == 0:
        return ""  # nop
    if i.kind == "alu":
        if n == "sll":
            return W(i.rd, "%s << %d" % (rt, i.sa))
        if n == "srl":
            return W(i.rd, "%s >> %d" % (rt, i.sa))
        if n == "sra":
            return W(i.rd, "(uint32_t)((int32_t)%s >> %d)" % (rt, i.sa))
        if n == "sllv":
            return W(i.rd, "%s << (%s & 31)" % (rt, rs))
        if n == "srlv":
            return W(i.rd, "%s >> (%s & 31)" % (rt, rs))
        if n == "srav":
            return W(i.rd, "(uint32_t)((int32_t)%s >> (%s & 31))" % (rt, rs))
        if n == "mfhi":
            return W(i.rd, "cpu.hi")
        if n == "mflo":
            return W(i.rd, "cpu.lo")
        if n == "mthi":
            return "cpu.hi = %s;" % rs
        if n == "mtlo":
            return "cpu.lo = %s;" % rs
        if n == "mult":
            return "op_mult(%s, %s);" % (rs, rt)
        if n == "multu":
            return "op_multu(%s, %s);" % (rs, rt)
        if n == "div":
            return "op_div(%s, %s);" % (rs, rt)
        if n == "divu":
            return "op_divu(%s, %s);" % (rs, rt)
        if n in ("add", "addu"):
            return W(i.rd, "%s + %s" % (rs, rt))
        if n in ("sub", "subu"):
            return W(i.rd, "%s - %s" % (rs, rt))
        if n == "and":
            return W(i.rd, "%s & %s" % (rs, rt))
        if n == "or":
            return W(i.rd, "%s | %s" % (rs, rt))
        if n == "xor":
            return W(i.rd, "%s ^ %s" % (rs, rt))
        if n == "nor":
            return W(i.rd, "~(%s | %s)" % (rs, rt))
        if n == "slt":
            return W(i.rd, "(int32_t)%s < (int32_t)%s" % (rs, rt))
        if n == "sltu":
            return W(i.rd, "%s < %s" % (rs, rt))
        if n in ("addi", "addiu"):
            return W(i.rt, "%s + 0x%Xu" % (rs, simm))
        if n == "slti":
            return W(i.rt, "(int32_t)%s < %d" % (rs, i.simm))
        if n == "sltiu":
            return W(i.rt, "%s < 0x%Xu" % (rs, simm))
        if n == "andi":
            return W(i.rt, "%s & 0x%Xu" % (rs, imm))
        if n == "ori":
            return W(i.rt, "%s | 0x%Xu" % (rs, imm))
        if n == "xori":
            return W(i.rt, "%s ^ 0x%Xu" % (rs, imm))
        if n == "lui":
            return W(i.rt, "0x%Xu" % (imm << 16))
    if i.kind == "load":
        a = ea(i)
        if n == "lb":
            return W(i.rt, "(uint32_t)(int32_t)(int8_t)mem_r8(%s)" % a)
        if n == "lbu":
            return W(i.rt, "mem_r8(%s)" % a)
        if n == "lh":
            return W(i.rt, "(uint32_t)(int32_t)(int16_t)mem_r16(%s)" % a)
        if n == "lhu":
            return W(i.rt, "mem_r16(%s)" % a)
        if n == "lw":
            return W(i.rt, "mem_r32(%s)" % a)
        if n == "lwl":
            return W(i.rt, "mem_lwl(%s, %s)" % (a, rt))
        if n == "lwr":
            return W(i.rt, "mem_lwr(%s, %s)" % (a, rt))
        if n == "lwc2":
            if 12 <= i.rt <= 15:  # a screen position back into the GTE: with its precise value (gte.c, pgxp_load)
                return "{ uint32_t a_ = %s; gte_write_data(%d, mem_r32(a_)); pgxp_load(a_, %d); }" % (a, i.rt, i.rt)
            return "gte_write_data(%d, mem_r32(%s));" % (i.rt, a)
    if i.kind == "store":
        a = ea(i)
        if n == "sb":
            return "mem_w8(%s, (uint8_t)%s);" % (a, rt)
        if n == "sh":
            return "mem_w16(%s, (uint16_t)%s);" % (a, rt)
        if n == "sw":
            return "mem_w32(%s, %s);" % (a, rt)
        if n == "swl":
            return "mem_swl(%s, %s);" % (a, rt)
        if n == "swr":
            return "mem_swr(%s, %s);" % (a, rt)
        if n == "swc2":
            if 12 <= i.rt <= 15:  # a screen position: its precise value goes with it (gte.c, pgxp_store)
                return "{ uint32_t a_ = %s; mem_w32(a_, gte_read_data(%d)); pgxp_store(a_, %d); }" % (a, i.rt, i.rt)
            return "mem_w32(%s, gte_read_data(%d));" % (a, i.rt)
    if i.kind == "cop":
        if n == "mfc0":
            return W(i.rt, "cop0_read(%d)" % i.rd)
        if n == "mtc0":
            return "cop0_write(%d, %s);" % (i.rd, rt)
        if n == "rfe":
            return "cop0_rfe();"
        if n == "mfc2":
            return W(i.rt, "gte_read_data(%d)" % i.rd)
        if n == "cfc2":
            return W(i.rt, "gte_read_ctrl(%d)" % i.rd)
        if n == "mtc2":
            return "gte_write_data(%d, %s);" % (i.rd, rt)
        if n == "ctc2":
            return "gte_write_ctrl(%d, %s);" % (i.rd, rt)
        if n == "cop2":
            return "gte_command(0x%07Xu);" % (i.word & 0x1FFFFFF)
    if i.kind == "sys":
        if n == "syscall":
            return "bios_syscall(0x%08Xu);" % i.pc
        return "bios_break(0x%08Xu, 0x%Xu);" % (i.pc, (i.word >> 6) & 0xFFFFF)
    return "bad_insn(0x%08Xu, 0x%08Xu);" % (i.pc, i.word)


def writes_reg(i):
    """GPR written by a non-control instruction (or None)."""
    if i.kind == "alu":
        if i.op == 0:
            return None if i.name in ("mthi", "mtlo", "mult", "multu", "div", "divu") else i.rd
        return i.rt
    if i.kind == "load" and i.name != "lwc2":
        return i.rt
    if i.kind == "cop" and i.name in ("mfc0", "mfc2", "cfc2"):
        return i.rt
    return None
