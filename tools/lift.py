#!/usr/bin/env python3
"""Static recompiler: x86-32 PE (MSVC) -> C.

Usage: lift.py program.exe outdir [extra_entries.txt]
"""
import sys, os, struct, collections
import pefile, capstone
from capstone import x86_const as X

import argparse
ap = argparse.ArgumentParser()
ap.add_argument('image'); ap.add_argument('out')
ap.add_argument('--name', default='main')
ap.add_argument('--base', type=lambda x: int(x, 16), default=None)
ap.add_argument('--extra', default=None)
ap.add_argument('--hooks', default=None)
ap.add_argument('--patches', default=None)
ap.add_argument('--nfiles', type=int, default=None)
args = ap.parse_args()
EXE, OUT, EXTRA, MOD = args.image, args.out, args.extra, args.name
HOOKS = set()      # hook runs after the instruction
PREHOOKS = set()   # '<addr': hook runs before the instruction (works for ret/jmp too)
if args.hooks and os.path.exists(args.hooks):
    for l in open(args.hooks):
        l = l.split('#')[0].strip()
        if l.startswith('<'): PREHOOKS.add(int(l[1:], 16))
        elif l: HOOKS.add(int(l, 16))

pe = pefile.PE(EXE)
if args.base is not None and args.base != pe.OPTIONAL_HEADER.ImageBase:
    pe.relocate_image(args.base)
    pe.OPTIONAL_HEADER.ImageBase = args.base
IB = pe.OPTIONAL_HEADER.ImageBase
img = bytearray(pe.get_memory_mapped_image())
img += bytes(max(0, pe.OPTIONAL_HEADER.SizeOfImage - len(img)))
# byte patches (tools/patches.txt for the exe: --patches FILE)
if args.patches and os.path.exists(args.patches):
    for l in open(args.patches):
        l = l.split('#')[0].split()
        if len(l) == 2:
            a, b = int(l[0], 16), bytes.fromhex(l[1])
            img[a - IB:a - IB + len(b)] = b
            print('patch %x: %s' % (a, b.hex()), file=sys.stderr)
CODE = [(IB + s.VirtualAddress, IB + s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Characteristics & 0x20000000]
TLO = min(a for a, b in CODE); THI = max(b for a, b in CODE)

def in_text(a): return any(lo <= a < hi for lo, hi in CODE)
def rd32(a): return struct.unpack_from('<I', img, a - IB)[0]
def rd8(a): return img[a - IB]

iat = {}
for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for i in e.imports:
        iat[i.address] = (e.dll.decode().lower(), i.name.decode() if i.name else '#%d' % i.ordinal)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
_icache = {}
def dis(a):
    if a in _icache: return _icache[a]
    off = a - IB
    ins = next(md.disasm(bytes(img[off:off + 16]), a), None)
    _icache[a] = ins
    return ins

JCC = {X.X86_INS_JE, X.X86_INS_JNE, X.X86_INS_JA, X.X86_INS_JAE, X.X86_INS_JB, X.X86_INS_JBE,
       X.X86_INS_JG, X.X86_INS_JGE, X.X86_INS_JL, X.X86_INS_JLE, X.X86_INS_JS, X.X86_INS_JNS,
       X.X86_INS_JO, X.X86_INS_JNO, X.X86_INS_JP, X.X86_INS_JNP, X.X86_INS_JECXZ, X.X86_INS_JCXZ,
       X.X86_INS_LOOP, X.X86_INS_LOOPE, X.X86_INS_LOOPNE}

# ---------------------------------------------------------------- discovery
entries = set()
if pe.OPTIONAL_HEADER.AddressOfEntryPoint: entries.add(IB + pe.OPTIONAL_HEADER.AddressOfEntryPoint)
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if in_text(IB + sym.address): entries.add(IB + sym.address)
weak = set()
# code pointers in data sections
for s in pe.sections:
    if s.Name.startswith(b'.rsrc') or s.Name.startswith(b'.reloc'): continue
    base = IB + s.VirtualAddress
    d = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
    for o in range(0, len(d) - 3, 4):
        v = struct.unpack_from('<I', d, o)[0]
        if in_text(v): weak.add(v)
if EXTRA and os.path.exists(EXTRA):
    for l in open(EXTRA):
        l = l.split('#')[0].strip()
        if l: entries.add(int(l, 16))

def jump_table(ins, insmap_addr_list):
    """ins is jmp [reg*4+disp]; return list of targets"""
    op = ins.operands[0]
    m = op.mem
    if op.type != X.X86_OP_MEM: return None
    base_only = m.base != 0 and m.index == 0
    if not (base_only or (m.base == 0 and m.index != 0 and m.scale == 4)): return None
    tbl = m.disp & 0xffffffff
    if not (IB <= tbl < IB + len(img)): return None
    # bound: look back for cmp idx, imm ; ja
    bound = None
    if base_only:
        out = []
        tbl = m.disp & 0xffffffff
        for k in range(64):
            a = tbl + 4 * k
            if a - IB + 4 > len(img) or not in_text(rd32(a)): break
            out.append(rd32(a))
        return out or None
    idxreg = m.index
    # MSVC memcpy/memmove tails: 'neg ecx; jmp [ecx*4 + tbl]' indexes backwards
    if len(insmap_addr_list) > 1 and dis(insmap_addr_list[-2]).id == X.X86_INS_NEG:
        out = []
        for k in range(0, 33):
            a = tbl - 4 * k
            if a < IB or not in_text(rd32(a)): break
            out.append(rd32(a))
        return out or None
    for pa in reversed(insmap_addr_list[-6:]):
        p = dis(pa)
        if p.id == X.X86_INS_CMP and p.operands[0].type == X.X86_OP_REG and p.operands[1].type == X.X86_OP_IMM:
            bound = p.operands[1].imm + 1
            break
    # byte-index table pattern: movzx/mov idxreg8, byte [r+tbl2]
    for pa in reversed(insmap_addr_list[-4:]):
        p = dis(pa)
        if p.id in (X.X86_INS_MOV, X.X86_INS_MOVZX) and p.operands[1].type == X.X86_OP_MEM and p.operands[1].size == 1 and p.operands[1].mem.disp:
            t2 = p.operands[1].mem.disp & 0xffffffff
            if bound is not None and IB <= t2 and t2 + bound - IB <= len(img):
                bound = max(rd8(t2 + k) for k in range(bound)) + 1
            break
    out = []
    k = 0
    while True:
        if bound is not None and k >= bound: break
        if bound is None and k > 512: break
        a = tbl + 4 * k
        if a - IB + 4 > len(img): break
        v = rd32(a)
        if not in_text(v): break
        out.append(v); k += 1
    return out

funcs = {}   # entry -> dict(addr->ins), jt: addr->targets

def explore(entry):
    insns = {}
    jts = {}
    work = [entry]
    order = []
    bad = False
    while work:
        a = work.pop()
        while True:
            if a in insns: break
            if not in_text(a): bad = True; break
            ins = dis(a)
            if ins is None: insns[a] = None; bad = True; break
            insns[a] = ins
            order.append(a)
            iid = ins.id
            nxt = a + ins.size
            if iid in JCC:
                work.append(ins.operands[0].imm & 0xffffffff)
                a = nxt; continue
            if iid == X.X86_INS_JMP:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    a = op.imm & 0xffffffff; continue
                t = jump_table(ins, order)
                if t:
                    jts[a] = t
                    work.extend(t)
                break
            if iid in (X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_INT3, X.X86_INS_HLT, X.X86_INS_UD2):
                break
            if iid == X.X86_INS_CALL:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if in_text(t): pending.add(t)
                elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    ia = op.mem.disp & 0xffffffff
                    if ia in iat and iat[ia][1] in ('ExitProcess', 'TerminateProcess'):
                        break
            # immediates pointing to code
            for op in ins.operands:
                if op.type == X.X86_OP_IMM and in_text(op.imm & 0xffffffff) and iid != X.X86_INS_CALL:
                    pending.add(op.imm & 0xffffffff)
            a = nxt
    return insns, jts, bad

pending = set(entries)
weak_try = set(weak)
done = set()
while pending or weak_try:
    if pending:
        e = pending.pop(); isweak = False
    else:
        e = weak_try.pop(); isweak = True
    if e in done: continue
    done.add(e)
    saved = set(pending)
    insns, jts, bad = explore(e)
    if isweak and bad:
        # validate: weak entries must decode cleanly
        pending = saved
        continue
    funcs[e] = (insns, jts)

def compute_cov():
    cov = bytearray(THI - TLO)
    for e, (insns, jts) in funcs.items():
        for a, i in insns.items():
            if i is not None: cov[a - TLO:a - TLO + i.size] = b'\x01' * i.size
    return cov
PAD = (0xcc, 0x90, 0x00)
for rnd in range(8):
    cov = compute_cov()
    cand = set()
    for lo, hi in CODE:
        a = lo
        while a < hi:
            if cov[a - TLO]: a += 1; continue
            b = a
            while b < hi and not cov[b - TLO]: b += 1
            # skip leading padding; then a candidate function start
            p = a
            while p < b and img[p - IB] in PAD: p += 1
            if b - p >= 3 and p not in done: cand.add(p)
            a = b
    if not cand: break
    weak_try = cand
    while pending or weak_try:
        if pending: e = pending.pop(); isweak = False
        else: e = weak_try.pop(); isweak = True
        if e in done: continue
        done.add(e)
        saved = set(pending)
        insns, jts, bad = explore(e)
        if isweak and bad:
            pending = saved; continue
        funcs[e] = (insns, jts)
    print('gap round', rnd, 'candidates', len(cand), 'functions', len(funcs), file=sys.stderr)

print('functions:', len(funcs), file=sys.stderr)

# ---------------------------------------------------------------- emission
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REGS = {}
for r in R32:
    REGS[r] = (r, 0, 4)
for r16, r in zip(['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'], R32):
    REGS[r16] = (r, 0, 2)
for r8, r in zip(['al', 'cl', 'dl', 'bl'], R32[:4]):
    REGS[r8] = (r, 0, 1)
for r8, r in zip(['ah', 'ch', 'dh', 'bh'], R32[:4]):
    REGS[r8] = (r, 8, 1)
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SBITS = {1: 8, 2: 16, 4: 32}
STY = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}
UTY = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}

