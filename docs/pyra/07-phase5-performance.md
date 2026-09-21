# Report 7 — Phase 5: Performance Measurements

> ## The FPS numbers in this report are superseded. Do not quote them as current.
>
> They remain useful as *ratios* — the resolution sweep in §7.2 and the vsync
> quantisation in §7.4 are structural and still hold. The absolute frame rates do not,
> for two independent reasons:
>
> 1. **The rig was too coarse.** Every number here came from the title-screen attract
>    demo with the process relaunched between arms, which carries a noise floor around
>    ±13% — wider than most of the changes anyone would want to test. Report 9 replaces
>    that rig; anything re-measured should be measured with it.
> 2. **Audio moved off the CPU afterwards.** The Pyra's AESS DSP path now carries
>    playback, so PulseAudio's share of a core is largely gone. The §7.6 profile below
>    was taken before that and its percentages no longer add up — see §7.8.
>
> §7.5's recommended defaults are still the shipping defaults, and §7.5b's correction to
> them still applies, but the frame rate they were chosen against has moved.

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

## 7.5b CORRECTION - the recommended defaults run the game ~8% slow

The defaults above were chosen by maximising frame rate. That was the wrong objective on its own,
because in SoH frame rate and game speed are coupled.

One call to `RunCommands` (`soh/soh/OTRGlobals.cpp:1794-1825`) is one game-logic tick, and it
emits N interpolated sub-frames with N averaging `InterpolationFPS / 20`. Each becomes a rendered
frame and a swap. There is no catch-up if the machine falls behind - the game simply ticks slower.
So:

```
game speed = render FPS / InterpolationFPS
```

With the recommended `InterpolationFPS = 30` and the measured sustained **27.7 FPS**, the game
runs at **0.92x** - about 8% slow, permanently, including audio pitch and any timing-based
gameplay.

| `InterpolationFPS` | Render FPS | Game speed | Trade |
|---|---|---|---|
| 30 (as recommended in 7.5) | ~27.7 | **0.92x** | 8% slow motion |
| 20 | 20.0 | **1.00x** | correct speed, choppier |
| 30 at 0.25x internal res | 37.5, capped to 30 | **1.00x** | correct speed, 320x180 |

The cap is only honoured as a *speed* reference when the hardware can actually reach it. Setting
it above sustained frame rate does not buy smoothness, it buys slow motion.

This was found because the device's owner noticed the game playing in slow motion during a
benchmark run - not by any measurement in this report. Frame rate was measured throughout and
game speed never was.

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

## 7.8 Audio moved to the DSP after this report, and these numbers predate it

Audio playback on the Pyra now runs through the OMAP5 AESS DSP rather than the direct McPDM
path, which is separate work tracked outside this repo. The consequence for everything above is
that the CPU budget the game was competing for got larger between the measurements and now.

What that does and does not change:

- **PulseAudio's share is largely gone.** It was measured at ~15.3% of a core during playback,
  and it was the single largest non-game consumer on the machine. That share was never visible
  in a `perf` profile of the game — it is a different process — so it does not appear in the
  §7.6 table at all, and removing it is a straight gain in headroom that the table cannot show.
- **SoH's own audio cost is untouched.** The ~6% spent in `aEnvMixerImpl`, `aADPCMdecImpl` and
  `aResampleImpl` (`soh/soh/mixer.c`) is the N64 audio microcode being emulated in-process to
  *produce* the samples. The DSP receives those samples; it does not generate them. So report 8
  item 9 — NEON paths for those three routines — is still live and is worth exactly what it was
  worth before.
- **Every FPS figure above is a lower bound on what the same build does now**, by an unknown
  amount. The relationship is not simple: the freed time goes to two Cortex-A15 cores shared
  with the render thread, and the workload was already found to be fragment-bound (§7.3), so
  the gain may be small even though the freed CPU is not.

**Nothing here has been re-measured.** The correct next step is a baseline run on the report 9
harness, not an estimate. Until that exists, treat the FPS column of this report as history.
