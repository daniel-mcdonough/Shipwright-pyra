# Report 11 — A GL thread: splitting the render side across two cores

Branch `feature/pyra-two-core` (libultraship `feature/gles2-pyra-two-core`). Started 2026-09-27, when the
owner chose NEXT.md 0T (3) over settling on 30/875 or 20/625 (PROMPT task 1, after §8.48).

This report is the design first and the measurements after it. The design is written before any code
because this is the one change in the project with real design decisions in it — which thread owns the
GL context, what crosses between the threads, and which calls need an answer back — and getting one of
them wrong shows up as a hang or a wrong picture, not as a number.

## 11.1 The goal and the budget

**Goal:** 30 fps at 750 MHz that holds at the Market and on Death Mountain Trail in real play. The bar is
the 1000 MHz miss rate: ~1.9% of frames over 40 ms at the Market (§8.46, §8.48). Today 30/750 misses
3.5-4% there and 3.2-3.6% under the falling rocks; the other scenes already hold (~1%).

**Why it only pays through the clock.** Splitting work across threads adds work (the hand-off) and
removes none. At a fixed clock it costs heat. It is worth building only if it lets the clock drop: 30/750
against 30/1000 was −0.34 W and −2.7 °C (§8.44), before this design's overhead. For comparison, 20/625
already holds real play at −0.81 W (§8.45). That is the owner's call and it has been made; the number is
here so the result is judged against it.

**What can move off the main thread** (Market, frozen, 1000 MHz, from §8.36, §8.41 and §8.43):

| | µs per frame | source |
|---|---|---|
| `glDrawArrays` and per-draw GL, 262 → 145 draws after LeanGroup | ~3,500 | §8.36's ~4,390 less §8.41's −840 |
| end of frame: discard, flush, the swap's CPU | ~285 | §8.36 `eof_cpu_us` |
| ImGui's GL draw (`ImGui_ImplOpenGL3_RenderDrawData`) | ≤200 | §8.43 `EndDraw` 200 is an upper bound |
| **CPU that moves** | **~3,800-4,000** | ~24% of the main thread's ~15.9 ms |
| the swap's wait (uncapped 5.1-5.4 ms; slow real-play frames 7-19 ms) | wait, not work | §8.43, §8.46 |

At 750 MHz the CPU part is ~5.1-5.3 ms (×4/3, an upper bound: memory stalls do not scale with the
clock). NEXT.md 0T (5) put the Market's need at ~5 ms off its worst ticks. **The ceiling is about the
size of the gap, with no margin**, and the hand-off's own cost comes out of it.

The wait may matter more than the work. A slow real-play frame is 18-30 ms of render-thread CPU, 7-19 ms
of swap and ~20 ms off-CPU (§8.43). A GL thread takes the swap's wait off the main thread entirely, and
the GL third of the render CPU with it. The off-CPU part (preemption, ~760/s at 750, §8.47) stays.

**What cannot move:** the interpreter (display-list decode, T&L, clipping; ~9.6 ms of window CPU at the
Market, §8.36), Play_Draw and the recording (1.3 ms), interpolation, input, ImGui's frame building. The
main thread after the split is ~12 ms at 1000 MHz, ~16 ms at 750, against a 33.3 ms frame.

**The trap this has to avoid:** LeanSwap (§8.46) also took the swap's wait off the main thread's path, and
it made real play *worse* (>40 ms frames 0.97% → 1.54%, render CPU +832 µs, t 2.22, MDE 788). Nobody
found out why. The t is marginal, so it may be noise, but a design whose main benefit is the same
overlap must not repeat it blind. Stage 2 below measures real play before anything else is tuned.

## 11.2 Where to cut: the `GfxRenderingAPI` interface

libultraship's interpreter already talks to GL through one abstract interface,
`Fast::GfxRenderingAPI` (`include/fast/backends/gfx_rendering_api.h`), implemented for GLES2 by
`GfxRenderingAPIOGL`. Everything the interpreter wants from GL — state, textures, draws, framebuffers,
the frame's start and end — is a call on it. The GUI loads its textures through the same object.

The design is a **proxy**, `GfxRenderingAPIThreaded`, that implements the interface and wraps the real
backend. On the main thread it turns each call into a record in a command stream. A GL thread replays the
stream into the real backend, which is otherwise unchanged. The interpreter does not change.

Why here and not elsewhere:

- **Not a logic/render split** (§8.33): logic is 15-20% of the main thread; it buys ~10% uncapped and
  nothing at the cap. It also needs a game-state snapshot. The render side is where the time is.
- **Not inside the GLES2 backend.** The backend already defers draws for LeanGroup, and recording there
  would reuse that arena. But the backend's state-shadowing logic would then be split across two
  threads. At the interface, the backend runs whole on one thread and every existing Lean* mode keeps
  working unmodified.
- **Not at the GL call level** (a GL command recorder). The backend makes ~690 GL calls a frame besides
  draws (§8.36), and the interface carries far fewer, larger calls.

## 11.3 Every interface call, and how it crosses

Most calls return nothing and are simply queued. The hard ones are those that return something the
main thread uses immediately.

