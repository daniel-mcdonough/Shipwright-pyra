#!/usr/bin/env python3
"""Symbolize and aggregate the benchmark's per-segment call-site tables (report 8 §8.31).

    python3 docs/pyra/tools/sites.py build-pyra/soh/soh.elf alloc-sites-0.txt [more.txt ...]

Reads alloc-sites-N.txt (scripts/pyra/allocshim.so) or lookup-sites-N.txt (the resource manager),
both written under SOH_BENCH_SITES. Offsets inside soh.elf are named with llvm-symbolizer against
the symbol table of the binary that ran (release builds have no line tables, so function names
only); other DSOs are shown as basename+offset. Several files are averaged, so pass segments of
one arm together. Prints per-frame counts by caller function, largest first.

The recorded address is a return address. On Thumb it carries bit 0, and it points after the call,
so the symbolized address is (offset & ~1) - 2: inside the calling instruction.
"""
import collections
import os
import subprocess
import sys


def parse(path):
    rows = []
    for line in open(path):
        if line.startswith("#"):
            continue
        f = line.split()
        if not f:
            continue
        if os.path.basename(path).startswith("alloc"):
            count, nbytes, dso, off = float(f[0]), float(f[1]), f[2], int(f[3], 16)
            rows.append((count, nbytes, "", dso, off))
        else:
            count, entry, dso, off = float(f[0]), f[1], f[2], int(f[3], 16)
            rows.append((count, 0.0, entry, dso, off))
    return rows


def symbolize(elf, offsets):
    if not offsets:
        return {}
    addrs = [hex((o & ~1) - 2) for o in offsets]
    out = subprocess.run(["llvm-symbolizer", "--obj=" + elf, "--functions=linkage", "--demangle", "--no-inlines"]
                         + addrs, capture_output=True, text=True).stdout
    blocks = [b for b in out.strip().split("\n\n")]
    names = {}
    for o, b in zip(offsets, blocks):
        names[o] = b.splitlines()[0] if b else "??"
    return names


def main():
    elf, files = sys.argv[1], sys.argv[2:]
    per_file = [parse(p) for p in files]
    soh = sorted({r[4] for rows in per_file for r in rows if r[3].endswith("soh.elf")})
    names = symbolize(elf, soh)
    agg = collections.defaultdict(lambda: [0.0, 0.0, set()])
    for rows in per_file:
        for count, nbytes, entry, dso, off in rows:
            if dso.endswith("soh.elf"):
                who = names.get(off, "??")
            else:
                who = "%s+0x%x" % (os.path.basename(dso), off)
            key = (entry, who)
            agg[key][0] += count / len(per_file)
            agg[key][1] += nbytes / len(per_file)
            agg[key][2].add(off)
    total = sum(v[0] for v in agg.values())
    print("# %.1f per frame over %d file(s)" % (total, len(files)))
    for (entry, who), (count, nbytes, offs) in sorted(agg.items(), key=lambda kv: -kv[1][0]):
        if count < 0.05:
            continue
        # C++ signatures run long; the function name is what identifies the caller.
        short = who if len(who) < 150 else who[:147] + "..."
        extra = ("  %.0f B" % nbytes) if nbytes else ""
        print("%8.1f  %5.1f%%  %s%s  [%d site%s]%s" % (count, 100 * count / total if total else 0, entry + "  " if entry else "",
                                                       short, len(offs), "" if len(offs) == 1 else "s", extra))


if __name__ == "__main__":
    main()
