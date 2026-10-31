#!/usr/bin/env python3
"""Map of the gizmo classes of LEGO Star Wars: The Complete Saga (see docs/CLASSES.md).

  class_map.py dump [pid]            read the class table of the RUNNING game from /proc/<pid>/mem (pid default: pgrep LEGO)
                                     and write it as JSON on stdout (run it on the Deck / Linux box the game runs on)
  class_map.py rolls EXE             every call of the random functions in the whole exe (game sub_54FA60 and the C runtime
                                     rand 0x6E8E1E), split into GATES (a compare of the result follows: "random < N") and
                                     values; gates with a small N (below 0x4000, under 25 %) that run every call are the
                                     frame-rate bug. A float gate (fild, fmul 1/65536, fcom) is listed with the gates.
  class_map.py scan classes.json EXE for every class hook (+52 pre, +56 update, +60 render prep), look in the hook and one
                                     call level below for calls of the game's random function sub_54FA60; a hook that
                                     rolls it on every call is a frame-rate bug of the "twitch" kind (lever, hat machine,
                                     building blocks, spinner). EXE is the unpacked exe, orig/LEGOStarWarsSaga_unp.exe.
                                     Needs capstone (the repo .venv has it).
"""
import json, struct, subprocess, sys

CLASS_PTR = 0x93B268        # dword_93B268 -> {+0 count, +8 table}; entries are 156 bytes: +0 name, +52/+56/+60 hooks
STRIDE = 156
RANDOM_FN = 0x54FA60


def dump(pid):
    f = open('/proc/%d/mem' % pid, 'rb', buffering=0)

    def u32(a):
        f.seek(a)
        return struct.unpack('<I', f.read(4))[0]

    t = u32(CLASS_PTR)
    n, base = u32(t), u32(t + 8)
    rows = []
    for i in range(n):
        e = base + STRIDE * i
        f.seek(e)
        name = f.read(32).split(b'\0')[0].decode('latin1')
        rows.append({'index': i, 'name': name, 'pre': u32(e + 52), 'update': u32(e + 56), 'prep': u32(e + 60)})
    return rows


def rolls(exe):
    import capstone, re
    d = open(exe, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    o = pe + 24 + opt
    vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)       # first section = .text
    lo, code = base + va, d[ro:ro + max(vs, rs)]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    targets = {RANDOM_FN: 'game', 0x6E8E1E: 'crt'}
    for m in re.finditer(b'\xe8', code):
        p = m.start()
        if p + 5 > len(code):
            continue
        t = (lo + p + 5 + struct.unpack_from('<i', code, p + 1)[0]) & 0xffffffff
        if t not in targets:
            continue
        site = lo + p
        ins = list(md.disasm(code[p:p + 90], site))[1:16]
        mn = [x.mnemonic for x in ins]
        small = None
        gate = False
        for x in ins[:4]:
            if x.mnemonic == 'cmp' and ('eax' in x.op_str or 'al,' in x.op_str):
                gate = True
                mm = re.search(r',\s*(0x[0-9a-f]+)$', x.op_str)
                small = int(mm.group(1), 16) if mm else None
            if x.mnemonic.startswith('set') or (x.mnemonic == 'test' and 'eax' in x.op_str):
                gate = True
        if 'fild' in mn[:4] and any(k.startswith('fcom') or k.startswith('ficom') for k in mn[:14]):
            gate = True
        if gate:
            print('%06x %-4s %s%s' % (site, targets[t], ' '.join('%s %s' % (x.mnemonic, x.op_str) for x in ins[:4]),
                                       '   <-- small chance' if small is not None and small < 0x4000 else ''))


def scan(classes, exe):
    import capstone
    d = open(exe, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
        secs.append((base + va, max(vs, rs), ro))

    def off(a):
        for va, sz, ro in secs:
            if va <= a < va + sz:
                return ro + a - va

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def calls_of(fn):          # direct calls reachable from fn without leaving its body (recursive descent)
        seen, stack, calls = set(), [fn], set()
        while stack and len(seen) < 8000:
            a = stack.pop()
            while a not in seen:
                seen.add(a)
                ins = next(md.disasm(d[off(a):off(a) + 16], a, 1), None)
                if ins is None or ins.mnemonic == 'ret':
                    break
                if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
                    calls.add((ins.address, int(ins.op_str, 16)))
                if ins.mnemonic in ('jmp',) or ins.mnemonic.startswith('j'):
                    if ins.op_str.startswith('0x'):
                        t = int(ins.op_str, 16)
                        if fn <= t < fn + 0x6000:
                            if ins.mnemonic == 'jmp':
                                a = t
                                continue
                            stack.append(t)
                    if ins.mnemonic == 'jmp':
                        break
                a += ins.size
        return calls

    for c in classes:
        for kind in ('pre', 'update', 'prep'):
            h = c[kind]
            if not h:
                continue
            calls = calls_of(h)
            direct = sorted(a for a, t in calls if t == RANDOM_FN)
            deep = []
            for a, t in calls:
                if t != RANDOM_FN and 0x400000 <= t < 0x700000:
                    n = sorted(x for x, tt in calls_of(t) if tt == RANDOM_FN)
                    if n:
                        deep.append('%x(%d)' % (t, len(n)))
            if direct or deep:
                print('%2d %-13s %-6s %06x  direct %s  via %s' % (c['index'], c['name'], kind, h,
                      ' '.join('%x' % x for x in direct) or '-', ' '.join(deep) or '-'))


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == 'dump':
        pid = int(sys.argv[2]) if len(sys.argv) > 2 else int(subprocess.check_output('pgrep LEGO | head -1', shell=True))
        json.dump(dump(pid), sys.stdout, indent=1)
    elif len(sys.argv) == 3 and sys.argv[1] == 'rolls':
        rolls(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == 'scan':
        scan(json.load(open(sys.argv[2])), sys.argv[3])
    else:
        sys.exit(__doc__)