| Call | Crossing | Notes |
|---|---|---|
| `LoadShader`, `UnloadShader`, `SelectTexture`, `SetSamplerParameters`, `SetDepthTestAndMask`, `SetZmodeDecal`, `SetViewport`, `SetScissor`, `SetUseAlpha`, `StartFrame`, `EndFrame`, `FinishRender`, `UpdateFramebufferParameters`, `StartDrawToFramebuffer`, `CopyFramebuffer`, `ClearFramebuffer`, `DiscardDepthStencil`, `FlushDeferredDraws`, `ResolveMSAAColorBuffer`, `SelectTextureFb`, `DeleteTexture`, `SetSrgbMode`, `OnResize` | queued | Arguments by value. `ShaderProgram*` is stable: programs live in the backend's pool until `ClearShaderCache`, which is itself queued in order. |
| `DrawTriangles` | queued, **vertices copied** | The interpreter reuses its buffer after the call. The main cost of the hand-off (11.5). |
| `UploadTexture` | queued, **pixels copied** | Rare after a scene loads (LeanTexLoad, §8.31). |
| `ClearShaderCache` | queued, and the proxy empties its shader map | |
| `SetTextureFilter` | **synchronous**, and the proxy keeps a shadow for `GetTextureFilter` | The GL backend's also clears the interpreter's texture cache, which is main-thread state. A settings change, rare. |
| `GetName`, `GetMaxTextureSize` | answered by the proxy | Constant after `Init`; cached then. |
| `GetClipParameters` | answered by the proxy from a shadow | **Not constant** (11.8): the flip of the framebuffer being drawn to. Shadowed from its only two writers, `UpdateFramebufferParameters` and `StartDrawToFramebuffer`; checked in mode 3. |
| `LookupShader` | answered by the proxy | The proxy's own map of `(id0, id1) → ShaderProgram*`, filled by `CreateAndLoadNewShader`. |
| `ShaderGetInfo` | answered by the proxy | Reads fields the backend writes once, when it creates the program, which happened in a synchronous call. |
| `IsTextureBound` | answered by the proxy from a shadow | See below. |
| `GetFramebufferTextureId` | answered by the proxy | A framebuffer's colour texture is created in `CreateFramebuffer` and never replaced (`UpdateFramebufferParameters` resizes it in place), so it is cached then. Called every frame by the GUI; a round-trip here would serialise the threads. |
| `GetTextureById` | answered by the proxy | The GL backend's is a pure cast. |
| `GetPixelDepth` | **GLES2: answered by the proxy (zeros)**; the side effects queued | On GLES the backend compiles the `glReadPixels` out and returns zeros for every coordinate (`gfx_opengl.cpp`, "not supported on gles"). The sun's lens flare (`z_kankyo.c`) and `z_lights.c` have always received 0 on the Pyra. Answering 0 is exact. Desktop GL: synchronous. |
| `NewTexture` | from a pool of names | The main thread uses the id at once (select, upload). The GL thread keeps a pool of names from the backend's own `NewTexture` (which also sizes its texture table), topped up as it drains. An empty pool falls back to a synchronous call. |
| `CreateAndLoadNewShader`, `CreateFramebuffer`, `ReadFramebufferToCPU`, `Init` | **synchronous** | The main thread waits for the GL thread to drain, then the call runs there. Shaders compile once per variant; framebuffers are made at start; `G_READFB` is rare. All three stall, all three are rare. |

**`IsTextureBound` (LeanTexReuse, §8.37).** The interpreter asks "is this texture bound on this tile?"
and skips a reload and a flush if so. In the grouped mode (LeanGroup=1, the GLES2 default) the backend's
answer depends only on the last `SelectTexture` per tile (`mCurrentTextureIds`), which the proxy sees in
the same order and can shadow exactly (`SelectTextureFb` selects the framebuffer's colour texture on tile
0 through the same function, so it updates the shadow too; nothing else writes it). When grouping is off, the backend's answer also depends on its real
bindings, which the proxy cannot see. There the proxy answers **false**, which is always safe: the
interpreter then binds as it did before §8.37. A wrong *true* would draw with the wrong texture, so the
equivalence mode (11.6) records every true answer and checks it against the backend's own at replay.

## 11.4 Threads, the context, and the stream

**The GL thread owns the context in the threaded mode.** The context is created on the main thread by
SDL (`gfx_sdl2.cpp`). On entering the mode, the main thread releases it (`SDL_GL_MakeCurrent(wnd,
nullptr)`) and the GL thread makes it current. Leaving the mode reverses it. SDL events, window
management and everything else in SDL stay on the main thread. Whether the PVR DDK's EGL accepts a
context moving between threads is the first thing stage 2 tests, before anything else is built on it.

**The swap moves with the context.** `SwapBuffersBegin` (the frame-cap sleep and `SDL_GL_SwapWindow`)
becomes a queued command. The main thread is held back by the stream: it may run at most one frame ahead
of the GL thread. When it finishes recording frame N+2 and frame N's swap has not completed, it waits.

**The stream is chunked, not per-frame.** Commands go into fixed-size blocks (64 KB to start). A block is
published to the GL thread when it fills and at each flush point (`FlushDeferredDraws`, `EndFrame`, any
synchronous call), so the GL thread starts on frame N while the interpreter is still producing it. A
per-frame buffer would be simpler but would add a full frame of latency and give no overlap within a
frame. A payload larger than a block (a large texture upload) gets its own allocation, which the GL thread
frees after replay. Blocks are recycled through a free list; nothing is allocated per frame at steady
state.

**Latency.** When the GL thread keeps up, a frame reaches the screen later by the GL thread's lag behind
the interpreter, a few ms. With one frame in flight the worst case is one frame (33 ms at 30 fps). Input
is sampled in logic, before either thread sees the frame.

