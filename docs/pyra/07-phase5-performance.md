# Report 7 — Phase 5: Performance Measurements

All measured on the device at **thermal equilibrium** (73–79 °C), title-screen attract demo,
armv7ve build with `LUS_FPS_LOG`. Methodology matters here far more than expected — see §7.1.

## 7.1 Two measurement traps found first

**Thermal state moves frame rate by ~30%.** Cold, the SoC runs at 1.5 GHz; under sustained load
the Pyra's thermal cpufreq policy walks it down through 1.4 / 1.25 / 1.125 / 1.0 / 0.875 / 0.75 GHz.
Within a single 50-second run, FPS fell 20.1 → 13.8 as the die went 56 → 68 °C. Any A/B taken
without a warm-up measures the thermostat, not the code. Every number below is post-warm-up.

**The game ships capped at 20 FPS.** `CVAR_SETTING("InterpolationFPS")` defaults to **20**
(`soh/soh/OTRGlobals.cpp:994-1002`), enforced by `SyncFramerateWithTime()`
(`libultraship/src/fast/backends/gfx_sdl2.cpp:648`). OoT renders natively at 20 FPS and SoH
interpolates above that. Frame times pinned at exactly `50.00 ms` **with vsync off** is the
signature. Until this is raised, any configuration fast enough to reach 20 FPS reports exactly
20.00 and hides all remaining headroom.

## 7.2 Resolution sweep — the decisive result

Vsync off, cap raised, at equilibrium:

| Internal resolution | Cap | Median FPS |
|---|---|---|
| 1.0× (1280×720) | 30 | **16.4** — genuinely hardware-limited |
| 0.5× (640×360) | 30 | **26.7** |
| 0.5× (640×360) | 60 | **28.7** |
| 0.25× (320×180) | 60 | **37.5** |

Scaling from 1.0× to 0.5× gains **+75%**; 0.5× to 0.25× gains a further **+31%**. Resolution is
by far the strongest lever available.

**At 640×360 — the internal resolution named in the original brief — the port sustains ~29 FPS**,
which clears the 20 FPS minimum comfortably and is at the edge of the 30 FPS preferred target.

## 7.3 Correction to report 5 §5.2-5.3

Report 5 concluded from synthetic benchmarks that fill rate had ~15× headroom and that
"the bottleneck is expected to be CPU-side, not GPU", and on that basis recommended raising the
internal resolution toward native 720p and deprioritising fragment-cost work.

**That was wrong.** The real workload is strongly resolution-sensitive: +75% from halving it.
The synthetic test used one textured, blended quad with no state changes; the N64 workload has
far more overdraw, texture binds, small triangles and state churn per pixel, and the
tile-based deferred architecture behaves very differently under it. The synthetic number was an
upper bound on a best case, not a prediction.

The CPU-side findings in report 5 still hold as far as they go — a `perf` profile of the running
game shows no single hotspot — but the ordering was backwards. Fragment/fill cost matters here,
and internal resolution should be the *first* setting tuned, not the last.

## 7.4 Vsync interacts badly with this frame rate

Vsync quantises presentation to 60/N — 60, 30, 20, 15 FPS. At ~35 ms/frame (28.7 FPS) the frame
just misses the 33.3 ms two-interval slot and falls to the three-interval slot at 50 ms, i.e.
**vsync on turns 29 FPS into 20 FPS**. Measured directly: 0.5× with vsync on reported exactly
20.00 FPS / 50.00 ms.

Vsync should therefore be **off** by default on this device, accepting tearing, unless the frame
rate can be held reliably under 33.3 ms.

## 7.5 Recommended Pyra defaults

| Setting | Value | Why |
|---|---|---|
| `InternalResolution` | **0.5** (640×360 on the 1280×720 panel) | +75% over native; the brief's stated target |
| `InterpolationFPS` | **30** | the default 20 caps the device below its ability |
| `VsyncEnabled` | **0** | otherwise 29 FPS quantises down to 20 (§7.4) |
| `MSAAValue` | **1** | no multisampling on this GLES2 path anyway |

Expected result: **~27–29 FPS sustained at thermal equilibrium**, versus the 17 FPS seen at
native resolution with the stock 20 FPS cap.

### Implemented

`ApplyPyraDefaults()` in `soh/soh/OTRGlobals.cpp`, called once right after
`InitConsoleVariables()` and compiled in only when the `PYRA` preset defines it.

Each value is written **only if the key is absent from the config**, so these are a starting
point rather than a lock: anything changed in the settings menu persists and is never
overwritten on a later launch. The block ends with `CVarSave()` so the values land on disk on
the first run instead of waiting for a clean shutdown — they are then visible in the menu, and
they survive a crash or a kill.

Verified on the device with a fresh config directory:

```
InternalResolution   = 0.5
InterpolationFPS     = 30
VsyncEnabled         = 0
MSAAValue            = 1

FPS, fresh install, no manual tuning, 73.8 C:
27.47, 27.01, 29.04, 28.98, 27.40, 27.38, 27.16, 26.95
```

So an untouched install now lands at **~27 FPS sustained** where it previously landed at 17.

## 7.6 CPU profile (for later work)

`perf record` on the running game, no dominant hotspot — the cost is spread:

| Bucket | Share |
|---|---|
| shared_ptr refcounting, resource-cache lookups, malloc/free, hashing | ~13% |
| Fast3D interpreter (`GfxSpVertex`, `GfxSpTri1`, `Interpreter::Run`) | ~11% |
| PowerVR driver + kernel (`_SegmentListInsert`, PVR mutexes, spinlocks) | ~8.5% |
| Audio (`aEnvMixerImpl`, `aADPCMdecImpl`, `aResampleImpl` — all scalar) | ~6% |
| `__aeabi_uidiv` software division | ~1.6% |

Notes:
- The NEON mixer (`aMixImpl`) does **not** appear, confirming the armv7 NEON fix works. The three
  audio routines that do appear have no NEON path at all — a possible future win.
- `__aeabi_uidiv` is addressed by the `armv7ve` toolchain change (hardware `udiv`/`sdiv`); the
  binary now contains 591 hardware divide instructions where it had none. The effect is ~1-2%,
  which is below the noise floor of a 19 FPS measurement, so it is justified on principle rather
  than by a measured delta.
- The resource-cache lookup cost is partly the upstream revert of
  "Improve ResourceManager cache lookup performance" (#989, reverted in #1028 because it
  regressed texture packs). Not a free win to re-apply.

## 7.7 A data-loss bug found along the way

spdlog's file sink fails repeatedly — `Failed flush to file ./logs/Ship of Harkinian.log:
Bad file descriptor`, **37 times in one run** — despite `logs/` existing and being writable.
Its writes then land in a reused descriptor. Observed outcome: a log line written *into* a save
file, corrupting it:

```
"vibrato": 0,      [08:27:52.520] [SaveManager.cpp:1132] [info] Save File - fileNum: 0
"vol...
```

This reproduced in a solo run, so it is not purely a concurrency artefact. It is a data-loss
class bug and should be diagnosed before the port is used for real play. Not yet investigated.
