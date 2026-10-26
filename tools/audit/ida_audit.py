#!/usr/bin/env python3
"""Build the data for a whole-loop code audit from the unpacked exe (see docs/AUDIT.md).

Run with a Python that can `import idapro` (idalib), e.g. the venv of the ida-pro-mcp plugin:
    <venv>/bin/python tools/audit/ida_audit.py <command> [options]

Commands (all work on a COPY of the .i64 so the GUI/MCP session keeps its lock):
  reach       functions reachable by static calls from a root (default sub_491D50, the logic update)
              -> <out>/reach.txt        "<addr> <size> <flag>"
  dtreaders   every function that reads the frame time flt_925018 (0x925018)
              -> <out>/dt_readers.txt   "<addr> IN|NOT <refs> <name>"   (IN = already in reach.txt)
  decompile   Hex-Rays text of a list of functions (+ optional callee closure not in reach.txt)
              -> <out>/<dir>/<addr>.c
  shard       strip blank lines and local declarations, split into shards of <= 1300 lines
              -> <out>/<dir>_shards/compact/<addr>.c and shard_NN.txt  ("<file> 1 <lines>")

Typical session:
  ida_audit.py reach       --root 0x491D50
  ida_audit.py decompile   --list reach
  ida_audit.py shard       --src pseudo --dst shards
  ida_audit.py dtreaders                               # round 3: dt readers missed by the call graph
  ida_audit.py decompile   --list notin --dst pseudo2 --closure
  ida_audit.py shard       --src pseudo2 --dst shards2 --only notin
"""
import argparse
import os
import re
import sys

IDA_DIR = os.environ.get("IDADIR", "/home/alan/ida-pro-9.3")
DT_ADDR = 0x925018
TEXT_LO, TEXT_HI = 0x401000, 0x700000      # game code; the CRT above it is skipped


def open_db(path):
    sys.path.insert(0, os.path.join(IDA_DIR, "idalib", "python"))
    import idapro
    idapro.open_database(path, True)
    return idapro


def callees(f):
    import idautils
    import ida_funcs
    import ida_xref
    out = set()
    for h in idautils.FuncItems(f.start_ea):
        for x in idautils.XrefsFrom(h, 0):
            if x.type in (ida_xref.fl_CN, ida_xref.fl_CF, ida_xref.fl_JN, ida_xref.fl_JF):   # calls and tail jumps
                g = ida_funcs.get_func(x.to)
                if g and g.start_ea == x.to:
                    out.add(x.to)
    return out


def closure(seeds, skip=()):
    import ida_funcs
    seen, todo = set(seeds), list(seeds)
    while todo:
        f = ida_funcs.get_func(todo.pop())
        if not f:
            continue
        for c in callees(f):
            if c not in seen and c not in skip and TEXT_LO <= c < TEXT_HI:
                seen.add(c)
                todo.append(c)
    return seen


def read_addrs(path, col=0, flag=None):
    out = []
    for line in open(path):
        p = line.split()
        if p and (flag is None or p[1] == flag):
            out.append(int(p[col], 16))
    return out


def cmd_reach(a):
    import ida_funcs
    seen = closure([a.root])
    with open(os.path.join(a.out, "reach.txt"), "w") as f:
        for x in sorted(seen):
            fn = ida_funcs.get_func(x)
            f.write("%x %d 0\n" % (x, fn.end_ea - fn.start_ea if fn else 0))
    print("reach", len(seen))


def cmd_dtreaders(a):
    import idautils
    import ida_funcs
    reach = set(read_addrs(os.path.join(a.out, "reach.txt")))
    res = {}
    for x in idautils.XrefsTo(DT_ADDR):
        f = ida_funcs.get_func(x.frm)
        if f:
            res.setdefault(f.start_ea, []).append(x.frm)
    with open(os.path.join(a.out, "dt_readers.txt"), "w") as f:
        for ea, refs in sorted(res.items()):
            f.write("%08x %s %d %s\n" % (ea, "IN " if ea in reach else "NOT", len(refs), ida_funcs.get_func_name(ea)))
    print("dt readers", len(res), "not in reach", sum(1 for e in res if e not in reach))


def cmd_decompile(a):
    import ida_hexrays
    reach_f = os.path.join(a.out, "reach.txt")
    reach = set(read_addrs(reach_f)) if os.path.exists(reach_f) else set()
    if a.list == "reach":
        todo = sorted(reach)
    elif a.list == "frontier":
        todo = read_addrs(os.path.join(a.out, "frontier_%s.txt" % a.round))
    else:                                           # "notin": dt readers outside reach
        todo = read_addrs(os.path.join(a.out, "dt_readers.txt"), flag="NOT")
        if a.closure:
            todo = sorted(closure(todo, skip=reach))
    dst = os.path.join(a.out, a.dst)
    os.makedirs(dst, exist_ok=True)
    ok = bad = 0
    for ea in todo:
        try:
            open(os.path.join(dst, "%x.c" % ea), "w").write(str(ida_hexrays.decompile(ea)))
            ok += 1
        except Exception:
            bad += 1
    print("decompiled", ok, "failed", bad)