class Unsupported(Exception): pass

def rname(ins, reg): return ins.reg_name(reg)

SEGV = {'cs': '0x1bu', 'ds': '0x23u', 'es': '0x23u', 'ss': '0x23u', 'fs': '0x3bu', 'gs': '0u'}
def reg_get(name):
    if name in SEGV: return SEGV[name]
    if name not in REGS: raise Unsupported('reg ' + name)
    r, sh, sz = REGS[name]
    if sz == 4: return r
    if sh: return '((%s >> 8) & 0xffu)' % r
    return '(%s & %s)' % (r, MASK[sz])

def reg_set(name, v):
    if name in SEGV: return '(void)(%s);' % v
    if name not in REGS: raise Unsupported('reg ' + name)
    r, sh, sz = REGS[name]
    if sz == 4: return '%s = (uint32_t)(%s);' % (r, v)
    if sh: return '%s = (%s & 0xffff00ffu) | (((uint32_t)(%s) & 0xffu) << 8);' % (r, r, v)
    return '%s = (%s & ~%s) | ((uint32_t)(%s) & %s);' % (r, r, MASK[sz], v, MASK[sz])

def ea(ins, op):
    m = op.mem
    parts = []
    if m.base: parts.append(reg_get(rname(ins, m.base)))
    if m.index:
        ix = reg_get(rname(ins, m.index))
        parts.append(ix if m.scale == 1 else '%s*%d' % (ix, m.scale))
    if m.disp or not parts: parts.append('0x%xu' % (m.disp & 0xffffffff))
    e = ' + '.join(parts)
    if m.segment == X.X86_REG_FS:
        e = 'c->fs_base + ' + e
    elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES, X.X86_REG_CS):
        raise Unsupported('segment')
    return '(uint32_t)(%s)' % e

def mrd(addr, sz):
    if sz not in (1, 2, 4, 8): raise Unsupported('mem size %d' % sz)
    return 'R%d(%s)' % (sz * 8, addr)
def mwr(addr, sz, v): return 'W%d(%s, %s);' % (sz * 8, addr, v)

def get(ins, op, sz=None):
    sz = sz or op.size
    if op.type == X.X86_OP_REG: return reg_get(rname(ins, op.reg))
    if op.type == X.X86_OP_IMM: return '0x%xu' % (op.imm & ((1 << (sz * 8)) - 1))
    if op.type == X.X86_OP_MEM: return mrd(ea(ins, op), sz)
    raise Unsupported('op')

def put(ins, op, v, sz=None):
    sz = sz or op.size
    if op.type == X.X86_OP_REG: return reg_set(rname(ins, op.reg), v)
    if op.type == X.X86_OP_MEM: return mwr(ea(ins, op), sz, v)
    raise Unsupported('put')

def szf(sz, r='r'):
    return 'zf = (%s) == 0; sf = ((%s) >> %d) & 1; pf = PARITY(%s);' % (r, r, SBITS[sz] - 1, r)

# condition expressions from flags
COND = {
    X.X86_INS_JE: 'zf', X.X86_INS_JNE: '!zf', X.X86_INS_JA: '(!cf && !zf)', X.X86_INS_JAE: '!cf',
    X.X86_INS_JB: 'cf', X.X86_INS_JBE: '(cf || zf)', X.X86_INS_JG: '(!zf && sf == of)',
    X.X86_INS_JGE: '(sf == of)', X.X86_INS_JL: '(sf != of)', X.X86_INS_JLE: '(zf || sf != of)',
    X.X86_INS_JS: 'sf', X.X86_INS_JNS: '!sf', X.X86_INS_JO: 'of', X.X86_INS_JNO: '!of',
    X.X86_INS_JP: 'pf', X.X86_INS_JNP: '!pf',
}
CC_NAMES = {'e': X.X86_INS_JE, 'ne': X.X86_INS_JNE, 'a': X.X86_INS_JA, 'ae': X.X86_INS_JAE, 'b': X.X86_INS_JB,
            'be': X.X86_INS_JBE, 'g': X.X86_INS_JG, 'ge': X.X86_INS_JGE, 'l': X.X86_INS_JL, 'le': X.X86_INS_JLE,
            's': X.X86_INS_JS, 'ns': X.X86_INS_JNS, 'o': X.X86_INS_JO, 'no': X.X86_INS_JNO, 'p': X.X86_INS_JP,
            'np': X.X86_INS_JNP}

