import bisect
import collections
import os
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse, symbols, INDIRECT_SYMBOL_LOCAL, INDIRECT_SYMBOL_ABS  # noqa: E402

def uleb(buf, i):
    v = s = 0
    while True:
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            return v, i

def sleb(buf, i):
    v = s = 0
    while True:
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            if b & 0x40:
                v -= 1 << s
            return v, i

def run_binds(buf, segs, lazy):
    i = 0
    ordinal, name, weak, addend, addr = 0, "", False, 0, 0
    while i < len(buf):
        b = buf[i]
        i += 1
        op, imm = b & 0xF0, b & 0x0F
        if op == 0x00:
            if not lazy:
                return
        elif op == 0x10:
            ordinal = imm
        elif op == 0x20:
            ordinal, i = uleb(buf, i)
        elif op == 0x30:
            ordinal = (imm | 0xF0) - 0x100 if imm else 0
        elif op == 0x40:
            end = buf.index(b"\0", i)
            name = buf[i:end].decode("latin-1")
            weak = bool(imm & 1)
            i = end + 1
        elif op == 0x50:
            if imm != 1:
                raise SystemExit("bind type %d not supported" % imm)
        elif op == 0x60:
            addend, i = sleb(buf, i)
        elif op == 0x70:
            off, i = uleb(buf, i)
            addr = segs[imm]["vmaddr"] + off
        elif op == 0x80:
            d, i = uleb(buf, i)
            addr = (addr + d) & 0xFFFFFFFF
        elif op == 0x90:
            yield addr, name, ordinal, addend, weak
            addr += 4
        elif op == 0xA0:
            yield addr, name, ordinal, addend, weak
            d, i = uleb(buf, i)
            addr = (addr + 4 + d) & 0xFFFFFFFF
        elif op == 0xB0:
            yield addr, name, ordinal, addend, weak
            addr += 4 + imm * 4
        elif op == 0xC0:
            n, i = uleb(buf, i)
            skip, i = uleb(buf, i)
            for _ in range(n):
                yield addr, name, ordinal, addend, weak
                addr += 4 + skip
        else:
            raise SystemExit("bad bind opcode 0x%02x" % b)

class Image:
    def __init__(self, data, info):
        self.data, self.info = data, info
        self.segs = info["segments"]
        self.sects = {(seg["name"], s["name"]): s for seg in self.segs for s in seg["sections"]}
        self.lo = self.segs[1]["vmaddr"]
        self.hi = max(s["vmaddr"] + s["vmsize"] for s in self.segs if s["name"] != "__LINKEDIT")

    def sect(self, seg, name):
        return self.sects.get((seg, name))

    def bytes_of(self, s):
        if s["flags"] & 0xFF == 1:
            return b""
        return self.data[s["offset"]:s["offset"] + s["size"]]

    def u32(self, addr):
        for seg in self.segs:
            if seg["vmaddr"] <= addr < seg["vmaddr"] + seg["filesize"]:
                return struct.unpack_from("<I", self.data, seg["fileoff"] + addr - seg["vmaddr"])[0]
        return 0

    def section_at(self, addr):
        for (segname, name), s in self.sects.items():
            if s["addr"] <= addr < s["addr"] + s["size"]:
                return segname, name
        return None

