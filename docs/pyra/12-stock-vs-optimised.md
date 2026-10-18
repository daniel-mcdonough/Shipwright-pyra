# Report 12 — Stock against optimised: the whole fork in one measurement

Started 2026-10-18 (NEXT.md section 0). Every gain in reports 8 and 11 was measured one change at a
time, against whatever was shipped that day. This report measures the whole fork against a stock
Ship of Harkinian in one design, on the same OS, boot and clock, so that OS-side work (the SGX driver
patch, the undervolt, the thermal daemon, the 750 MHz pin itself) is common to both arms and cancels.

NEXT.md section 0 defines three tiers. **Only Tier 1 is done.** §12.6 says what Tiers 2 and 3 need and
why they were not run in this session.

## 12.1 Headline

**Tier 1 (every fork switch at its upstream value against shipped defaults, one process, ABBA, frozen
fixture, uncapped, 640×360):**

| Clock | Market | Hyrule Field | Kokiri Forest |
|---|---|---|---|
| **750 MHz** (shipped) | 59.0 → 21.4 ms, **−63.8%** (16.9 → 46.8 fps), t −121, MDE 1.4% | 39.9 → 14.2 ms, **−64.5%** (25.1 → 70.6 fps), t −138, MDE 1.2% | 54.5 → 18.2 ms, **−66.6%** (18.4 → 55.0 fps), t −115, MDE 1.5% |
| **1250 MHz** (report 8's clock) | 42.0 → 14.4 ms, **−65.8%** (23.8 → 69.7 fps), t −105, MDE 1.6% | 29.4 → 10.7 ms, **−63.7%** (34.0 → 93.7 fps), t −122, MDE 1.3% | 40.0 → 13.4 ms, **−66.6%** (25.0 → 74.9 fps), t −195, MDE 0.9% |

**With three-point filtering held in both arms** (the code work alone; the filter default is a fidelity
choice and is excluded):

| Clock | Market | Hyrule Field | Kokiri Forest |
|---|---|---|---|
| **750 MHz** | 58.9 → 22.4 ms, **−61.9%** (17.0 → 44.6 fps), t −126 | 39.5 → 18.6 ms, **−52.8%** (25.3 → 53.7 fps), t −129 | 54.5 → 21.1 ms, **−61.3%** (18.3 → 47.4 fps), t −232 |
| **1250 MHz** | 41.7 → 17.6 ms, **−57.8%** (24.0 → 56.8 fps), t −204 | 29.3 → 16.7 ms, **−43.0%** (34.1 → 59.8 fps), t −83 | 39.8 → 17.3 ms, **−56.6%** (25.1 → 57.9 fps), t −282 |

The number is the frame period (`period_us`: start of frame to start of frame, which includes the
game's own work outside the timed window). Every effect is 40-280 standard errors from zero; the MDEs
(0.6-1.6% of the stock period) are two orders of magnitude below the effects, so precision is not the
question here. What the numbers mean is (§12.4).

**What a player gets from this.** The game is capped at 30 fps. Stock settings at the shipped 750 MHz
draw the Market at 17 fps and Kokiri at 18: below the cap, so a stock build at this clock would run
the game visibly slow (frame rate and game speed are one number, §7.5b). The fork draws the same
frames at 47 and 55 fps uncapped, so it holds 30 fps with a third or more of the frame spare. That
headroom, not a higher frame rate, is what let report 11 drop the clock to 750 MHz.

## 12.2 Method

**Data.** Every run's `BENCH` lines are in `docs/pyra/data/12/<tag>.txt` (`t1-` all switches, `t1h-`
three-point held, `v-` the validations, `vis-` the dump runs). `python3 docs/pyra/tools/tier1table.py
docs/pyra/data/12 t1-market-750 ...` rebuilds the tables; `pairs.py <file> stock <MHz>` gives the full
paired breakdown of one run.

**Arms.** Built from the code, not from NEXT.md's table (`grep -rn 'gSettings\.(Lean|ShaderOpts|
RenderThread|TextureFilter)'`; the list matches NEXT.md section 0):

| Switch | Stock arm | Shipped arm |
|---|---|---|
| `ShaderOpts` | 0 | 3 |
| `TextureFilter` | 0 (three-point, upstream's default) | 1 (bilinear); 0 in the three-point-held runs |
| `RenderThread` | 0 | 2 |
| `LeanGLState`, `LeanResLookup` | 0 | 3 |
| `LeanGroup`, `LeanPresent`, `LeanInterp`, `LeanInterpMap`, `LeanGameLookup`, `LeanTexLoad`, `LeanCVar`, `LeanGeom`, `LeanTexReuse`, `LeanCosmetics` | 0 | 1 |
| `LeanSwap` | 0 | 0 (ships off) |

The preset files are `scripts/pyra/presets/stock-vs-shipped.txt` and `stock-vs-shipped-3pt.txt`.
Internal resolution was 0.5× (640×360 in a 1280×720 window) in both arms, from the config.

**Instrument.** The harness's new preset A/B (`SOH_BENCH_AB_PRESET`, `bench.sh preset`, TESTING.md §3):
an arm is a whole set of CVars, each arm change sets the whole line and runs the recompile paths once,
and every touched CVar is restored at the end. Two checks before trusting it, at the Market, 1250 MHz:

| Check | Result |
|---|---|
| Identical arms (`null.txt`) | period +0.9%, t 1.2, MDE 1.9% (269 µs): a null |
| `ShaderOpts` only, preset (`shaderopts.txt`) against the single-CVar `ab gSettings.ShaderOpts 0 3`, same session | GL thread −268 µs (t −32.8) against −286 µs (two clean pairs; the other four were capped to 1000 MHz, where they read −132 µs, t −13.6) |

The ShaderOpts check compares the GL thread, not the period: in `RenderThread` 2 the shader work is on
the GL thread, which does not set the period at the Market. So §8.20's −4.85% period (taken in mode 0)
is not the comparison; the single-CVar A/B in the same session is. Both instruments agree. The config
file was checked after the first run: the four keys it had held (`LeanGLState`, `LeanInterp`,
`LeanSwap`, `RenderThread`) were back and nothing else was left behind.

**Runs.** 6 pairs per run, ABBA, 600 measured frames per segment plus a 180-frame grace after each arm
change (which covers the shader recompile and texture re-upload that a `TextureFilter` change causes).
CPU pinned by the userspace governor at 750 or 1250 MHz. Soaked: `SOH_BENCH_SOAK_C=70` at 1250 MHz
and 58 at 750 (the die does not reach 70 there; it sat at 63-70 °C), warm-up floor 2400 frames, and
the die cooled to 55 °C before every run. Every segment read `window=1280x720` and
`fb=640x360->640x360`. **Discarded:** 3 pairs of 72, all at 1250 MHz, each the run's last pair,
capped to 1000 MHz by `pyra-thermal` at 82 °C: Kokiri, Market three-point held, Kokiri three-point
held. None at 750 MHz. Device on its charger, nobody using it, USB ethernet.

## 12.3 Where the time goes

Render-side CPU (main thread plus the GL thread, `pairs.py`'s `total_us`) falls 35-44% in every run;
the period falls 43-67%. The rest of the period gain is waiting removed, two ways:

- **The swap.** Stock's main thread spends 13.0 ms of a 42 ms Market frame at 1250 MHz blocked in
  `SDL_GL_SwapWindow`, waiting on the GPU (three-point is fragment ALU). Shipped spends ~5.2 ms there,
  on the GL thread rather than the main one (approximate in mode 2, TESTING trap 24).
- **The second core.** In mode 2 the GL work (3.4-5.8 ms per frame) runs beside the main thread
  instead of in series with it. That is report 11's design; it costs ~10% more total CPU (§11.10) and
  this table counts that cost against it (`total_us` includes the GL thread).

**The filter's share depends on the clock and the scene** (read across runs, so these are not paired
numbers; the per-run MDEs are ~1%):

| | Market | Hyrule Field | Kokiri |
|---|---|---|---|
| shipped code, three-point → bilinear, 1250 MHz | 17.6 → 14.4 ms, −19% | 16.7 → 10.7 ms, −36% | 17.3 → 13.4 ms, −23% |
| the same, 750 MHz | 22.4 → 21.4 ms, −5% | 18.6 → 14.2 ms, −24% | 21.1 → 18.2 ms, −14% |

At 750 MHz the main thread is the limit at the Market and the GPU's fragment cost hides under it; at
1250 the CPU work is short enough that the GPU sets the period. **Hyrule Field is the most
fragment-bound scene once the CPU work is gone**, the reverse of report 8 §8.18's "not fragment-bound"
(already retracted by TESTING trap 10 as a frame-cap artifact; this is the first measurement that says
how far wrong it was).

## 12.4 What these numbers are and are not

- **A frozen frame, not the game.** The fixture says a frame got cheaper to draw. It cannot see lazy
  asset loads, scene changes or game logic (`Play_Update` is skipped). Tier 1 in real play
  (watch mode, the owner playing, 30/750) is the confirmation and has not been run (§12.6).
- **Uncapped.** The shipped game runs capped at 30 fps; the fps above are what the frame would allow,
  not what anyone sees.
- **The stock arm is the tip with its switches off, not a stock binary.** Code that changed without a
  switch is in both arms and invisible here. A code audit (§12.5) found nothing large in that list.
  Tier 2 is the measurement that would see it.
- **Mode 0 is reached by switching in-process.** The GL context moves back from the GL thread at the
  arm change (`RenderThread: GL context moved` in the log at each switch), and the recording proxy
  stays in the call path, forwarding every call (report 11). Report 11 counts mode 0 as the
  unmodified path.
- **Both arms are benchmark builds.** `BENCH_GL_CALL` counts every GL call and mode 0 issues more of
  them, and a few counters sit in one arm's code path only. This biases slightly against the stock
  arm. Not priced; small against a 60% effect, but the effect is not release-exact.
- **Not chained, not added.** The per-change percentages in reports 8 and 11 do not sum to this, and
  this does not split into them. Each was measured against a different baseline.

**It contradicts the README.** The README's only cumulative figure was a three-link chain at the
Market, 1250 MHz: 33.4 → 23.8 ms (−29%), covering three changes with bilinear already on and
predating the GL thread and the later Lean changes. Measured whole, the same scene and clock is
42.0 → 14.4 ms (−66%), or 41.7 → 17.6 ms (−58%) with the filter held. The chain understated the fork
by about half, as its own caveat said it might. The README's render-thread row (~34 → 47 fps at the
Market, 750 MHz) agrees with this report's shipped arm (46.8 fps), from a separate run.

## 12.5 The code with no switch (NEXT.md section 0, item 2)

An audit of `develop..feature/pyra-port` and libultraship `fdcaf633..HEAD` for per-frame or per-tick
code that changed without a switch. These are in both Tier 1 arms; Tier 2 would see them.

| Change | Per-frame effect | In the Tier 2 stock build? |
|---|---|---|
| The `GfxRenderingAPIThreaded` proxy is always in the call path (mode 0 forwards ~600 calls per frame, 3 CVar reads) | tiny, unmeasured | no |
| `LeanPresent=0` is **not upstream-equivalent**: `Gui::DrawGame` adds the two present callbacks whatever the value (a bind, two `glTexParameteri`, a rerun of ImGui's render-state setup, ~20 GL calls) | small, unmeasured | no |
| The switches' own CVar reads, ~15-20 per frame | ~10-15 µs at 750 MHz | no |
| `glDiscardFramebufferEXT` of depth/stencil twice per frame | measured at no gain (§8.10) | no |
| Swap path restructured (`SwapPending`/`SwapNow`), `CVAR_Z_FIGHTING_MODE` read per frame not per decal change, `aspect` divide hoisted, ConsoleVariable bookkeeping on write, cache-generation atomics, the notification queue's mutex, the shutdown-signal check, the `LUS_FPS_LOG` check | ≈0 | no |
| The GLES2 port, the NEON audio mixer, the Pyra first-run defaults | needed to run / settings | **yes**, so they cancel in Tier 2 |

Off paths that are restructured but equivalent: `LeanInterp` 0 (the original tree, in a struct, two
bit tests per recorded op), `LeanGeom` 0 (a branch per colour component), `LeanCVar` 0 (same key, same
hash). Every other switch's 0 runs upstream's code. Two are not read per frame: `TextureFilter` (once,
at init) and `ShaderOpts` (per shader compile); the harness's recompile path handles both.

None of this can make a large Tier 2 − Tier 1 gap. If Tier 2 comes out far from Tier 1, look first at
the `TextureFilter` default (stock three-point, tip bilinear; the first-run defaults do not set it)
and at benchmark-counter work present in one build only.

## 12.6 Not done: real play, Tiers 2 and 3

- **Tier 1 in real play** needs the owner holding the device: watch mode with the preset
  (`PYRA_WATCH_PIN=1 PYRA_PIN_FREQ=750000 bench.sh watch start SOH_BENCH_ENTRANCE=0x00B1 SOH_BENCH_UNCAP=0
  SOH_BENCH_AB_PRESET=scripts/pyra/presets/stock-vs-shipped.txt`), 50+ pairs, `slowpairs.py` and
  `watchpairs.py`. Watch out for one thing a single CVar did not have: a `TextureFilter` change
  re-uploads every texture and recompiles every shader, so each arm change will hitch. The first
  10 frames of a block are skipped (`kWatchSkipFrames`); check that this covers it, or hold the filter
  in both arms for the real-play run.
- **Tier 2 (a stock binary) is a build-engineering job.** The harness's counters live in ~25 files
  across libultraship's interpreter, the GL backend and frame interpolation, many inside the switches'
  code paths. Porting the harness to `f1822a4af` + libultraship `cdf0d7e6` either carries those
  counters (and the code they sit in) or strips them, in which case the tip must be built without them
  too, or the counter work biases the comparison. NEXT.md says to stop and say so if porting the
  harness drags optimisation code in; it would. The clean design is a counter-free "lite" harness
  (fixture, pin, soak, period, `oncpu`) built into both a stock and a tip binary, each with its own
  `soh.o2r` and install directory, `TextureFilter` written explicitly into both configs. Then the
  A-vs-A launch null at each clock first.
- **Tier 3** depends on Tier 2's stock build.

## 12.7 Visual check

In-process frame dumps (`SOH_BENCH_DUMP`, frames 301 and 302 of every segment) of the three-point-held
arms at 1250 MHz, one process each, 6 pairs. Each comparison takes the closer of the two frame phases.

| Fixture | Within stock | Within shipped | Stock vs shipped |
|---|---|---|---|
| Market | 0 px | 0 px | 34 px, every one ±1 level, in all 6 pairs |
| Hyrule Field (rows 216 and up masked: the sky scrolls, ~65k px within either arm unmasked) | 0 px | 0 px | 29 px, every one ±1 level, in all 6 pairs |

Both arms draw the same picture. The ±1 pixels are `LeanGroup`'s depth ties (§8.41: 26 px at ±1 at
Hyrule Field), the one known inexact switch.
