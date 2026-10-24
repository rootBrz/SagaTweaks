#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
Static scan of the (unpacked) LEGO Star Wars TCS exe for code that assumes a fixed
frame rate. Reports:
  counters  - globals incremented/decremented by a constant (candidate per-frame counters)
  modulo    - globals read then used in a modulo / mask (flicker, blink, alternate-frame logic)
  consts    - code that references 1/60, 60.0, 30.0 style constants
Usage: scan_framerate.py LEGOStarWarsSaga_unp.exe [global_hex ...]
With global addresses, prints every instruction touching those globals.
"""
import struct, sys, collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_OP_REG

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
tva = base + text.VirtualAddress
tdata = text.get_data()[: text.Misc_VirtualSize]
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

def read(addr, n):
    try:
        return pe.get_data(addr - base, n)
    except Exception:
        return None

def const_kind(addr):
    d = read(addr, 8)
    if not d or len(d) < 4:
        return None
    f = struct.unpack("<f", d[:4])[0]
    kinds = []
    for name, v in (("1/60", 1 / 60), ("1/30", 1 / 30), ("60.0", 60.0), ("30.0", 30.0)):
        if abs(f - v) < v * 1e-4:
            kinds.append(name + " (float)")
    if len(d) == 8:
        g = struct.unpack("<d", d)[0]
        for name, v in (("1/60", 1 / 60), ("1/30", 1 / 30), ("60.0", 60.0), ("30.0", 30.0)):
            if abs(g - v) < v * 1e-4:
                kinds.append(name + " (double)")
    return kinds or None

# linear sweep, re-syncing on bad bytes
insns = []
off = 0
while off < len(tdata):
    got = False
    for i in md.disasm(tdata[off:], tva + off):
        insns.append(i)
        off = i.address + i.size - tva
        got = True
    if off >= len(tdata):
        break
    if not got or True:
        off += 1  # skip undecodable byte

focus = {int(a, 16) for a in sys.argv[2:]}

def abs_mem(op):
    if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp:
        d = op.mem.disp & 0xFFFFFFFF
        if base <= d < base + pe.OPTIONAL_HEADER.SizeOfImage:
            return d
    return None

if focus:
    for i in insns:
        for op in i.operands:
            a = abs_mem(op)
            if a in focus:
                print(f"{i.address:08x}  {i.mnemonic} {i.op_str}")
    sys.exit()

counters = collections.defaultdict(list)
modulo = collections.defaultdict(list)
consts = collections.defaultdict(list)

for idx, i in enumerate(insns):
    ops = i.operands
    # counters: inc/dec/add/sub [abs], imm
    if i.mnemonic in ("inc", "dec") and ops and abs_mem(ops[0]):
        counters[abs_mem(ops[0])].append((i.address, f"{i.mnemonic} {i.op_str}"))
    if i.mnemonic in ("add", "sub") and len(ops) == 2 and abs_mem(ops[0]) and ops[1].type == X86_OP_IMM:
        counters[abs_mem(ops[0])].append((i.address, f"{i.mnemonic} {i.op_str}"))
    # modulo: load global, then div/idiv/and-mask within next 8 insns
    if i.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG and abs_mem(ops[1]):
        g = abs_mem(ops[1])
        for j in insns[idx + 1: idx + 9]:
            if j.mnemonic in ("div", "idiv") or (
                j.mnemonic == "and" and len(j.operands) == 2 and j.operands[1].type == X86_OP_IMM
                and j.operands[1].imm in (1, 3, 7, 15, 31, 63)
            ) or (j.mnemonic == "test" and len(j.operands) == 2 and j.operands[1].type == X86_OP_IMM
                  and j.operands[1].imm in (1, 2, 4, 8)):
                modulo[g].append((i.address, j.address, j.mnemonic + " " + j.op_str))
                break
            if j.mnemonic in ("ret", "call", "jmp"):
                break
    # float constants
    for op in ops:
        a = abs_mem(op)
        if a and i.mnemonic.startswith("f"):
            k = const_kind(a)
            if k:
                consts[a].append((i.address, i.mnemonic, k))

# clocks advanced by sub_548FE0 (the engine's per-frame clock update); their integer
# frame counter at +12 is what blink / flicker code reads, and what the ASI now ticks at 60 Hz
CLOCK_UPDATE = 0x548FE0
clocks = set()
for idx, i in enumerate(insns):
    if i.mnemonic == "call" and i.operands and i.operands[0].type == X86_OP_IMM and i.operands[0].imm == CLOCK_UPDATE:
        for j in reversed(insns[max(0, idx - 4): idx]):
            if j.mnemonic == "push" and j.operands[0].type == X86_OP_IMM:
                clocks.add(j.operands[0].imm & 0xFFFFFFFF)
                break
counter_addrs = {c + 12: c for c in clocks}
print("== clocks updated by sub_548FE0 (counter = clock+12) and code reading the counter ==")
for c in sorted(clocks):
    print(f"clock {c:08x} (counter {c + 12:08x})")
for i in insns:
    for op in i.operands:
        a = abs_mem(op)
        if a in counter_addrs:
            print(f"  counter {a:08x} used at {i.address:08x}: {i.mnemonic} {i.op_str}")
print()

def fmt(g):
    return f"{g:08x}"

print("== counters: globals changed by constant, by number of sites (>=1) ==")
for g, sites in sorted(counters.items(), key=lambda kv: -len(kv[1])):
    mods = len(modulo.get(g, []))
    print(f"{fmt(g)} sites={len(sites)} modulo-uses={mods}  first={sites[0][0]:08x} {sites[0][1]}")

print("\n== modulo/mask uses of globals that are also counters ==")
for g, uses in sorted(modulo.items()):
    if g in counters:
        for a, b, t in uses:
            print(f"{fmt(g)} read@{a:08x} then {t}@{b:08x}")

print("\n== fixed-rate float constants (1/60, 60, 30, 1/30) ==")
for a, uses in sorted(consts.items()):
    kinds = const_kind(a)
    print(f"{fmt(a)} {kinds}: {len(uses)} refs")
    for u in uses[:50]:
        print(f"    {u[0]:08x} {u[1]}")