**Core placement.** Neither thread is pinned: pinning the main thread alone cost ~3.2 ms of period
(§8.47). Core 1 carries the audio stack (~5 ms of core time per frame at 750, §8.47) and all device IRQs
are on cpu0. The scheduler places the GL thread. If it pays, pinning is a later experiment with its own A/B.

**Where the thread boundary leaks today, and what each needs:**

- **CVars read by the backend on the GL thread.** `StartFrame` reads LeanGLState, LeanGroup and
  ProbeSkipDraw; `DrawTriangles` reads the Z-fighting mode per draw; `SwapBuffersBegin` reads vsync;
  `CreateAndLoadNewShader` reads ShaderOpts. The CVar store is not thread-safe. The proxy reads these on
  the main thread at `StartFrame` and passes them in the `StartFrame` record; the backend takes them from
  there instead of the store. `CreateAndLoadNewShader` is synchronous, so the main thread is waiting and
  its read is safe as it is.
- **ImGui.** `Gui::EndFrame` calls `ImGui::Render()` and then draws with `ImGui_ImplOpenGL3_RenderDrawData`.
  In the threaded mode the draw data is deep-copied (each `ImDrawList` cloned) into a queued command and
  drawn on the GL thread. `ImGui_ImplOpenGL3_NewFrame`, which creates GL objects on first use and when
  the font atlas is rebuilt, becomes a queued command too. The present callback (`PresentStateCallback`,
  LeanPresent) runs inside `RenderDrawData` and so moves with it. ImGui's multi-viewport path
  (`RenderPlatformWindowsDefault`, `SDL_GL_MakeCurrent`) is off on the Pyra; the threaded mode refuses
  to start if it is enabled.
- **The benchmark harness.** Counters the backend increments (`BENCH_COUNT`: draws, uploads, ...) and the
  dump paths (`gBenchDumpPath`) are globals written on one thread and read or reset on the other. In the
  threaded mode the GL thread keeps its own counters and adds them into the frame's at its `EndFrame`,
  under the frame hand-off's own synchronisation, and the dump path travels in the `EndFrame` record.
  Counts are then one frame late, which does not matter for averages over a segment. `thread_us` stays
  the main thread; a new `gl_thread_us` field carries the GL thread's CPU time per frame, and their sum
  is the render side's total. Swap time is measured on the GL thread.

## 11.5 The hand-off's cost, estimated

*Reasoning, not measured; stage 1 measures it.* Per Market frame: ~1,664 drawn triangles (§8.26's geom
line: 4,333 submitted, 34% clip-rejected, 28% culled), three vertices each at 10-20 floats is 200-400 KB
copied, ~0.2-0.4 ms at the A15's copy bandwidth. The draw records themselves (~145-262 calls a frame, a
few hundred state calls) are ~16-32 bytes each, noise. The grouped mode then copies groupable draws a
second time into its arena on the GL thread, which is off the main thread's path. The synchronisation is
a handful of atomic operations and at most one wake-up per block.

If the copy costs more than ~1 ms at 1000 MHz, the fix is to let the interpreter write its vertex buffer
directly into the stream (stage 3) rather than copy it afterwards.

## 11.6 The stages, and what each must show before the next

`gSettings.RenderThread`, read on the main thread at each frame start. Changing it takes effect at the
next frame boundary, after the stream has drained, so the harness can A/B it in-process like any other
CVar:

- **0** — direct: the proxy forwards every call as it arrives. The default until stage 2 is proven.
- **1** — record, and replay inline on the same thread at each flush point. No second thread.
- **2** — record, and replay on the GL thread.
- **3** (benchmark builds) — mode 1 with the checks: every proxy-answered query (`IsTextureBound`, the
  shader lookups) re-asked of the backend at replay, with `mismatch` counters on a `BENCH rthread` line,
  and a positive control (`gSettings.ProbeRThreadCorrupt`) that answers one query wrongly on purpose.

**Stage 1: record and replay, one thread (modes 1 and 3).** Everything except concurrency: the command
format, every crossing in 11.3, the proxy's answers. It needs no device to build and run.
- Equivalence in motion (`SOH_BENCH_STATIC=0`, Market): mode 3, 0 mismatches; the control fires.
- Picture: `SOH_BENCH_DUMP` at frames n and n+1, mode 1 against mode 0, in-process, Market and Hyrule
  Field (sky masked): 0 pixels.
- Price: frozen A/B 0 → 1, Market and Hyrule Field, 6 pairs each, soaked, 1000 MHz. Mode 1 does the
  same GL work plus the recording, so Δ render-thread CPU *is* the hand-off's cost. A new `replay_us`
  field is the CPU that would move to the GL thread. Their difference is stage 2's ceiling, measured.
- **Gate:** the cost is under ~1 ms at the Market and `replay_us` minus the cost is at least ~3 ms at
  1000 MHz. If not, stage 3's zero-copy comes before stage 2, or the design stops here.

**Stage 2: the GL thread (mode 2).**
- First, alone: the context moves to a second thread and back on the device, drawing correctly, 100
  times. If the DDK refuses, the design stops here.
- Then the thread, the CVar snapshot, ImGui's copy, the swap and the harness fields.
- Frozen A/B 0 → 2 at 750 MHz, Market and Hyrule Field: main-thread CPU and period, `gl_thread_us`, total.
- Real play before any tuning, the owner playing: `watch` at 30/750, `PYRA_WATCH_PIN=1`, mode 0 against 2,
  Market and Death Mountain Trail first. Frames over 40 ms per arm; the LeanSwap failure (11.1) is the
  thing to rule out.
