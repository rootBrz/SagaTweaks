#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""List instructions whose memory operand has a given displacement (reg+disp), grouped by function.
Usage: find_disp.py exe disp_hex [mnemonic_prefix]"""
import sys, pefile, bisect
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
disp = int(sys.argv[2], 16); pref = sys.argv[3] if len(sys.argv) > 3 else ""
t = next(s for s in pe.sections if s.Name.startswith(b".text"))
tva = base + t.VirtualAddress; data = t.get_data()[: t.Misc_VirtualSize]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
insns = []; off = 0
while off < len(data):
    for i in md.disasm(data[off:], tva + off):
        insns.append(i); off = i.address + i.size - tva
    off += 1
starts = sorted({i.operands[0].imm for i in insns if i.mnemonic == "call" and i.operands and i.operands[0].type == X86_OP_IMM and tva <= i.operands[0].imm < tva + len(data)})
for i in insns:
    if not i.mnemonic.startswith(pref): continue
    for op in i.operands:
        if op.type == X86_OP_MEM and op.mem.base != 0 and (op.mem.disp & 0xFFFFFFFF) == disp:
            fn = starts[bisect.bisect_right(starts, i.address) - 1]
            print(f"{i.address:08x} (fn {fn:08x}) {i.mnemonic} {i.op_str}")
