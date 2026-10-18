#!/usr/bin/env python3
# Paired A/B stats from a bench run.log: render-thread CPU, period, window, the window's own CPU
# time (oncpu window_us: the rest of the window is the thread blocked), swap, and the CPU time of
# the end of frame and swap (oncpu eof_cpu_us, §8.36; nan in older logs). Segments pair as (0,1),(2,3)...
# A pair is dropped if either segment left the pin: 1250 MHz, or the clock given as a third argument.
# Pass 1000 to analyse the pairs pyra-thermal capped whole (TESTING.md trap 14): usage run.log <arm A> [MHz]
# main_us (report 11) is thread_us less the BENCH rthread line's replay_us, the replay done on the main
# thread (mode 1): the main thread's CPU if it moved to another thread. Equal to thread_us in a segment that
# recorded nothing, and in mode 2, where the replay is already on the GL thread. gl_us is that thread's CPU
# (0 when there is none) and total_us thread_us + gl_us: the render side's whole CPU per frame.
import re, sys, math
PIN = sys.argv[3] if len(sys.argv) > 3 else '1250'
T = {1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306,9:2.262,10:2.228,11:2.201}
seg = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'BENCH (segment|oncpu|rthread) (\d+) (.*)', line)
    if not m: continue
    i = int(m.group(2)); kv = dict(re.findall(r'(\w+)=([^\s]+)', m.group(3)))
    if i in seg and m.group(1) in seg[i]: continue  # first occurrence only
    seg.setdefault(i, {})[m.group(1)] = kv
def ok(s):
    k = s['segment']
    a, b = k['cpu_mhz'].split('->')
    return all(v == PIN for v in (a, b, k['cpu_min_mhz'], k['cpu_max_mhz']))
arms = {}
pairs = []
for i in range(0, max(seg) + 1, 2):
    if i + 1 not in seg: break
    p = [seg[i], seg[i + 1]]
    bad = [j for j, s in zip((i, i + 1), p) if not ok(s)]
    if bad:
        print(f'pair {i//2}: segments {bad} off the pin, dropped'); continue
    a0 = p[0]['segment']['arm']
    d = {}
    A = p[0] if a0 == p[0]['segment']['arm'] and a0 == sys.argv[2] else p[1]
    B = p[1] if A is p[0] else p[0]
    for name, get in (('thread_us', lambda s: float(s['oncpu']['thread_us'])),
                      ('period_us', lambda s: float(s['segment']['period_us'])),
                      ('window_us', lambda s: float(s['segment']['mean_us'])),
                      ('win_cpu_us', lambda s: float(s['oncpu']['window_us'])),
                      ('swap_us', lambda s: float(s['segment']['swap_us'])),
                      ('eof_cpu_us', lambda s: float(s['oncpu'].get('eof_cpu_us', 'nan'))),
                      ('main_us', lambda s: float(s['oncpu']['thread_us']) - float(s.get('rthread', {}).get('replay_us', 0))),
                      ('gl_us', lambda s: float(s.get('rthread', {}).get('gl_us', 0))),
                      ('total_us', lambda s: float(s['oncpu']['thread_us']) + float(s.get('rthread', {}).get('gl_us', 0)))):
        d[name] = (get(A), get(B))
    pairs.append(d)
    print(f"pair {i//2}: arms A={A['segment']['arm']} B={B['segment']['arm']} temp {A['segment']['temp_c']} / {B['segment']['temp_c']}  "
          + '  '.join(f"{k} {v[1]-v[0]:+.0f}" for k, v in d.items()))
n = len(pairs)
for k in ('thread_us', 'period_us', 'window_us', 'win_cpu_us', 'swap_us', 'eof_cpu_us', 'main_us', 'gl_us', 'total_us'):
    ds = [p[k][1] - p[k][0] for p in pairs]; a = sum(p[k][0] for p in pairs) / n
    mu = sum(ds) / n; sd = math.sqrt(sum((x - mu) ** 2 for x in ds) / (n - 1)) if n > 1 else float('nan')
    se = sd / math.sqrt(n)
    pct = f'{100*mu/a:+.1f}%' if a else 'A=0'  # gl_us in a mode 0 arm
    print(f'{k:10s} n={n} A={a:.0f} delta={mu:+.0f} us ({pct}) t={mu/se:.1f} mde={T.get(n-1, 2.0)*se:.0f}')