- **Gate:** the Market and Death Mountain Trail at 30/750 at or under the 1000 MHz miss rate (~1.9%).

**Stage 3, only if a gate asks for it:** the interpreter writes its vertices straight into the stream.

**Then** `thermcost.sh clock`, 30/750 threaded against 30/1000 and 20/625, on battery: PROMPT task 2.

**Stop conditions:** the DDK will not move the context; stage 1's cost leaves under ~2 ms of net gain at
1000 MHz; or real play at 30/750 in mode 2 does not beat mode 0. Any of these ends the branch, recorded
here, and the owner is back to choosing between 30/875 and 20/625.

## 11.7 What is not in the design

- Moving logic to another core (§8.33, above).
- Splitting the interpreter itself across cores (by display-list segment): its state is sequential, and
  the render side's GL third is the part with a clean boundary.
- Desktop GL beyond keeping it working: the host build is for correctness and the game's data shape
  (TESTING.md §6), not timing. On desktop GL `GetPixelDepth` is synchronous in every mode.

## 11.8 Stage 1 on the host (2026-09-27)

Built and run on the host's native benchmark build (desktop GL, so LeanGLState 0 and no grouping),
frozen and in motion at the Market, `gSettings.RenderThread` 0 against 3 in one process. libultraship
`f7b827dd`.

**What the host showed:**

- **A flip bug, found by the owner watching the window, not by the checks.** The first build answered
  `GetClipParameters` from a value cached at `Init`, in every mode. The backend's answer depends on the
  framebuffer being drawn to (`mFrameBuffers[mCurrentFrameBuffer].invertY`), and the game's framebuffer
  is flipped, so the scene was drawn upside down in *both* arms. The in-process dumps compared equal,
  because both arms were wrong the same way. Fixed: the proxy shadows each framebuffer's flip
  (`UpdateFramebufferParameters`) and the current one (`StartDrawToFramebuffer`), the only two writers,
  and mode 3 checks every answer. Mode 0 now forwards every query, so it is the unmodified path.
  **Lesson for the checks:** an A/B between two arms of the same new code cannot see a bug they share.
  The reference arm has to run the old code path.