def fused(cid, last):
    """direct comparison if last flag op was cmp/sub or test/logic"""
    if not last: return None
    kind, sz = last
    s, u = STY[sz], UTY[sz]
    if kind == 'sub':
        m = {X.X86_INS_JE: 'fa == fb', X.X86_INS_JNE: 'fa != fb', X.X86_INS_JA: 'fa > fb',
             X.X86_INS_JAE: 'fa >= fb', X.X86_INS_JB: 'fa < fb', X.X86_INS_JBE: 'fa <= fb',
             X.X86_INS_JG: '(%s)fa > (%s)fb' % (s, s), X.X86_INS_JGE: '(%s)fa >= (%s)fb' % (s, s),
             X.X86_INS_JL: '(%s)fa < (%s)fb' % (s, s), X.X86_INS_JLE: '(%s)fa <= (%s)fb' % (s, s)}
        return m.get(cid)
    if kind == 'logic':
        m = {X.X86_INS_JE: 'fr == 0', X.X86_INS_JNE: 'fr != 0', X.X86_INS_JS: '(%s)fr < 0' % s,
             X.X86_INS_JNS: '(%s)fr >= 0' % s, X.X86_INS_JLE: '(%s)fr <= 0' % s, X.X86_INS_JG: '(%s)fr > 0' % s,
             X.X86_INS_JL: '(%s)fr < 0' % s, X.X86_INS_JGE: '(%s)fr >= 0' % s,
             X.X86_INS_JBE: 'fr == 0', X.X86_INS_JA: 'fr != 0', X.X86_INS_JB: '0', X.X86_INS_JAE: '1'}
        return m.get(cid)
    if kind == 'incdec':
        m = {X.X86_INS_JE: 'fr == 0', X.X86_INS_JNE: 'fr != 0', X.X86_INS_JS: '(%s)fr < 0' % s,
             X.X86_INS_JNS: '(%s)fr >= 0' % s}
        return m.get(cid)
    return None

def fn_name(a): return 'f_%08x' % a

unsupported = collections.Counter()

def cond_of(cid, last):
    f = fused(cid, last)
    return f if f else COND[cid]

def lift_fpu(ins, out):
    b = bytes(ins.bytes)
    # skip prefixes
    i = 0
    while b[i] in (0x66, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x9b): i += 1
    op0 = b[i]; modrm = b[i + 1]
    mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
    E = out.append
    memop = None
    for o in ins.operands:
        if o.type == X.X86_OP_MEM: memop = o
    A = ea(ins, memop) if memop is not None else None
    ARITH = ['+', '*', 'com', 'comp', '-', '-r', '/', '/r']
    def arith(kind, dst, src):
        if kind == '+': return '%s = %s + %s;' % (dst, dst, src)
        if kind == '*': return '%s = %s * %s;' % (dst, dst, src)
        if kind == '-': return '%s = %s - %s;' % (dst, dst, src)
        if kind == '-r': return '%s = %s - %s;' % (dst, src, dst)
        if kind == '/': return '%s = %s / %s;' % (dst, dst, src)
        if kind == '/r': return '%s = %s / %s;' % (dst, src, dst)
    if mod != 3:
        if op0 in (0xd8, 0xdc, 0xda, 0xde):
            src = {0xd8: 'RF32(%s)', 0xdc: 'RF64(%s)', 0xda: '(double)(int32_t)R32(%s)', 0xde: '(double)(int16_t)R16(%s)'}[op0] % A
            k = ARITH[reg]
            if k in ('com', 'comp'):
                E('FCOM(ST(0), %s);' % src)
                if k == 'comp': E('FPOP();')
            else:
                E(arith(k, 'ST(0)', src))
            return
        if op0 == 0xd9:
            if reg == 0: E('FPUSH(RF32(%s));' % A); return
            if reg == 2: E('WF32(%s, ST(0));' % A); return
            if reg == 3: E('WF32(%s, ST(0)); FPOP();' % A); return
            if reg == 5: E('c->fcw = R16(%s);' % A); return
            if reg == 7: E('W16(%s, c->fcw);' % A); return
            if reg == 6: E('fpu_fnstenv(c, %s);' % A); return
            if reg == 4: E('fpu_fldenv(c, %s);' % A); return
        if op0 == 0xdb:
            if reg == 0: E('FPUSH((double)(int32_t)R32(%s));' % A); return
            if reg == 2: E('W32(%s, fpu_toint32(c, ST(0)));' % A); return
            if reg == 3: E('W32(%s, fpu_toint32(c, ST(0))); FPOP();' % A); return
            if reg == 5: E('FPUSH(fpu_ld80(M + %s));' % A); return
            if reg == 7: E('fpu_st80(M + %s, ST(0)); FPOP();' % A); return
        if op0 == 0xdd:
            if reg == 0: E('FPUSH(RF64(%s));' % A); return
            if reg == 2: E('WF64(%s, ST(0));' % A); return
            if reg == 3: E('WF64(%s, ST(0)); FPOP();' % A); return
            if reg == 7: E('W16(%s, FSW());' % A); return
            if reg == 6: E('fpu_fnsave(c, %s);' % A); return
            if reg == 4: E('fpu_frstor(c, %s);' % A); return
        if op0 == 0xdf:
            if reg == 0: E('FPUSH((double)(int16_t)R16(%s));' % A); return
            if reg == 2: E('W16(%s, fpu_toint16(c, ST(0)));' % A); return
            if reg == 3: E('W16(%s, fpu_toint16(c, ST(0))); FPOP();' % A); return
            if reg == 5: E('{ int64_t q = (int64_t)R64(%s); FPUSH((double)q); FI(0) = q; FIV(0) = 1; }' % A); return
            if reg == 7: E('W64(%s, FIEXACT(0) ? (uint64_t)FI(0) : fpu_toint64(c, ST(0))); FPOP();' % A); return
        raise Unsupported('fpu mem %x/%d' % (op0, reg))
    st = 'ST(%d)' % rm
    if op0 == 0xd8:
        k = ARITH[reg]
        if k in ('com', 'comp'):
            E('FCOM(ST(0), %s);' % st)
            if k == 'comp': E('FPOP();')
        else: E(arith(k, 'ST(0)', st))
        return
    if op0 in (0xdc, 0xde):
        # dst = st(i), src = st0. Intel: DC E0 fsubr st(i),st0 : st(i)=st0-st(i); DC E8 fsub: st(i)=st(i)-st0
        m = {0: '+', 1: '*', 4: '-r', 5: '-', 6: '/r', 7: '/'}
        if op0 == 0xde and modrm == 0xd9: E('FCOM(ST(0), ST(1)); FPOP(); FPOP();'); return
        if reg in (2, 3):
            E('FCOM(ST(0), %s);' % st)
            if reg == 3 or op0 == 0xde: E('FPOP();')
            return
        E(arith(m[reg], st, 'ST(0)'))
        if op0 == 0xde: E('FPOP();')
        return
    if op0 == 0xd9:
        if reg == 3: E('ST(%d) = ST(0); FI(%d) = FI(0); FIV(%d) = FIV(0); FPOP();' % (rm, rm, rm)); return  # fstp1 alias
        if reg == 0: E('{ double t = %s; int64_t q = FI(%d); uint8_t v = FIV(%d); FPUSH(t); FI(0) = q; FIV(0) = v; }' % (st, rm, rm)); return
        if reg == 1: E('{ double t = ST(0); ST(0) = %s; %s = t; int64_t q = FI(0); FI(0) = FI(%d); FI(%d) = q; uint8_t v = FIV(0); FIV(0) = FIV(%d); FIV(%d) = v; }' % (st, st, rm, rm, rm, rm)); return
        x = {0xd0: '', 0xe0: 'ST(0) = -ST(0);', 0xe1: 'ST(0) = fabs(ST(0));', 0xe4: 'FCOM(ST(0), 0.0);',
             0xe5: 'fpu_fxam(c);', 0xe8: 'FPUSH(1.0);', 0xe9: 'FPUSH(3.321928094887362);',
             0xea: 'FPUSH(1.4426950408889634);', 0xeb: 'FPUSH(3.141592653589793);',
             0xec: 'FPUSH(0.30102999566398120);', 0xed: 'FPUSH(0.69314718055994531);', 0xee: 'FPUSH(0.0);',
             0xf0: 'ST(0) = exp2(ST(0)) - 1.0;', 0xf1: 'ST(1) = ST(1) * log2(ST(0)); FPOP();',
             0xf2: 'ST(0) = tan(ST(0)); FPUSH(1.0);', 0xf3: 'ST(1) = atan2(ST(1), ST(0)); FPOP();',
             0xf8: 'ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x0400;', 0xf5: 'ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x0400;',
             0xfa: 'ST(0) = sqrt(ST(0));', 0xfb: '{ double t = ST(0); ST(0) = sin(t); FPUSH(cos(t)); }',
             0xfc: 'ST(0) = fpu_round(c, ST(0));', 0xfd: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
             0xfe: 'ST(0) = sin(ST(0));', 0xff: 'ST(0) = cos(ST(0));', 0xf6: 'c->ftop = (c->ftop - 1) & 7;',
             0xf7: 'c->ftop = (c->ftop + 1) & 7;', 0xf9: 'ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();',
             0xf4: 'fpu_fxtract(c);'}.get(modrm)
        if x is not None: E(x); return
    if op0 == 0xdb:
        if modrm == 0xe2: E('c->fsw &= 0x7f00;'); return
        if modrm == 0xe3: E('fpu_init(c);'); return
        if modrm == 0xe0 or modrm == 0xe1 or modrm == 0xe4: return  # feni/fdisi/fsetpm nop
    if op0 == 0xdd:
        if reg == 0: return  # ffree
        if reg == 2: E('ST(%d) = ST(0); FI(%d) = FI(0); FIV(%d) = FIV(0);' % (rm, rm, rm)); return
        if reg == 3: E('ST(%d) = ST(0); FI(%d) = FI(0); FIV(%d) = FIV(0); FPOP();' % (rm, rm, rm)); return
        if reg == 4: E('FCOM(ST(0), %s);' % st); return
        if reg == 5: E('FCOM(ST(0), %s); FPOP();' % st); return
    if op0 == 0xda and modrm == 0xe9: E('FCOM(ST(0), ST(1)); FPOP(); FPOP();'); return
    if op0 == 0xdf and modrm == 0xe0: E(reg_set('ax', 'FSW()')); return
    if op0 == 0xdf and reg == 0: return  # ffreep-ish
    raise Unsupported('fpu reg %x %x' % (op0, modrm))