def main():
    path, out = sys.argv[1], sys.argv[2]
    report = open(sys.argv[sys.argv.index("--report") + 1], "w") if "--report" in sys.argv else None

    def rep(*a):
        if report:
            print(*a, file=report)

    with open(path, "rb") as f:
        data, info = parse(f.read())
    img = Image(data, info)
    syms = symbols(data, info)
    defined = {}
    sym_addrs = set()
    for s in syms:
        if s["value"] and (s["type"] & 0x0E) == 0x0E:
            sym_addrs.add(s["value"])
            if s["type"] & 0x01:
                defined.setdefault(s["name"], s["value"])

    text = img.sect("__TEXT", "__text")
    coal = img.sect("__TEXT", "__const_coal")
    stubs = img.sect("__TEXT", "__symbol_stub")
    cstr = img.sect("__TEXT", "__cstring")
    ustr = img.sect("__TEXT", "__ustring")

    dy = info["dyld_info"]
    binds = []
    for off, size, lazy in ((dy["bind_off"], dy["bind_size"], False),
                            (dy["lazy_off"], dy["lazy_size"], True)):
        binds += [(a, n, o, ad, w, lazy) for a, n, o, ad, w in
                  run_binds(data[off:off + size], img.segs, lazy)]
    bound_slots = {b[0] for b in binds}
    sets = []
    ext_binds = []
    for slot, name, ordinal, addend, weak, lazy in binds:
        if ordinal == 0 or ordinal == -1:
            if name not in defined:
                raise SystemExit("self bind to undefined %s" % name)
            sets.append((slot, defined[name] + addend))
        else:
            ext_binds.append((slot, name, ordinal, addend, weak, lazy))
    rep("binds: %d external, %d self-resolved" % (len(ext_binds), len(sets)))

    weak_where = collections.Counter()
    for slot, name, _ordinal, addend, _weak in run_binds(
            data[dy["weak_off"]:dy["weak_off"] + dy["weak_size"]], img.segs, False):
        if name in defined:
            sets.append((slot, defined[name] + addend))
            weak_where[(img.section_at(slot) or ("?", "?"))[1]] += 1
        elif slot not in bound_slots:
            raise SystemExit("weak bind of undefined %s at 0x%x is not bound" % (name, slot))
    rep("weak binds resolved to the game's own definitions:", dict(weak_where))
    if len({s for s, _ in sets}) != len(sets):
        raise SystemExit("a slot is resolved twice")

    code = img.bytes_of(text)
    funcs = set(a for a in sym_addrs if text["addr"] <= a < text["addr"] + text["size"])
    for i in range(0, len(code) - 3, 16):
        if code[i:i + 3] == b"\x55\x89\xe5":
            funcs.add(text["addr"] + i)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    insns = []
    called = set()
    boundaries = set()
    framed = set()
    enders = {"ret", "jmp", "hlt", "nop", "int3", "ud2"}
    for blob_sect in (text, coal):
        blob = img.bytes_of(blob_sect)
        prev = "ret"
        for ins in md.disasm(blob, blob_sect["addr"]):
            if ins.id == 0:
                prev = "nop"
                continue
            if (prev in enders and prev != "jmp" and ins.mnemonic not in ("nop", "int3")) or \
                    ins.bytes[:3] == b"\x55\x89\xe5":
                funcs.add(ins.address)
                if prev in enders and ins.bytes[:3] == b"\x55\x89\xe5":
                    framed.add(ins.address)
            if ins.id == x86.X86_INS_CALL and ins.bytes[0] == 0xE8:
                t = ins.operands[0].imm
                if t != ins.address + 5:
                    called.add(t)
            prev = ins.mnemonic
            boundaries.add(ins.address)
            insns.append(ins)
    funcs |= called
    rep("function starts: %d (%d call targets), instructions: %d" % (len(funcs), len(called), len(insns)))

    cbytes = img.bytes_of(cstr)
    str_starts = {cstr["addr"]} | {cstr["addr"] + i + 1 for i, b in enumerate(cbytes[:-1]) if b == 0}

    def text_like(v):
        return text["addr"] <= v < text["addr"] + text["size"] or \
            (coal and coal["addr"] <= v < coal["addr"] + coal["size"])

    mask_ops = {"and", "or", "xor", "test", "shl", "shr", "sar", "rol", "ror", "imul", "bt"}

    data_syms = sorted((s["value"], s["name"]) for s in syms
                       if s["value"] and (s["type"] & 0x0E) == 0x0E and s["value"] >= img.sect("__DATA", "__data")["addr"])
    dsym_addr = [v for v, _ in data_syms]

    def in_vtable(a):
        i = bisect.bisect_right(dsym_addr, a) - 1
        return i >= 0 and data_syms[i][1].startswith(("__ZTV", "__ZTC"))

    def valid_target(v, ctx, mnem="", slot=0):
        if ctx == "data" and text_like(v) and in_vtable(slot):
            return (v in boundaries), "vtbl"
        if v < text["addr"] or v >= img.hi:
            return False, "range"
        sec = img.section_at(v)
        if sec is None:
            return False, "gap"
        if text_like(v):
            if ctx == "imm" and mnem not in ("mov", "push"):
                return False, "func-op"
            # 0x30000 0xea60
            if v & 0xFF == 0 and v not in sym_addrs and v not in called and v not in framed:
                return False, "round"
            return (v in funcs), "func"
        if sec[1] == "__symbol_stub":
            return (v - stubs["addr"]) % 6 == 0, "stub"
        if sec[1] in ("__cstring", "__ustring"):
            return True, "str"
        if sec[1] in ("__stub_helper", "__unwind_info"):
            return False, sec[1]
        if ctx == "abs" or v in sym_addrs:
            return True, "sym"
        if bin(v).count("1") <= 1:
            return False, "pow2"
        if ctx == "imm":
            return mnem not in mask_ops, "mask"
        return v % 4 == 0, "data"

    relocs = set()
    why = collections.Counter()

    for j in range(stubs["size"] // 6):
        a = stubs["addr"] + 6 * j
        relocs.add(a + 2)
    why["stubs"] = stubs["size"] // 6

    dyst = info["dysymtab"]
    ind = struct.unpack_from("<%dI" % dyst["nindirectsyms"], data, dyst["indirectsymoff"])
    for seg in img.segs:
        for s in seg["sections"]:
            if s["flags"] & 0xFF in (6, 7):
                for j in range(s["size"] // 4):
                    a = s["addr"] + 4 * j
                    if ind[s["reserved1"] + j] & INDIRECT_SYMBOL_LOCAL and \
                            not ind[s["reserved1"] + j] & INDIRECT_SYMBOL_ABS and a not in bound_slots:
                        if img.lo <= img.u32(a) < img.hi:
                            relocs.add(a)
                            why["local sym_ptr"] += 1

    for name in ("__mod_init_func", "__program_vars"):
        s = img.sect("__DATA", name)
        for a in range(s["addr"], s["addr"] + s["size"], 4):
            if img.lo <= img.u32(a) < img.hi:
                relocs.add(a)
                why[name] += 1

    rejected = collections.Counter()
    scan = [s for seg in img.segs if seg["name"] in ("__DATA", "__OBJC") for s in seg["sections"]
            if s["flags"] & 0xFF not in (1, 6, 7) and s["name"] not in ("__mod_init_func", "__program_vars")]
    for s in scan:
        blob = img.bytes_of(s)
        n = 0
        for j in range(0, len(blob) - 3, 4):
            a = s["addr"] + j
            if a in bound_slots:
                continue
            v = struct.unpack_from("<I", blob, j)[0]
            if not (img.lo <= v < img.hi):
                continue
            ok, kind = valid_target(v, "data", slot=a)
            if ok:
                relocs.add(a)
                n += 1
            else:
                rejected[kind] += 1
                rep("  data reject %-18s 0x%06x = 0x%06x (%s)" % (s["name"], a, v, kind))
        why["data " + s["name"]] = n
    rep("data rejects by kind:", dict(rejected))

    jt = {}
    jt_unbounded = []
    for k, ins in enumerate(insns):
        if ins.id == x86.X86_INS_JMP and len(ins.operands) == 1:
            mop = ins.operands[0]
        elif ins.id == x86.X86_INS_MOV and len(ins.operands) == 2 and ins.operands[0].type == x86.X86_OP_REG:
            dst = ins.operands[0].reg
            if not any(n.id == x86.X86_INS_JMP and n.operands and n.operands[0].type == x86.X86_OP_REG
                       and n.operands[0].reg == dst for n in insns[k + 1:k + 3]):
                continue
            mop = ins.operands[1]
        else:
            continue
        if mop.type != x86.X86_OP_MEM or mop.mem.base != 0 or mop.mem.index == 0 or \
                mop.mem.scale != 4 or mop.mem.segment != 0:
            continue
        table, idx, count = mop.mem.disp & 0xFFFFFFFF, mop.mem.index, None
        for back in range(k - 1, max(k - 8, 0), -1):
            p, nx = insns[back], insns[back + 1]
            if p.id == x86.X86_INS_CMP and len(p.operands) == 2 and p.operands[0].type == x86.X86_OP_REG \
                    and p.operands[0].reg == idx and p.operands[1].type == x86.X86_OP_IMM:
                if nx.id == x86.X86_INS_JA:
                    count = p.operands[1].imm + 1
                elif nx.id == x86.X86_INS_JAE:
                    count = p.operands[1].imm
                break
        if count is None:
            jt_unbounded.append(table)
        elif 0 < count <= 4096:
            jt[table] = max(jt.get(table, 0), count)
    bounded = set(jt)
    for table in jt_unbounded:
        if table in jt:
            continue
        n = 0
        while n < 4096 and (table + 4 * n) not in jt and img.u32(table + 4 * n) in boundaries:
            n += 1
        if n:
            jt[table] = n
    jt_bad = jt_entries = 0
    for table, count in jt.items():
        for j in range(count):
            a = table + 4 * j
            v = img.u32(a)
            if text_like(v):
                relocs.add(a)
                jt_entries += 1
                if v not in boundaries:
                    jt_bad += 1
                    rep("  jump table 0x%06x[%d] = 0x%06x is not an instruction start" % (table, j, v))
            else:
                jt_bad += 1
                rep("  jump table 0x%06x[%d] = 0x%08x is not code" % (table, j, v))
    why["jump table entries"] = jt_entries
    rep("jump tables: %d (%d without a cmp bound), %d entries, %d suspicious" % (
        len(jt), len(set(jt) - bounded), jt_entries, jt_bad))
    jt_slots = {t + 4 * j for t, c in jt.items() for j in range(c)}

    tc = img.sect("__TEXT", "__const")
    blob = img.bytes_of(tc)
    tc_kinds = collections.Counter()
    for j in range(0, len(blob) - 3, 4):
        a = tc["addr"] + j
        v = struct.unpack_from("<I", blob, j)[0]
        if a in jt_slots or not (img.lo <= v < img.hi):
            continue
        ok, kind = valid_target(v, "data", slot=a)
        tc_kinds[("would " if ok else "rej ") + kind] += 1
        rep("  tconst %s %-6s 0x%06x = 0x%06x" % ("would" if ok else "rej  ", kind, a, v))
    rep("__TEXT,__const in-image words (NOT slid):", dict(tc_kinds))

    code_rej = collections.Counter()
    for ins in insns:
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == 0 and ins.disp_size == 4:
                v = op.mem.disp & 0xFFFFFFFF
                if img.lo <= v < img.hi:
                    ok, kind = valid_target(v, "abs")
                    if ok:
                        relocs.add(ins.address + ins.disp_offset)
                        why["code [abs]"] += 1
                    else:
                        code_rej["abs/" + kind] += 1
                    rep("  code abs %s %-8s 0x%06x %s %s" % ("OK " if ok else "rej", kind, ins.address, ins.mnemonic, ins.op_str))
            elif op.type == x86.X86_OP_IMM and ins.imm_size == 4 and \
                    ins.id not in (x86.X86_INS_CALL, x86.X86_INS_JMP) and not ins.mnemonic.startswith("j"):
                v = op.imm & 0xFFFFFFFF
                if img.lo <= v < img.hi:
                    ok, kind = valid_target(v, "imm", ins.mnemonic)
                    if ok:
                        relocs.add(ins.address + ins.imm_offset)
                        why["code imm"] += 1
                    else:
                        code_rej["imm/" + kind] += 1
                    rep("  code imm %s %-8s 0x%06x %s %s" % ("OK " if ok else "rej", kind, ins.address, ins.mnemonic, ins.op_str))
    rep("code rejects by kind:", dict(code_rej))

    relocs -= bound_slots
    relocs -= {s for s, _ in sets}
    for k, v in why.items():
        rep("reloc source %-28s %d" % (k, v))
    rep("total relocs: %d" % len(relocs))

    names = bytearray()
    name_off = {}
    for _, n, *_ in ext_binds:
        if n not in name_off:
            name_off[n] = len(names)
            names += n.encode("latin-1") + b"\0"
    with open(out, "wb") as f:
        f.write(b"GMVL" + struct.pack("<4I", 2, len(relocs), len(ext_binds), len(names)))
        f.write(struct.pack("<%dI" % len(relocs), *sorted(relocs)))
        for slot, n, ordinal, addend, weak, lazy in ext_binds:
            f.write(struct.pack("<IiHBBI", slot, addend, ordinal & 0xFFFF,
                                (1 if weak else 0) | (2 if lazy else 0), 0, name_off[n]))
        f.write(names)
        f.write(struct.pack("<I", len(sets)))
        for slot, v in sets:
            f.write(struct.pack("<II", slot, v))
    print("relocs %d, external binds %d (%d unique), self binds %d -> %s" % (
        len(relocs), len(ext_binds), len(name_off), len(sets), out))
    print("sources:", dict(why))
    print("data rejects:", dict(rejected), " code rejects:", dict(code_rej))
    print("jump tables: %d, %d entries, %d suspicious;  __TEXT,__const words: %s" % (
        len(jt), jt_entries, jt_bad, dict(tc_kinds)))

if __name__ == "__main__":
    main()