- **Picture:** mode 3 against mode 0 at frames n and n+1, four segments: 0 pixels. The control (the
  flipped frame from the buggy build against today's mode 0): 95,710 pixels, so the comparison can
  see a flip.
- **Checks:** 192,661 per 119 frames, **0 mismatches** (the proxy said bound, or gave a flip, that the
  backend did not). 7,497 `safe_miss`: without grouping the proxy always answers "not bound", which
  costs ~63 extra binds a frame (212 → 241 draws in the vertex traces). The device's default path
  (grouped) is shadowed exactly and should read 0 there.
- **Stream:** ~732 records and ~315 KB per Market frame, as §11.5 estimated (mode 1; mode 3 adds the
  check records).
- **Syncs:** 0.99 per frame on the host, all from the sun's `GetPixelDepth`
  (`Environment_GraphCallback`), which is a real read on desktop GL. On GLES2 it is answered with zeros
  and queued, so it should read ~0 on the device. A stage 2 on desktop GL would serialise on it.
- The host's timing is not the device's (TESTING §6) and is not read here.

**Not testable on the host:** the grouped path (client arrays need GLES2), the GLES2 `GetPixelDepth`
path, and all timing. Next, on the device: mode 3 in motion at the Market (0 mismatches, 0 safe
misses, syncs ~0, and the control `gSettings.ProbeRThreadCorrupt=1` firing), the dumps, then the
frozen A/B 0 → 1 for the cost and `replay_us`.

## 11.9 Stage 1 on the device: exact, nearly free, and the gate passes (2026-09-27)

The device free for the session, `feature/pyra-two-core` at libultraship `f7b827dd`, all defaults
(LeanGLState 3, LeanGroup 1). Pinned at 1000 MHz; every segment below was on the pin.

**Equivalence in motion** (`SOH_BENCH_STATIC=0`, Market, mode 0 against 3, 2 pairs): **2,172,912
checks, 0 mismatches, 0 safe misses**: under grouping the proxy's texture-binding answer is the
backend's, exactly. `syncs` **0.00** per frame: the GLES2 `GetPixelDepth` path does not wait. The control
(`gSettings.ProbeRThreadCorrupt=1`, mode 3, 2 segments) read **659 and 628 safe misses**, ~1 per frame
as designed, with 0 mismatches.

**Picture** (frozen, in-process dumps at frames 301 and 302, segments 0 1 1 0, mode 0 against 1):
Market, **0 pixels** at the same frame in every pair; frame n against n+1 differs by the scene's own
21-pixel cycle in every arm alike. Hyrule Field with the sky masked (rows 216 and up), **0 pixels**
in every comparison, across arms and frames; unmasked, the sky differs as much between two mode-0
segments as across modes.

**Price** (frozen, 6 pairs each, soaked to 66 °C; die 65.7-73.4 °C across the runs):

| RenderThread 0 → 1 | Market (A 15,906 µs) | Hyrule Field (A 10,312 µs) |
|---|---|---|
| render-thread CPU (the recording's cost) | **+99 µs (+0.6%), t 2.7, MDE 94** | **−47 µs (−0.5%), t −2.4, MDE 51** |
| `replay_us` (what would move) | ~3,860 µs | ~3,270 µs |
| `main_us` = thread CPU − replay (projected main thread) | **−3,759 µs (−23.6%), t −112, MDE 86** | **−3,261 µs (−31.6%), t −167, MDE 50** |
| window CPU | +5 µs, t 0.2, MDE 81 | −73 µs, t −3.5, MDE 53 |
| period | −63 µs, t −0.4, MDE 388 | −34 µs, t −0.7, MDE 119 |
| stream per frame | 623 records, 347 KB | 502 records, 166 KB |

- **The recording costs next to nothing**: +99 µs at the Market, under its MDE at the Field (where it
  reads slightly negative). §11.5 estimated 0.2-0.4 ms for the vertex copy alone. *Reasoning, not
  measured:* running the frame's GL side in one burst at the end, instead of interleaved with the
  interpreter, may be paying for the copy in instruction-cache locality. Stage 3 (zero-copy) is not needed.
- **What moves is ~3.9 ms at the Market and ~3.3 ms at the Field** at 1000 MHz: the top of §11.1's
  3.8-4.0 ms estimate. It includes the frame's `EndFrame` and the swap's CPU (`eof_cpu_us` rises by the
  replay because the drain runs inside it) and ImGui's GL draw.
- **Gate: passed at both fixtures** (cost < ~1 ms, net ≥ ~3 ms). At 750 MHz the Market's projected
  saving is ~5 ms (×4/3, an upper bound), which is §11.1's need.
- **What `main_us` does not know:** the two threads will share the L2 and the memory bus, and the GL
  thread will share core 1 with the audio stack (~5 ms a frame at 750, §8.47). Contention will take
  something off the projection. Stage 2 measures it directly: main-thread CPU and period with the
  replay really on another core.

Next: stage 2 (§11.6), starting with the context moving to a second thread on the device, alone.

## 11.10 Stage 2: the GL thread works, and at 750 MHz the frame period falls by a third (2026-09-27)

libultraship `784bf4f4` and after: `gSettings.RenderThread` 2 (4 with the checks). The stream is cut into
64 KB blocks, published to the GL thread as they fill, so the GL thread starts on a frame while the
interpreter is still producing it; the swap is a queued frame-end record, and the main thread may run
one frame ahead and no more. Calls that need an answer (shader creation, framebuffers, the texture-name
pool's refill) run on the GL thread while the main thread waits. What the GL thread would have read
from the CVar store is captured on the main thread instead: the backend's per-frame modes and the
Z-fighting mode (which it had read per decal draw) in a `StartFrame` payload, vsync by
`GfxWindowBackend::LatchFrameSettings`, and the benchmark's dump path in its own record. ImGui's draw
data is cloned for the GL thread. The mode changes at a frame start: the context is released on the main
thread and made current on the GL thread, or the reverse.

**The context moves** (the check that could have ended the branch): 50 segments of mode 0 against 4 in
motion at the Market, the context moved 25 times (13 to the GL thread, 12 back), every segment drew
and measured, no error. The checks across those segments: **2,657,584, 0 mismatches, 0 safe misses**.

**Picture** (device, frozen, in-process dumps, mode 0 against 2): Market **0 pixels** at the same frame,
frame n against n+1 the scene's own ~20-pixel cycle in every arm; Hyrule Field with the sky masked,
**0 pixels** in every comparison.

**Timing** (frozen, uncapped, **750 MHz** pin, 6 pairs each, soaked to 62 °C; every segment on the pin):

| RenderThread 0 → 2 | Market (A) | Δ | t | MDE | Hyrule Field (A) | Δ | t | MDE |
|---|---|---|---|---|---|---|---|---|
| main-thread CPU | 20,881 µs | **−3,696 µs (−17.7%)** | −67 | 142 | 13,210 µs | **−3,422 µs (−25.9%)** | −197 | 45 |
| period | 29,400 µs | **−8,312 µs (−28.3%)** | −34 | 628 | 20,693 µs | **−6,775 µs (−32.7%)** | −45 | 388 |
| window CPU | 17,817 µs | −4,075 µs (−22.9%) | −79 | 133 | 11,490 µs | −3,801 µs (−33.1%) | −214 | 46 |
| GL thread CPU (`replay_us` then, `gl_us` now) | | ~5,700 µs | | | | ~4,700 µs | | |
| main thread waiting for the GL thread | | 15-70 µs | | | | 0.6-1.2 ms | | |

- **The period falls by more than twice the CPU saving.** The main thread no longer waits in the swap
  for the GPU (~5 ms, §8.46); the GL thread does. Uncapped, the Market goes from 34 to 47 fps at 750 MHz.
  At the Field the main thread now outruns the GL thread and the GPU and waits for them (0.6-1.2 ms).
- **It is not free.** The GL work costs ~5.7 ms on the GL thread at the Market against the ~3.7 ms it
  took off the main thread: **~2 ms more CPU per frame across the machine (+10%)**, ~1.3 ms at the Field.
  A short run with the counters split (mode 1 against 2, 120 frames, 750 MHz): the same replay is
  4,604 µs on the main thread and 5,649 µs on the GL thread, of which ~0.35 ms is the swap's CPU (the GL
  thread's figure includes it, mode 1's does not). The other ~0.7 ms (~15%) is the price of the other
  core: *reasoning, not measured* - the stream's 350 KB written on one core and read on the other, and
  the GL thread sharing core 1 with the audio stack. At a fixed clock the design costs heat; it pays only
  through the clock drop, as §11.1 said.
- **Headroom:** at 750 MHz the Market's main thread is now ~17.2 ms of CPU against a 33.3 ms frame, and
  the GL thread ~5.7 ms. *Reasoning, not measured:* at 625 MHz that is ~20.6 and ~6.8 ms, which may hold
  30 fps too. Real play decides, not the fixture (§8.46: the fixture overstated play's misses).

**What the benchmark reads in mode 2:** `thread_us`, the period and the window are the main thread's,
as before. The backend's own counters (`BENCH work`, `group`, `batch`, `glfn`) and `swap_us`/`eof_cpu_us`
are written on the GL thread and read or reset on the main thread without synchronisation, one frame
apart: good for averages, not exact. `pairs.py` now pairs `gl_us` and `total_us` (main + GL thread); its
`main_us` is meaningful for mode 1 runs only.

Next: the gate, real play at 30/750 with the owner, mode 0 against 2, the Market and Death Mountain
Trail first (§11.6).

**Not tested: mode 2 from the first frame.** Every run so far started in mode 0 and switched later. Launched
with `RenderThread=2` in the config, the first frame would queue `ImGui_ImplOpenGL3_NewFrame` (which creates
the font texture, and builds the font atlas through `GetTexDataAsRGBA32`) for the GL thread while the main
thread runs `ImGui::NewFrame`, which reads the atlas: a race. The fix is small: stay direct for the first
frames, or run that first `NewFrame` synchronously. Until then, turn the mode on in a running game.
*(Corrected in §11.13: there was no race in this code. The first frame's `NewFrame` runs before the proxy
leaves mode 0. The guard added there makes that hold whatever the order.)*

## 11.11 Real play at 30/750: the hard scenes come in under the bar (2026-09-27)

The owner playing, `PYRA_WATCH_PIN=1 PYRA_PIN_FREQ=750000 bench.sh watch start SOH_BENCH_ENTRANCE=0x00B1
SOH_BENCH_UNCAP=0 SOH_BENCH_AB_CVAR=gSettings.RenderThread SOH_BENCH_AB_VALUES=0,2`, the Market, then Death
Mountain Trail and eight other scenes: 59 blocks of 290 frames, all on the 750 MHz pin, die 55-6x °C.
The context moved 15 times in play; no errors.

| frames over 40 ms (`hist_ms` 50 + 67 + over) | mode 0 | mode 2 |
|---|---|---|
| **Market** (scene 0x20; 3 blocks each) | 3.22% (28 / 870) | **1.61%** (14 / 870) |
| **Death Mountain Trail** (0x60; 4 and 6 blocks) | 3.19% (37 / 1,160) | **1.09%** (19 / 1,740) |
| all play | 1.45% (122 / 8,410) | **0.85%** (74 / 8,700) |
| over 67 ms, all play | 0.34% | 0.33% |

| paired by block (23 same-scene pairs) | Δ | t | MDE |
|---|---|---|---|
| main-thread CPU (A 15,389 µs) | **−2,870 µs (−18.7%)** | −4.15 | 1,452 |
| frames over 40 ms | −0.66 points | −1.74 | 0.79 |

- **The gate (§11.6) is met on the rates**: the Market at 1.61% and Death Mountain Trail at 1.09%, against the
  1000 MHz bar of ~1.9% and their own 3.2% without the GL thread. All play falls from 1.45% to 0.85%.
- **But the paired test does not resolve it** (t −1.74, under its MDE): misses come in bursts, the block is
  the independent unit, and the Market had only three blocks per arm. The direction is the same in the
  two scenes that matter and overall; the size is not established. A longer session at the Market and
  on the Trail would settle it.
- **The long stalls are untouched** (over 67 ms: 0.34% and 0.33%). They are not frame work the GL thread
  can take: scene loads and the like.
- **LeanSwap's failure (§8.46) did not repeat.** Deferring the swap on the same thread made play worse;
  moving it to another thread with the frame's GL work did not.
- The main thread's CPU in play falls by 2.9 ms (−18.7%), close to the frozen Market's −3.7 ms.
- **The owner felt no difference between the arms** ("it felt the same"), with the arms switching every
  ~10 s: no visible glitch, no felt input lag from the frame the GL thread may add. Played on the built-in
  controls.
- `draws` on the watch lines reads ~113 in mode 2 against ~205 in mode 0: the backend's counters are
  written on the GL thread and reset on the main one (§11.10), so they lose counts. Not a change in drawing
  (the dumps are identical).

Next: a longer real-play A/B at the Market and on Death Mountain Trail to resolve the paired test, then
`thermcost.sh clock` (PROMPT task 2): 30/750 with the GL thread against 30/1000 and 20/625, on battery.
625 MHz with the GL thread is worth one real-play session first (§11.10's headroom).

## 11.12 Watts: the GL thread pays through the clock (2026-09-28)

Done out of order: the owner could not play, and this is the number that decides whether the design is
worth keeping, so it came before §11.11's longer real-play A/B. Hardware repo
`experiments/power-budget/thermcost.sh twocore` (its README's 2026-09-28 section; `thermclock.py . twocore`
reproduces everything): on battery, the Market fixture with game logic running (`SOH_BENCH_STATIC=0`) at the
game's frame cap, battery-gauge watts, userspace-pinned clock. RenderThread 2 is applied by the harness's
one-valued A/B CVar after the settle, i.e. in a running game. The build is 30f9261 (libultraship 1b0ccaa8),
the same binary as §11.11. Mode 0 against 2 at 750 MHz is ABBA by process, 180 s blocks; 750 against 625 in
mode 2 is ABBA in one process, 60 s blocks; temperatures from one 10-minute hold per arm.

| arm | watts (hold) | against 30/1000 | die cpu °C (hold) | frames lost (fixture) |
|---|---|---|---|---|
| 30 fps, 1000 MHz, mode 0 | 4.51 | reference | 67.6 | 0.8% |
| 30 fps, 750 MHz, mode 0 | 4.07-4.10 (ABBA blocks) | | | **4.3%, 3.2%** |
| 30 fps, 750 MHz, mode 2 | 4.21 | **−0.30 W, −3.3 °C** | 64.3 | 0.8% |
| 30 fps, 625 MHz, mode 2 | 4.07 | **−0.44 W, −5.3 °C** | 62.3 | 1.5% |
| 20 fps, 625 MHz, mode 0 | 3.71 | −0.80 W, −7.1 °C | 60.5 | 0.9% |

- **At the same clock the GL thread costs +0.091 W** (two ABBA pairs: +0.106, +0.076; ~+1 °C): §11.10's
  ~2 ms of extra machine CPU per frame, priced.
- **It buys two clock steps at 30 fps.** In the fixture, mode 2 at 750 MHz loses what mode 0 loses at
  1000 (0.8%), where mode 0 at 750 loses 3-4% (§8.44: 3.9%). At 625 MHz mode 2 loses 1.5% (worst 5 s window
  27.8 fps) for −0.44 W, and draws what mode 0 draws at 750 (4.07 W).
- **20 fps at 625 MHz is still 0.36 W and ~1.8 °C cooler than the best 30 fps arm.** The design makes
  30 fps affordable; it does not close the gap to 20.
- The holds ran once each in a fixed order (625-m2, 750-m2, 1000-m0, 20/625), so their temperatures carry
  the run's drift (slopes ≤ 0.12 °C/min at the end of each). The ABBA watts do not.
- These are fixture numbers. In mode 2 at 750, play missed more than the fixture (§11.11: Market 1.6%, Trail
  1.1%, against the fixture's 0.8%), so the fixture's 1.5% at 625 needs real play before 625 is chosen.
- The keyboard backlight pulses (50 ↔ 12) while the game runs, in this run and in §8.44's; ~0.03 W averaged,
  the same in every arm (hardware repo README).

Verdict: worth keeping, for 30 fps. For the thermal goal it is worth 0.30-0.44 W and 3-5 °C over 30/1000.
Next: real play at 30/625 and 30/750 in mode 2 with the owner at the Market and on Death Mountain Trail
(it can double as §11.11's longer A/B, with mode 0 at 750 as the other arm), and the first-frame fix
(§11.10) before mode 2 can be a launch setting.

## 11.13 Mode 2 from the first frame: no race, now guarded (2026-09-28)

**§11.10's race did not exist.** Every frame path calls `Gui::StartDraw` (and in it the ImGui backend's
`NewFrame`) *before* `Interpreter::StartFrame`, and the proxy changes mode only in its `StartFrame`
(`Fast3dWindow::DrawAndRunGraphicsCommands`, and `OTRGlobals`' extraction loop: `gui->StartDraw()` then
`sohFast3dWindow->StartFrame()`). The proxy starts in mode 0, so on a launch with `RenderThread` 2 in the
config, the first `ImGui_ImplOpenGL3_NewFrame` runs directly on the main thread with the context there. It
builds the atlas and sets `TexID`, and the context moves at that frame's `StartFrame`, after it. Nothing in
libultraship or the game destroys the font texture afterwards (no `DestroyFontsTexture`/`DestroyDeviceObjects`
call), so the backend never builds it again.

**The guard** (libultraship `b32aac46`): the interface gains `RunNow(fn, arg)`, which runs fn where the
context is and returns when it has run (the proxy's existing synchronous `Call`). `Gui::ImGuiBackendNewFrame`
uses it while `io.Fonts->TexID` is null, and `RunInOrder` after. Today that is only the first frame, in mode 0,
where both run the call at once, so nothing changes. It stops a reordering of the frame from bringing the race
back. Steady-state cost: one pointer test a frame.

**Tests** (device, 1000 MHz pin for the harness runs):

- **Five hand launches** with `RenderThread` 2 in `shipofharkinian.json` (TESTING §1's launch, 25 s each):
  every one ran, logged `GL context moved to the GL thread` about a second after start, and drew the title
  scene with the ImGui Stats window's text intact. Nothing new in the logs: the same controller-mapping,
  `ComputeFrameBufferCompleteness` and log-flush errors as a direct launch.
- **Checks from launch** (config `RenderThread` 4, the Market in motion, A/B 4 against 0, one pair): the
  context moved before the settle; segment 0 made **1,099,735 checks, 0 mismatches, 0 safe misses**.
- **Dumps from launch** (config `RenderThread` 2, frozen, A/B 2 against 0, in-process): Market **0 pixels**
  between the arms at the same frame (frame n against n+1: the scene's own 20-pixel cycle, as in §11.10);
  Hyrule Field with the sky masked, **0 pixels** in every comparison.

`RenderThread` 2 can now be a launch setting. It stays default 0 until the owner has chosen a clock (§11.12,
PROMPT task 2).

## 11.14 Real play settles it: mode 2 at 750, not 625 (2026-09-28)

Two watch sessions, the owner playing the Market then Death Mountain Trail, ~13 and ~15 minutes, the build of
§11.13 (315e144, libultraship b32aac46), `PYRA_WATCH_PIN=1 PYRA_PIN_FREQ=750000 bench.sh watch start
SOH_BENCH_ENTRANCE=0x00B1 SOH_BENCH_UNCAP=0 SOH_BENCH_AB_CVAR=gSettings.RenderThread`, frames over 40 ms
(`hist_ms` 50 + 67 + over) pooled and paired by block (TESTING trap 26). The input method was not recorded.

**(a) Mode 0 against 2 at 30/750** (`SOH_BENCH_AB_VALUES=0,2`): 82 blocks, all on the pin, die 51-66 °C.

| frames over 40 ms | mode 0 | mode 2 | paired by block (2 − 0) |
|---|---|---|---|
| **Market** (0x20; 17 and 16 blocks) | 2.68% (132 / 4,930) | **0.84%** (39 / 4,640) | −1.88 points, t −2.45, MDE 1.61 (16 pairs) |
| **Death Mountain Trail** (0x60; 21 each) | 6.12% (373 / 6,090) | **2.23%** (136 / 6,090) | −3.89 points, t −6.20, MDE 1.32 (21 pairs) |
| all play | 4.31% | **1.61%** | **−2.77 points, t −5.64, MDE 0.98** (40 pairs) |
| over 67 ms | 0.30% | 0.35% | |

Main-thread CPU (`watchpairs.py`, 40 pairs): A 18,974 µs, **−3,422 µs (−18.0%), t −9.02**. §11.11's
unresolved paired test is resolved: mode 2 misses fewer frames, in both scenes. The Trail was heavier than in
§11.11 (mode 0 6.1% against 3.2%), and mode 2 there came in at 2.23%, over the ~1.9% bar.

**(b) Mode 2 at 30/625 against 30/750, in one process** (`SOH_BENCH_AB_VALUES=2,2`, and a device-side loop
moving `scaling_setspeed` between 625000 and 750000 every 30 s under the userspace pin). 92 blocks: 33 at
625, 33 at 750, 26 straddling a change (dropped). Pairs are adjacent single-clock blocks in the same scene.
Die 63-69 °C.

| frames over 40 ms | 750 MHz | 625 MHz | paired by block (625 − 750) |
|---|---|---|---|
| **Market** (14 blocks each) | 0.99% (40 / 4,060) | 3.03% (123 / 4,060) | +2.23 points, t 2.70, MDE 1.73 (13 pairs) |
| **Death Mountain Trail** (16 and 17) | 1.34% (62 / 4,640) | 4.16% (205 / 4,930) | +2.86 points, t 5.21, MDE 1.15 (14 pairs) |
| all play | 1.11% | **3.52%** | **+2.50 points, t 5.34, MDE 0.98** (28 pairs) |
| over 67 ms | 0.25% | 0.15% | |

Main-thread CPU at 625: **+2,789 µs (+17.7%, t 5.19)** on 15,748 µs. The fixture's 1.5% at 625 (§11.12)
understated play by more than half: 625 misses about three times what 750 does, nearly twice the 1000 MHz bar.

- **Mode 2 at 750, both sessions pooled: the Market 0.91% (79 / 8,700), Death Mountain Trail 1.85%
  (198 / 10,730).** The gate (§11.6: at or under ~1.9%) is met, the Trail with no margin, and it moves between
  sessions (2.23% and 1.34%) with how the owner plays.
- The long stalls (over 67 ms) are 0.15-0.35% in every arm: not frame work (§11.11).
- **The owner could not tell 625 from 750** with the clock changing every 30 s ("I don't know if I noticed a
  difference"). One unblinded session; it says misses at these rates are hard to feel in play, not that
  625 holds.
- Nothing new in the logs: the startup `RGBA Texture ... Size = 0` lines (36, as a direct launch), the
  `ComputeFrameBufferCompleteness` notes and the known log-flush error (TESTING item 11).

**The owner's choice (2026-09-28): 30 fps at 750 MHz in mode 2**, over 30/625 (−0.14 W and ~2 °C more for
three times the misses) and 20/625 (−0.50 W and ~4 °C more, at 20 fps). Against 30/1000 it is −0.30 W and
−3.3 °C (§11.12) at the 1000 MHz miss rate. Next: `RenderThread` 2 as the default, the branch into
`feature/pyra-port`, the 750 MHz pin in the launcher, and the undervolt at 750 (NEXT 0T (6)).

## 11.15 RenderThread 2 is the default on GLES2 (2026-09-29)

libultraship `e0516fad`: `kRenderThreadDefault` (in `gfx_threaded.h`) is 2 under `USE_OPENGLES2` and 0
elsewhere, so the desktop GL build is unchanged; the proxy's `StartFrame` reads the CVar with it. A config
key still wins, and so does the harness's A/B CVar.

Tests on the device (the build of this commit; the previous binary kept as `soh.elf.pre-default`):

- **No `RenderThread` key in the config, three launches** (`launchtest2.sh none 3`, 25 s each): all ran and
  logged `GL context moved to the GL thread`; the title scene drew. The same five log errors as a direct launch.
- **Key set to 0, two launches:** all ran and the context stayed put (no move logged).
- **The harness overrides the default** (no key; `bench.sh ab gSettings.RenderThread 0 2`, the Market, 1000 MHz,
  one pair): arm 0 recorded no frames (no `BENCH rthread` line, mean 20.3 ms, the direct path's); arm 2
  recorded 599 (mean 12.6 ms, `gl_us` 4,959).

**From here, every device run is in mode 2 unless it says otherwise** (TESTING trap 31).