def lift_ins(ins, out, last, jts, entry, labels):
    """returns new 'last' flag-op descriptor"""
    E = out.append
    iid = ins.id
    ops = ins.operands
    a = ins.address
    nxt = a + ins.size
    mn = ins.mnemonic
    if 0xd8 <= ins.bytes[0] <= 0xdf or (ins.bytes[0] in (0x9b, 0x66, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65) and any(0xd8 <= x <= 0xdf for x in ins.bytes[1:3]) and mn.startswith('f')):
        lift_fpu(ins, out); return last
    def binop(op, setflags):
        d, s = ops[0], ops[1]
        sz = d.size
        E('{ uint32_t a = %s, b = %s & %s, r = (a %s b) & %s;' % (get(ins, d), get(ins, s, sz), MASK[sz], op, MASK[sz], ))
        return d, s, sz
    if iid == X.X86_INS_NOP or iid == X.X86_INS_WAIT: return last
    if iid == X.X86_INS_MOV:
        E(put(ins, ops[0], get(ins, ops[1], ops[0].size))); return last
    if iid in (X.X86_INS_MOVZX, X.X86_INS_MOVSX):
        s = get(ins, ops[1])
        if iid == X.X86_INS_MOVSX: s = '(uint32_t)(int32_t)(%s)(%s)' % (STY[ops[1].size], s)
        E(put(ins, ops[0], s)); return last
    if iid == X.X86_INS_LEA:
        E(put(ins, ops[0], ea(ins, ops[1]))); return last
    if iid == X.X86_INS_PUSH:
        sz = ops[0].size
        if ops[0].type == X.X86_OP_IMM: sz = 4 if ins.prefix[2] != 0x66 else 2
        E('{ uint32_t v = %s; esp -= %d; W%d(esp, v); }' % (get(ins, ops[0], sz), sz, sz * 8)); return last
    if iid == X.X86_INS_POP:
        sz = ops[0].size
        E('{ uint32_t v = R%d(esp); esp += %d; %s }' % (sz * 8, sz, put(ins, ops[0], 'v'))); return last
    if mn in ('pushal', 'pushad'):
        E('{ uint32_t t = esp; esp -= 32; W32(esp+28,eax); W32(esp+24,ecx); W32(esp+20,edx); W32(esp+16,ebx); W32(esp+12,t); W32(esp+8,ebp); W32(esp+4,esi); W32(esp,edi); }'); return last
    if mn in ('popal', 'popad'):
        E('{ edi=R32(esp); esi=R32(esp+4); ebp=R32(esp+8); ebx=R32(esp+16); edx=R32(esp+20); ecx=R32(esp+24); eax=R32(esp+28); esp += 32; }'); return last
    if iid in (X.X86_INS_PUSHFD, X.X86_INS_PUSHF):
        E('esp -= 4; W32(esp, MKFLAGS());'); return last
    if iid in (X.X86_INS_POPFD, X.X86_INS_POPF):
        E('{ uint32_t v = R32(esp); esp += 4; SETFLAGS(v); }'); return None
    if iid in (X.X86_INS_ADD, X.X86_INS_SUB, X.X86_INS_CMP, X.X86_INS_ADC, X.X86_INS_SBB):
        d, s = ops[0], ops[1]; sz = d.size; hb = SBITS[sz] - 1
        A, B = get(ins, d), get(ins, s, sz)
        if iid == X.X86_INS_ADD:
            E('{ uint32_t a = %s, b = %s, r = (a + b) & %s;' % (A, B, MASK[sz]))
            E(' cf = r < a; of = ((a ^ r) & (b ^ r)) >> %d & 1; %s fa = a; fb = b; fr = r;' % (hb, szf(sz)))
            k = ('add', sz)
        elif iid == X.X86_INS_ADC:
            E('{ uint32_t a = %s, b = %s, ci = cf, r = (a + b + ci) & %s;' % (A, B, MASK[sz]))
            E(' cf = ci ? (r <= a) : (r < a); of = ((a ^ r) & (b ^ r)) >> %d & 1; %s fr = r;' % (hb, szf(sz)))
            k = None
        elif iid == X.X86_INS_SBB:
            E('{ uint32_t a = %s, b = %s, ci = cf, r = (a - b - ci) & %s;' % (A, B, MASK[sz]))
            E(' cf = ci ? (a <= b) : (a < b); of = ((a ^ b) & (a ^ r)) >> %d & 1; %s fr = r;' % (hb, szf(sz)))
            k = None
        else:
            E('{ uint32_t a = %s, b = %s, r = (a - b) & %s;' % (A, B, MASK[sz]))
            E(' cf = a < b; of = ((a ^ b) & (a ^ r)) >> %d & 1; %s fa = a; fb = b; fr = r;' % (hb, szf(sz)))
            k = ('sub', sz)
        if iid != X.X86_INS_CMP: E(' ' + put(ins, d, 'r'))
        E('}')
        return k
    if iid in (X.X86_INS_AND, X.X86_INS_OR, X.X86_INS_XOR, X.X86_INS_TEST):
        d, s = ops[0], ops[1]; sz = d.size
        o = {X.X86_INS_AND: '&', X.X86_INS_OR: '|', X.X86_INS_XOR: '^', X.X86_INS_TEST: '&'}[iid]
        if iid == X.X86_INS_XOR and d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
            E('{ uint32_t r = 0; %s cf = of = 0; %s fr = r; }' % (put(ins, d, '0'), szf(sz))); return ('logic', sz)
        E('{ uint32_t r = (%s %s %s) & %s; cf = of = 0; %s fr = r;' % (get(ins, d), o, get(ins, s, sz), MASK[sz], szf(sz)))
        if iid != X.X86_INS_TEST: E(' ' + put(ins, d, 'r'))
        E('}')
        return ('logic', sz)
    if iid in (X.X86_INS_INC, X.X86_INS_DEC):
        d = ops[0]; sz = d.size; hb = SBITS[sz] - 1
        if iid == X.X86_INS_INC:
            E('{ uint32_t a = %s, r = (a + 1) & %s; of = r == 0x%xu; %s fr = r; %s }' % (get(ins, d), MASK[sz], 1 << hb, szf(sz), put(ins, d, 'r')))
        else:
            E('{ uint32_t a = %s, r = (a - 1) & %s; of = a == 0x%xu; %s fr = r; %s }' % (get(ins, d), MASK[sz], 1 << hb, szf(sz), put(ins, d, 'r')))
        return ('incdec', sz)
    if iid == X.X86_INS_NEG:
        d = ops[0]; sz = d.size; hb = SBITS[sz] - 1
        E('{ uint32_t a = %s, r = (0u - a) & %s; cf = a != 0; of = a == 0x%xu; %s fr = r; %s }' % (get(ins, d), MASK[sz], 1 << hb, szf(sz), put(ins, d, 'r')))
        return None
    if iid == X.X86_INS_NOT:
        d = ops[0]; sz = d.size
        E(put(ins, d, '~%s & %s' % (get(ins, d), MASK[sz]))); return last
    if iid == X.X86_INS_SAL: iid = X.X86_INS_SHL
    if iid == X.X86_INS_ENTER:
        n = ops[0].imm; lvl = ops[1].imm
        if lvl != 0: raise Unsupported('enter level')
        E('esp -= 4; W32(esp, ebp); ebp = esp; esp -= %d;' % n); return last
    if iid in (X.X86_INS_SHL, X.X86_INS_SHR, X.X86_INS_SAR, X.X86_INS_ROL, X.X86_INS_ROR, X.X86_INS_RCL, X.X86_INS_RCR):
        d = ops[0]; sz = d.size; bits = SBITS[sz]; hb = bits - 1
        cnt = get(ins, ops[1]) if len(ops) > 1 else '1'
        E('{ uint32_t n = (%s) & 31; if (n) { uint32_t a = %s, r;' % (cnt, get(ins, d)))
        if iid == X.X86_INS_SHL:
            E(' r = (uint32_t)(((uint64_t)a << n) & %s); cf = (uint32_t)(((uint64_t)a << n) >> %d) & 1; of = ((r >> %d) & 1) ^ cf; %s fr = r;' % (MASK[sz], bits, hb, szf(sz)))
        elif iid == X.X86_INS_SHR:
            E(' r = a >> n; cf = (uint32_t)((uint64_t)a >> (n - 1)) & 1; of = (a >> %d) & 1; %s fr = r;' % (hb, szf(sz)))
        elif iid == X.X86_INS_SAR:
            E(' int32_t sa = (int32_t)(%s)a; r = (uint32_t)(sa >> (n > %d ? %d : n)) & %s; cf = (uint32_t)(sa >> (n - 1 > %d ? %d : n - 1)) & 1; of = 0; %s fr = r;' % (STY[sz], hb, hb, MASK[sz], hb, hb, szf(sz)))
        elif iid == X.X86_INS_ROL:
            E(' uint32_t m = n %% %d; r = m ? ((a << m) | (a >> (%d - m))) & %s : a; cf = r & 1; of = ((r >> %d) & 1) ^ cf;' % (bits, bits, MASK[sz], hb))
        elif iid == X.X86_INS_ROR:
            E(' uint32_t m = n %% %d; r = m ? ((a >> m) | (a << (%d - m))) & %s : a; cf = (r >> %d) & 1; of = ((r >> %d) ^ (r >> %d)) & 1;' % (bits, bits, MASK[sz], hb, hb, hb - 1))
        elif iid == X.X86_INS_RCL:
            E(' uint64_t w = ((uint64_t)cf << %d) | a; uint32_t m = n %% %d; if (m) w = ((w << m) | (w >> (%d - m))) & ((1ull << %d) - 1); r = (uint32_t)w & %s; cf = (uint32_t)(w >> %d) & 1; of = ((r >> %d) & 1) ^ cf;' % (bits, bits + 1, bits + 1, bits + 1, MASK[sz], bits, hb))
        elif iid == X.X86_INS_RCR:
            E(' uint64_t w = ((uint64_t)cf << %d) | a; uint32_t m = n %% %d; if (m) w = ((w >> m) | (w << (%d - m))) & ((1ull << %d) - 1); r = (uint32_t)w & %s; cf = (uint32_t)(w >> %d) & 1; of = ((r >> %d) ^ (r >> %d)) & 1;' % (bits, bits + 1, bits + 1, bits + 1, MASK[sz], bits, hb, hb - 1))
        E(' %s } }' % put(ins, d, 'r'))
        return None
    if iid in (X.X86_INS_SHLD, X.X86_INS_SHRD):
        d, s = ops[0], ops[1]
        cnt = get(ins, ops[2])
        if iid == X.X86_INS_SHLD:
            E('{ uint32_t n = (%s) & 31; if (n) { uint32_t a = %s, b = %s, r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1; %s %s } }' % (cnt, get(ins, d), get(ins, s), szf(4), put(ins, d, 'r')))
        else:
            E('{ uint32_t n = (%s) & 31; if (n) { uint32_t a = %s, b = %s, r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1; %s %s } }' % (cnt, get(ins, d), get(ins, s), szf(4), put(ins, d, 'r')))
        return None
    if iid == X.X86_INS_IMUL:
        if len(ops) == 1:
            sz = ops[0].size
            if sz == 4:
                E('{ int64_t p = (int64_t)(int32_t)eax * (int32_t)%s; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32); cf = of = p != (int32_t)p; }' % get(ins, ops[0]))
            elif sz == 2:
                E('{ int32_t p = (int16_t)(eax & 0xffff) * (int16_t)%s; %s %s cf = of = p != (int16_t)p; }' % (get(ins, ops[0]), reg_set('ax', 'p'), reg_set('dx', '(uint32_t)p >> 16')))
            else:
                E('{ int32_t p = (int8_t)(eax & 0xff) * (int8_t)%s; %s cf = of = p != (int8_t)p; }' % (get(ins, ops[0]), reg_set('ax', 'p')))
            return None
        d = ops[0]; sz = d.size
        if len(ops) == 2: A, B = get(ins, d), get(ins, ops[1], sz)
        else: A, B = get(ins, ops[1], sz), get(ins, ops[2], sz)
        E('{ int64_t p = (int64_t)(%s)%s * (int64_t)(%s)%s; cf = of = p != (%s)p; %s }' % (STY[sz], A, STY[sz], B, STY[sz], put(ins, d, '(uint32_t)p & %s' % MASK[sz])))
        return None
    if iid == X.X86_INS_MUL:
        sz = ops[0].size
        if sz == 4: E('{ uint64_t p = (uint64_t)eax * %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); cf = of = edx != 0; }' % get(ins, ops[0]))
        elif sz == 2: E('{ uint32_t p = (eax & 0xffff) * %s; %s %s cf = of = (p >> 16) != 0; }' % (get(ins, ops[0]), reg_set('ax', 'p'), reg_set('dx', 'p >> 16')))
        else: E('{ uint32_t p = (eax & 0xff) * %s; %s cf = of = (p >> 8) != 0; }' % (get(ins, ops[0]), reg_set('ax', 'p')))
        return None
    if iid in (X.X86_INS_DIV, X.X86_INS_IDIV):
        sz = ops[0].size
        s = get(ins, ops[0])
        if sz == 4:
            if iid == X.X86_INS_DIV:
                E('{ uint32_t dv = %s; uint64_t n = ((uint64_t)edx << 32) | eax; if (!dv) rt_divzero(c, 0x%x); eax = (uint32_t)(n / dv); edx = (uint32_t)(n %% dv); }' % (s, a))
            else:
                E('{ int32_t dv = (int32_t)%s; int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); if (!dv) rt_divzero(c, 0x%x); eax = (uint32_t)(int32_t)(n / dv); edx = (uint32_t)(int32_t)(n %% dv); }' % (s, a))
        elif sz == 2:
            if iid == X.X86_INS_DIV:
                E('{ uint32_t dv = %s; uint32_t n = ((edx & 0xffff) << 16) | (eax & 0xffff); if (!dv) rt_divzero(c, 0x%x); %s %s }' % (s, a, reg_set('ax', 'n / dv'), reg_set('dx', 'n % dv')))
            else:
                E('{ int32_t dv = (int16_t)%s; int32_t n = (int32_t)(((edx & 0xffff) << 16) | (eax & 0xffff)); if (!dv) rt_divzero(c, 0x%x); %s %s }' % (s, a, reg_set('ax', 'n / dv'), reg_set('dx', 'n % dv')))
        else:
            if iid == X.X86_INS_DIV:
                E('{ uint32_t dv = %s; uint32_t n = eax & 0xffff; if (!dv) rt_divzero(c, 0x%x); %s %s }' % (s, a, reg_set('al', 'n / dv'), reg_set('ah', 'n % dv')))
            else:
                E('{ int32_t dv = (int8_t)%s; int32_t n = (int16_t)(eax & 0xffff); if (!dv) rt_divzero(c, 0x%x); %s %s }' % (s, a, reg_set('al', 'n / dv'), reg_set('ah', 'n % dv')))
        return None
    if iid == X.X86_INS_CDQ: E('edx = (int32_t)eax < 0 ? 0xffffffffu : 0;'); return last
    if iid == X.X86_INS_CWDE: E('eax = (uint32_t)(int32_t)(int16_t)eax;'); return last
    if iid == X.X86_INS_CBW: E(reg_set('ax', '(uint32_t)(int16_t)(int8_t)eax')); return last
    if iid == X.X86_INS_CWD: E(reg_set('dx', '((int16_t)eax < 0) ? 0xffff : 0')); return last
    if iid == X.X86_INS_XCHG:
        d, s = ops[0], ops[1]
        E('{ uint32_t t1 = %s, t2 = %s; %s %s }' % (get(ins, d), get(ins, s), put(ins, d, 't2'), put(ins, s, 't1'))); return last
    if iid == X.X86_INS_XADD:
        d, s = ops[0], ops[1]; sz = d.size
        E('{ uint32_t a = %s, b = %s, r = (a + b) & %s; cf = r < a; %s %s %s }' % (get(ins, d), get(ins, s), MASK[sz], szf(sz), put(ins, s, 'a'), put(ins, d, 'r'))); return None
    if iid == X.X86_INS_CMPXCHG:
        d, s = ops[0], ops[1]; sz = d.size
        acc = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        E('{ uint32_t a = %s, dv = %s; zf = a == dv; if (zf) { %s } else { %s } }' % (reg_get(acc), get(ins, d), put(ins, d, get(ins, s)), reg_set(acc, 'dv'))); return None
    if iid in (X.X86_INS_BT, X.X86_INS_BTS, X.X86_INS_BTR, X.X86_INS_BTC):
        d, s = ops[0], ops[1]
        if d.type != X.X86_OP_REG:
            A = ea(ins, d)
            if s.type == X.X86_OP_IMM:
                n = s.imm & (SBITS[d.size] - 1)
                E('{ uint32_t ba = %s + %d; uint32_t v = R8(ba), n = %d; cf = (v >> n) & 1;' % (A, n >> 3, n & 7))
            else:
                E('{ uint32_t ba = %s + (uint32_t)((int32_t)%s >> 3); uint32_t v = R8(ba), n = %s & 7; cf = (v >> n) & 1;' % (A, get(ins, s), get(ins, s)))
            if iid == X.X86_INS_BTS: E('W8(ba, v | (1u << n));')
            if iid == X.X86_INS_BTR: E('W8(ba, v & ~(1u << n));')
            if iid == X.X86_INS_BTC: E('W8(ba, v ^ (1u << n));')
            E('}'); return None
        E('{ uint32_t v = %s, n = (%s) & %d; cf = (v >> n) & 1;' % (get(ins, d), get(ins, s), SBITS[d.size] - 1))
        if iid == X.X86_INS_BTS: E(put(ins, d, 'v | (1u << n)'))
        if iid == X.X86_INS_BTR: E(put(ins, d, 'v & ~(1u << n)'))
        if iid == X.X86_INS_BTC: E(put(ins, d, 'v ^ (1u << n)'))
        E('}'); return None
    if iid in (X.X86_INS_BSF, X.X86_INS_BSR):
        E('{ uint32_t v = %s; zf = v == 0; if (v) { %s } }' % (get(ins, ops[1]), put(ins, ops[0], '__builtin_ctz(v)' if iid == X.X86_INS_BSF else '31 - __builtin_clz(v)'))); return None
    if mn.startswith('set'):
        cid = CC_NAMES[mn[3:].replace('nz', 'ne').replace('z', 'e').replace('nae', 'b').replace('nbe', 'a').replace('nb', 'ae').replace('nle','g').replace('nl','ge').replace('nge','l').replace('ng','le')]
        E(put(ins, ops[0], '(%s) ? 1 : 0' % cond_of(cid, last))); return last
    if mn.startswith('cmov'):
        cid = CC_NAMES[mn[4:]]
        E('if (%s) { %s }' % (cond_of(cid, last), put(ins, ops[0], get(ins, ops[1])))); return last
    if iid == X.X86_INS_LEAVE: E('esp = ebp; ebp = R32(esp); esp += 4;'); return last
    if iid == X.X86_INS_CLD: E('c->df = 0;'); return last
    if iid == X.X86_INS_STD: E('c->df = 1;'); return last
    if iid == X.X86_INS_CLC: E('cf = 0;'); return None
    if iid == X.X86_INS_STC: E('cf = 1;'); return None
    if iid == X.X86_INS_CMC: E('cf = !cf;'); return None
    if iid == X.X86_INS_SAHF: E('{ uint32_t h = (eax >> 8) & 0xff; cf = h & 1; pf = (h >> 2) & 1; zf = (h >> 6) & 1; sf = (h >> 7) & 1; }'); return None
    if iid == X.X86_INS_LAHF: E(reg_set('ah', '(cf | 2 | (pf << 2) | (zf << 6) | (sf << 7))')); return last
    if iid == X.X86_INS_XLATB: E(reg_set('al', 'R8(ebx + (eax & 0xff))')); return last
    if iid == X.X86_INS_CPUID: E('SAVE(); rt_cpuid(c); LOAD();'); return last
    if iid == X.X86_INS_RDTSC: E('{ uint64_t t = rt_rdtsc(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }'); return last
    if iid == X.X86_INS_BSWAP: E(put(ins, ops[0], '__builtin_bswap32(%s)' % get(ins, ops[0]))); return last
    # string ops
    STR = {X.X86_INS_MOVSB: ('movs', 1), X.X86_INS_MOVSW: ('movs', 2), X.X86_INS_MOVSD: ('movs', 4),
           X.X86_INS_STOSB: ('stos', 1), X.X86_INS_STOSW: ('stos', 2), X.X86_INS_STOSD: ('stos', 4),
           X.X86_INS_LODSB: ('lods', 1), X.X86_INS_LODSW: ('lods', 2), X.X86_INS_LODSD: ('lods', 4),
           X.X86_INS_CMPSB: ('cmps', 1), X.X86_INS_CMPSW: ('cmps', 2), X.X86_INS_CMPSD: ('cmps', 4),
           X.X86_INS_SCASB: ('scas', 1), X.X86_INS_SCASW: ('scas', 2), X.X86_INS_SCASD: ('scas', 4)}
    if iid in STR:
        k, sz = STR[iid]
        rep = ins.prefix[0]
        acc = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        step = '(c->df ? -%d : %d)' % (sz, sz)
        if k == 'movs': body = 'W%d(edi, R%d(esi)); esi += %s; edi += %s;' % (sz * 8, sz * 8, step, step)
        elif k == 'stos': body = 'W%d(edi, %s); edi += %s;' % (sz * 8, reg_get(acc), step)
        elif k == 'lods': body = '%s esi += %s;' % (reg_set(acc, 'R%d(esi)' % (sz * 8)), step)
        elif k == 'cmps': body = '{ uint32_t a = R%d(esi), b = R%d(edi), r = (a - b) & %s; cf = a < b; of = ((a ^ b) & (a ^ r)) >> %d & 1; %s fa = a; fb = b; fr = r; } esi += %s; edi += %s;' % (sz * 8, sz * 8, MASK[sz], SBITS[sz] - 1, szf(sz), step, step)
        else: body = '{ uint32_t a = %s, b = R%d(edi), r = (a - b) & %s; cf = a < b; of = ((a ^ b) & (a ^ r)) >> %d & 1; %s fa = a; fb = b; fr = r; } edi += %s;' % (reg_get(acc), sz * 8, MASK[sz], SBITS[sz] - 1, szf(sz), step)
        if rep == 0:
            E(body)
        elif k in ('movs', 'stos') and rep == 0xf3:
            if k == 'movs': E('if (!c->df) { rt_movs(M, edi, esi, ecx * %d); esi += ecx * %d; edi += ecx * %d; ecx = 0; } else while (ecx) { %s ecx--; }' % (sz, sz, sz, body))
            elif sz == 4: E('if (!c->df) { rt_stos32(M, edi, eax, ecx); edi += ecx * 4; ecx = 0; } else while (ecx) { %s ecx--; }' % body)
            else: E('while (ecx) { %s ecx--; }' % body)
        elif k in ('cmps', 'scas'):
            term = '!zf' if rep == 0xf3 else 'zf'
            E('while (ecx) { %s ecx--; if (%s) break; }' % (body, term))
            return ('sub', sz)
        else:
            E('while (ecx) { %s ecx--; }' % body)
        return ('sub', sz) if k in ('cmps', 'scas') else last
    if iid == X.X86_INS_INT3 or iid == X.X86_INS_HLT or iid == X.X86_INS_UD2:
        E('rt_trap(c, 0x%x);' % a); return last
    if iid in (X.X86_INS_IN, X.X86_INS_OUT, X.X86_INS_INT, X.X86_INS_INTO):
        E('rt_trap(c, 0x%x);' % a); return last
    if iid == X.X86_INS_AAS:
        E('rt_trap(c, 0x%x);' % a); return last
    raise Unsupported(mn)