DECL = re.compile(r"^\s+(unsigned\s+)?(__int\d+|int|float|double|char|bool|_DWORD|_WORD|_BYTE|void|signed|short|long)"
                  r"[\w\s\*\[\]]*\s+\**[A-Za-z_]\w*(\[\d+\])?;\s*//")


def cmd_shard(a):
    src = os.path.join(a.out, a.src)
    dst = os.path.join(a.out, a.dst)
    if a.only == "frontier":
        names = ["%x.c" % x for x in read_addrs(os.path.join(a.out, "frontier_%s.txt" % a.round))]
    elif a.only == "notin":
        names = ["%x.c" % x for x in read_addrs(os.path.join(a.out, "dt_readers.txt"), flag="NOT")]
    else:
        names = sorted(os.listdir(src))
    os.makedirs(os.path.join(dst, "compact"), exist_ok=True)
    files = []
    for n in names:
        lines = [l for l in open(os.path.join(src, n)).read().split("\n") if l.strip() and not DECL.match(l)]
        open(os.path.join(dst, "compact", n), "w").write("\n".join(lines) + "\n")
        files.append((n, len(lines)))
    shards, cur, tot = [], [], 0
    for n, l in files:
        if cur and tot + l > 1300:
            shards.append(cur)
            cur, tot = [], 0
        cur.append((n, l))
        tot += l
    if cur:
        shards.append(cur)
    for i, s in enumerate(shards):
        with open(os.path.join(dst, "shard_%02d.txt" % i), "w") as f:
            for n, l in s:
                f.write("%s 1 %d\n" % (n, l))
    print("functions", len(files), "shards", len(shards))


def data_referenced():
    """Game functions whose address is stored in data (function-pointer tables, callbacks)."""
    import idautils
    import ida_funcs
    import ida_xref
    out = set()
    for ea in idautils.Functions(TEXT_LO, TEXT_HI):
        for x in idautils.XrefsTo(ea, 0):
            if x.type in (ida_xref.dr_O, ida_xref.dr_R, ida_xref.dr_W):
                f = ida_funcs.get_func(x.frm)
                if not f or not (TEXT_LO <= x.frm < TEXT_HI):   # reference comes from a data table, not from code
                    out.add(ea)
                    break
    return out


def cmd_frontier(a):
    """Next audit layer: everything called by, or table-referenced next to, what was reviewed and not yet reviewed."""
    rev_f = os.path.join(a.out, "reviewed.txt")
    if not os.path.exists(rev_f):                    # round 1 + round 3 seeds
        rev = set(read_addrs(os.path.join(a.out, "reach.txt")))
        rev |= set(read_addrs(os.path.join(a.out, "dt_readers.txt"), flag="NOT"))
    else:
        rev = set(read_addrs(rev_f))
    seeds = set()
    import ida_funcs
    for ea in rev:
        f = ida_funcs.get_func(ea)
        if f:
            seeds |= callees(f)
    ptrs = data_referenced()
    seeds |= ptrs
    layer = {x for x in seeds if x not in rev and TEXT_LO <= x < TEXT_HI}
    with open(os.path.join(a.out, "frontier_%s.txt" % a.round), "w") as f:
        for x in sorted(layer):
            fn = ida_funcs.get_func(x)
            f.write("%x %d %s\n" % (x, fn.end_ea - fn.start_ea if fn else 0, "tbl" if x in ptrs else "call"))
    print("reviewed", len(rev), "table-referenced", len(ptrs), "new layer", len(layer))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["reach", "dtreaders", "decompile", "shard", "frontier"])
    p.add_argument("--db", default="orig/LEGOStarWarsSaga_unp.exe", help="exe whose .i64 sits next to it (copied first)")
    p.add_argument("--out", default="orig/audit", help="data directory (gitignored)")
    p.add_argument("--round", default="4", help="frontier: layer name")
    p.add_argument("--root", type=lambda s: int(s, 0), default=0x491D50)
    p.add_argument("--list", choices=["reach", "notin", "frontier"], default="reach")
    p.add_argument("--closure", action="store_true", help="decompile: add callees not in reach.txt")
    p.add_argument("--dst", default="pseudo")
    p.add_argument("--src", default="pseudo")
    p.add_argument("--only", choices=["all", "notin", "frontier"], default="all")
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.command == "shard":
        cmd_shard(a)
        return
    import shutil
    import tempfile
    tmp = tempfile.mkdtemp(prefix="ida_audit_")
    for ext in ("", ".i64"):
        if os.path.exists(a.db + ext):
            shutil.copy(a.db + ext, os.path.join(tmp, os.path.basename(a.db) + ext))
    ida = open_db(os.path.join(tmp, os.path.basename(a.db)))
    {"reach": cmd_reach, "dtreaders": cmd_dtreaders, "decompile": cmd_decompile, "frontier": cmd_frontier}[a.command](a)
    ida.close_database()


if __name__ == "__main__":
    main()
