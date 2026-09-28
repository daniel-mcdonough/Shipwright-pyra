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
| `SetTextureFilter`, `ClearShaderCache` | queued, and the proxy updates its own shadow | `GetTextureFilter` reads the shadow; `ClearShaderCache` empties the proxy's shader map. |
| `GetName`, `GetMaxTextureSize`, `GetClipParameters` | answered by the proxy | Constant after `Init`; cached then. |
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