ret_counter = [0]

def emit_func(entry, insns, jts):
    out = []
    E = out.append
    addrs = sorted(k for k, v in insns.items() if v is not None)
    labels = set()
    for a in addrs:
        ins = insns[a]
        if ins.id in JCC or (ins.id == X.X86_INS_JMP and ins.operands[0].type == X.X86_OP_IMM):
            labels.add(ins.operands[0].imm & 0xffffffff)
    for t in jts.values(): labels.update(t)
    E('void %s(CPU *c) {' % fn_name(entry))
    E(' DECLS();')
    E(' goto L_%x;' % entry)
    last = None
    prev_end = None
    for a in addrs:
        ins = insns[a]
        if prev_end is not None and prev_end != a:
            E(' goto L_%x;' % prev_end)
            last = None
        if a in labels or prev_end != a: last = None
        E('L_%x: /* %s %s */' % (a, ins.mnemonic, ins.op_str.replace('*/', '')))
        prev_end = a + ins.size
        iid = ins.id
        if a in PREHOOKS:
            E(' SAVE(); rt_hook(c, 0x%xu); LOAD();' % a)
        try:
            if iid in JCC:
                t = ins.operands[0].imm & 0xffffffff
                if iid == X.X86_INS_JECXZ: E(' if (ecx == 0) goto L_%x;' % t)
                elif iid == X.X86_INS_JCXZ: E(' if ((ecx & 0xffff) == 0) goto L_%x;' % t)
                elif iid == X.X86_INS_LOOP: E(' if (--ecx) goto L_%x;' % t)
                elif iid == X.X86_INS_LOOPE: E(' if (--ecx && zf) goto L_%x;' % t)
                elif iid == X.X86_INS_LOOPNE: E(' if (--ecx && !zf) goto L_%x;' % t)
                else: E(' if (%s) goto L_%x;' % (cond_of(iid, last), t))
                continue
            if iid == X.X86_INS_JMP:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if t in funcs and t != entry and t not in insns:
                        E(' SAVE(); %s(c); return;' % fn_name(t))
                    else:
                        E(' goto L_%x;' % t)
                    prev_end = None
                    continue
                if a in jts:
                    E(' { uint32_t t = %s; switch (t) {' % get(ins, op))
                    for t in sorted(set(jts[a])): E('  case 0x%xu: goto L_%x;' % (t, t))
                    E('  default: SAVE(); rt_badjump(c, 0x%x, t); return; } }' % a)
                    prev_end = None
                    continue
                E(' { uint32_t t = %s; SAVE(); rt_call(c, t); return; }' % get(ins, op))
                prev_end = None
                continue
            if iid == X.X86_INS_CALL:
                op = ins.operands[0]
                ra = a + ins.size
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if t in funcs:
                        E(' esp -= 4; W32(esp, 0x%xu); SAVE(); %s(c); LOAD();' % (ra, fn_name(t)))
                    else:
                        E(' esp -= 4; W32(esp, 0x%xu); SAVE(); rt_call(c, 0x%xu); LOAD();' % (ra, t))
                else:
                    E(' { uint32_t t = %s; esp -= 4; W32(esp, 0x%xu); SAVE(); rt_call(c, t); LOAD(); }' % (get(ins, op), ra))
                last = None
                continue
            if iid == X.X86_INS_RET:
                n = ins.operands[0].imm if ins.operands else 0
                E(' esp += %d; SAVE(); return;' % (4 + n))
                prev_end = None
                continue
            body = []
            last = lift_ins(ins, body, last, jts, entry, labels)
            for l in body: E(' ' + l)
            if a in HOOKS:
                E(' SAVE(); rt_hook(c, 0x%xu); LOAD();' % a)
            if iid in (X.X86_INS_INT3, X.X86_INS_HLT, X.X86_INS_UD2):
                E(' SAVE(); return;'); prev_end = None
        except Unsupported as ex:
            unsupported[str(ex)] += 1
            E(' rt_unimpl(c, 0x%x, "%s %s");' % (a, ins.mnemonic, ins.op_str))
            E(' SAVE(); return;')
            prev_end = None
            last = None
    if prev_end is not None:
        E(' goto L_%x;' % prev_end) if prev_end in insns else E(' SAVE(); rt_fallthrough(c, 0x%x); return;' % prev_end)
    # bad decode targets
    for a, v in insns.items():
        if v is None:
            E('L_%x: SAVE(); rt_trap(c, 0x%x); return;' % (a, a))
    E('}')
    # labels referenced but absent (outside text etc.)
    have = set(insns)
    missing = [l for l in labels if l not in have]
    for l in missing:
        out.insert(len(out) - 1, 'L_%x: SAVE(); rt_trap(c, 0x%x); return;' % (l, l))
    return out

