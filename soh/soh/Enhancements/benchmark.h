#ifndef SOH_BENCHMARK_H
#define SOH_BENCHMARK_H

// A deterministic, unattended performance fixture.
//
// The problem it solves: measuring a renderer change by playing the game and averaging FPS over a
// wall-clock window is too noisy to resolve anything under about ten percent. Three things move
// at once - the scene content (because game speed is renderFPS/InterpolationFPS, so a faster arm
// travels further through the same window), the die temperature, and process start-up state.
//
// This harness removes all three. It boots straight into a chosen entrance on a debug save,
// optionally stops game logic so the display list is byte-identical every frame, renders a fixed
// number of frames rather than running for a fixed time, and can flip a CVar back and forth
// inside a single process so that both arms share one thermal state.
//
// Everything is driven by environment variables so a run is fully described by its command line
// and nothing touches the user's saved configuration. See BENCHMARK.md for the list.
//
// Compiled in only when SOH_BENCHMARK is defined; call sites are guarded so a normal build is
// byte-identical to one without this file.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// True when SOH_BENCH=1 was set in the environment. Cheap; safe to call before Benchmark_Init.
bool Benchmark_IsActive(void);

// Reads the environment and logs the resolved configuration. Called once during start-up.
void Benchmark_Init(void);

// Populates gSaveContext for the fixture scene and leaves it ready for Play_Init, mirroring what
// the debug warp screen does in Select_LoadGame. Only valid when Benchmark_IsActive().
void Benchmark_SetupDebugSave(void);

// Bracket one rendered frame - that is, one DrawAndRunGraphicsCommands call, not one game tick.
void Benchmark_FrameBegin(void);
void Benchmark_FrameEnd(void);

// Logic/render split of one game tick (SOH_BENCH_SPLIT=<ticks per log line>). Independent of the
// fixture phases, so it also runs under SOH_BENCH_WATCH while someone plays. The marks are:
//   SPLIT_TICK_BEGIN    top of the tick, before input and Graph_Update
//   SPLIT_RENDER_BEGIN  Graph_Update done; Graph_ProcessGfxCommands entered
//   SPLIT_RENDER_END    RunCommands done - every interpolated frame drawn and swapped
//   SPLIT_TICK_END      audio hand-off waited for; the tick is over
// `frames` is only read at SPLIT_RENDER_END: the frames that tick rendered.
//
// The finer marks below (report 8 §8.43) cut the whole main thread into contiguous blocks: each
// mark charges the thread CPU and wall time since the previous mark, whichever it was, to the block
// it ends. A mark that does not run (no Play_Update outside gameplay) folds its time into the next.
// The BENCH split line reports the blocks per frame period for each segment. They run with
// SOH_BENCH_SPLIT or gSettings.ProbeSplit=1 (read once per tick, so an A/B can price them).
enum {
    SPLIT_TICK_BEGIN,   // ends "between": the previous tick's end to this one (AltAssets, save states, the loop)
    SPLIT_RENDER_BEGIN, // ends "post_draw": after Play_Draw to Graph_ProcessGfxCommands
    SPLIT_RENDER_END,   // ends "recycle": after the last window, the interpolation maps handed back
    SPLIT_TICK_END,     // ends "audio_wait": the audio hand-off
    SPLIT_INPUT_DONE,   // ends "input": Graph_StartFrame and PadMgr
    SPLIT_UPDATE_DONE,  // ends "update": Graph_Update and Play_Main through Play_Update
    SPLIT_DRAW_DONE,    // ends "draw": FrameInterpolation_StartRecord, Play_Draw, StopRecord
    SPLIT_INTERP_DONE,  // ends "interp": lookup refresh, audio notify, FrameInterpolation_Interpolate
    SPLIT_EVENTS_DONE,  // ends "events": RunCommands' HandleEvents
    SPLIT_WIN_BEGIN,    // ends "between_frames": the frame loop's own work before each window
    SPLIT_GUI_START,    // ends "gui_start": Benchmark_FrameBegin, frame-ready check, mouse, Gui::StartDraw
    SPLIT_FB_START,     // ends "fb_start": Interpreter::StartFrame
    SPLIT_RUN_DONE,     // ends "run": Interpreter::Run, the display list interpreted and submitted
    SPLIT_GUI_END,      // ends "gui_end": Gui::EndDraw, ImGui and the present pass
    SPLIT_WIN_END,      // ends "eof": Interpreter::EndFrame (flush, swap) and Benchmark_FrameEnd
    SPLIT_MARK_COUNT
};
void Benchmark_SplitMark(int mark, int frames);

#ifdef __cplusplus
}
#endif

#endif
