#!/usr/bin/env python3
"""Group a flat perf profile's self time into the rows report 8 uses (§8.21, §8.24).

Input is `perf report --no-children --sort dso,sym --stdio` output. The groups are regexes over
symbol names, first match wins, so the edges of each group are good to about +-1 point. Kept here
so that the next profile is grouped the same way as the last one; §8.21's own regexes were not
saved and §8.24 re-derived them from the table's row names.

    python3 perfgroup.py sym.txt
"""
import re
import sys

GROUPS = [
    ("interpreter: vertex/triangle/matrix",
     r"GfxSpVertex|GfxSpTri1|tri[12]_handler|quad_handler|GfxSpMatrix|MatrixMul|mtx_handler|calculate_normal_dir"),
    ("resource lookup",
     r"ResourceManager|ArchiveManager|HashToCString|Context::GetInstance|ResourceIdentifier|ResourceMgr_"
     r"|LoadResource|IResource::|ValidateLookupCache|otr_hash_handler|vtx_hash_handler|dl_otr_hash|GetResourceBy"),
    ("PVR user-space mutex", r"PVRSRVLockMutex|PVRSRVUnlockMutex|pthread_mutex_(un)?lock"),
    ("malloc / free", r"\bmalloc\b|\bcfree\b|\bfree\b|calloc|realloc|operator new|operator delete|_int_malloc|_int_free"),
    ("shared_ptr refcount", r"_Sp_counted_base|_M_release|_M_add_ref"),
    ("memcmp / memset / hash / string", r"\bmem(cmp|set|cpy|move)\b|_Hash_bytes|\bstr(len|cmp|ncmp)\b|basic_string"),
    ("frame interpolation", r"interpolate|InterpolateCtx|FrameInterpolation_|anonymous namespace\)::(append|Path|Op|Data)"),
    ("CVar lookup by name", r"CVar|ConsoleVariable"),
    ("GL backend (soh.elf side)", r"GfxRenderingAPIOGL"),
    ("interpreter: everything else", r"Fast::"),
]
DSO_GROUPS = [
    ("DDK kernel module", r"pvrsrvkm"),
    ("kernel", r"kernel"),
    ("libGLESv2, resolved", r"libGLESv2"),
    ("libsrv_um, resolved", r"libsrv_um"),
]

rows = {}
total = 0.0
line_re = re.compile(r"^\s*([\d.]+)%\s+(\S+)\s+\[[.k]\]\s+(.*)$")
for line in open(sys.argv[1]):
    m = line_re.match(line)
    if not m:
        continue
    pct, dso, sym = float(m.group(1)), m.group(2), m.group(3)
    total += pct
    if re.match(r"^0x[0-9a-f]+$", sym.strip()):
        key = "unresolved (stripped DDK and libc addresses)"
    else:
        key = next((g for g, rx in GROUPS if re.search(rx, sym)), None)
        if key is None:
            key = next((g for g, rx in DSO_GROUPS if re.search(rx, dso)), "other")
    rows[key] = rows.get(key, 0.0) + pct

for k, v in sorted(rows.items(), key=lambda kv: -kv[1]):
    print(f"{v:6.2f}%  {k}")
print(f"{total:6.2f}%  total parsed")