os.makedirs(OUT, exist_ok=True)
names = sorted(funcs)
NFILES = args.nfiles or max(1, min(64, len(names) // 80))
chunks = [names[i * len(names) // NFILES:(i + 1) * len(names) // NFILES] for i in range(NFILES)]
with open(os.path.join(OUT, 'funcs.h'), 'w') as f:
    f.write('#pragma once\n#include "cpu.h"\n')
    for e in names: f.write('void %s(CPU *c);\n' % fn_name(e))
for i, ch in enumerate(chunks):
    with open(os.path.join(OUT, '%s_%02d.c' % (MOD, i)), 'w') as f:
        f.write('#define C2_GEN 1\n#include "funcs.h"\n#include "lifted.h"\n\n')
        for e in ch:
            insns, jts = funcs[e]
            f.write('\n'.join(emit_func(e, insns, jts)))
            f.write('\n\n')
with open(os.path.join(OUT, '%s_table.c' % MOD), 'w') as f:
    f.write('#define C2_GEN 1\n#include "funcs.h"\n#include "lifted.h"\n')
    f.write('static const FuncEntry funcs[] = {\n')
    for e in names: f.write(' {0x%xu, %s},\n' % (e, fn_name(e)))
    f.write('};\nstatic const ImportEntry imports[] = {\n')
    for a, (dll, n) in sorted(iat.items()): f.write(' {0x%xu, "%s", "%s"},\n' % (a, dll, n))
    f.write(' {0, 0, 0}\n};\n')
    f.write('const LiftedModule g_mod_%s = {"%s", 0x%xu, %d, funcs, imports, %d};\n' % (MOD, os.path.basename(EXE), IB, len(names), len(iat)))
print('unsupported:', dict(unsupported), file=sys.stderr)

# coverage report
covb = compute_cov()
tot = sum(hi - lo for lo, hi in CODE)
print('coverage: %d / %d bytes (%.1f%%)' % (sum(covb), tot, 100.0 * sum(covb) / tot), file=sys.stderr)
gaps = []
for lo, hi in CODE:
    a = lo
    while a < hi:
        if not covb[a - TLO]:
            b = a
            while b < hi and not covb[b - TLO]: b += 1
            chunk = img[a - IB:b - IB]
            if b - a > 16 and any(x not in PAD for x in chunk): gaps.append((a, b - a))
            a = b
        else: a += 1
with open(os.path.join(OUT, 'gaps.txt'), 'w') as f:
    for g in gaps: f.write('%x %d\n' % g)
print('gaps:', len(gaps), file=sys.stderr)
