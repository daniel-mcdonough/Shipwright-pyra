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

#ifdef __cplusplus
}
#endif

#endif
