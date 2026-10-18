// A counter-free benchmark harness, for comparing two different builds (report 12 Tiers 2 and 3).
//
// benchmark.cpp is the instrument for an A/B inside one binary. It cannot compare a stock build
// with the tip: its counters live in libultraship's interpreter, GL backend and frame
// interpolation, many of them inside the fork's own code paths, so porting it to a stock tree
// would carry optimisation code in, and a build with the counters would pay per-frame work its
// stock arm does not. This file implements the same entry points (benchmark.h) with none of that:
// the fixture (debug save, entrance, player and camera pin, frozen logic), the thermal soak, the
// uncap, and per-segment wall period and CPU time. The identical file and the identical three hook
// sites (main.c, title_setup.c, OTRGlobals.cpp) go into both builds, so both pay the same.
//
// One arm per launch: two builds cannot share a process. A run is a soak and then SOH_BENCH_REPS
// identical segments; launches are alternated outside (report 12). Built with -DSOH_BENCHMARK_LITE=ON,
// never together with SOH_BENCHMARK. Output: "BENCHL ..." lines in the log and SOH_BENCH_OUT.

#include "benchmark.h"

#ifdef SOH_BENCHMARK_LITE

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <time.h>

#include "soh/OTRGlobals.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include <ship/Context.h>
#include <fast/Fast3dWindow.h>
#include <libultraship/bridge.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
void Sram_InitDebugSave(void);
uint32_t OTRGetGameRenderWidth(void);
uint32_t OTRGetGameRenderHeight(void);
}

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
    bool enabled = false;
    uint16_t entrance = 0x00EE;
    int linkAge = 1;
    uint16_t dayTime = 0x8000;
    int settleFrames = 400;
    int warmupFrames = 2400;
    int warmupMaxFrames = 16000;
    int soakTargetC = 0;
    int measureFrames = 600;
    int reps = 4;
    bool uncap = true;
    int targetFps = 240;
    bool havePos = false, haveYaw = false, haveEye = false, haveAt = false;
    float pos[3] = {}, eye[3] = {}, at[3] = {};
    int yaw = 0;
    std::string outPath;
    std::string label;
};
Config gCfg;

const char* EnvOr(const char* name, const char* fallback) {
    const char* v = getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    return (v != nullptr && v[0] != '\0') ? (int)strtol(v, nullptr, 0) : fallback;
}

bool ParseVec3(const char* name, float out[3]) {
    const char* v = getenv(name);
    return v != nullptr && sscanf(v, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

int ReadIntFile(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        return -1;
    }
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) {
        v = -1;
    }
    fclose(f);
    return v;
}

// The Pyra's CPU zone is thermal_zone0, the zone benchmark.cpp resolves to by type.
int ReadTempMilliC() {
    return ReadIntFile(EnvOr("SOH_BENCH_TEMP_PATH", "/sys/class/thermal/thermal_zone0/temp"));
}

int ReadCpuKHz() {
    return ReadIntFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
}

uint64_t CpuMicros(clockid_t clock) {
    timespec ts{};
    return clock_gettime(clock, &ts) == 0 ? (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u : 0;
}

enum class Phase { WaitingForScene, Settling, Warmup, Measuring, Done };

struct Segment {
    int frames = 0;
    uint64_t periodSum = 0;
    uint32_t over40 = 0;
    uint64_t threadCpu0 = 0, procCpu0 = 0;
    Clock::time_point wall0;
    double threadUs = 0, procUs = 0, wallUs = 0;
    int temp0 = -1, temp1 = -1, khz0 = -1, khz1 = -1, khzMin = -1, khzMax = -1;
    uint32_t fbW0 = 0, fbH0 = 0, fbW1 = 0, fbH1 = 0;
};

Phase gPhase = Phase::WaitingForScene;
int gPhaseFrames = 0;
bool gPlayerMoved = false;
bool gInitialised = false;
Fast::Fast3dWindow* gWnd = nullptr;
Clock::time_point gPrevFrameStart{};
uint32_t gPeriodMicros = 0;
std::vector<Segment> gLiteSegments;
constexpr int kPlayerMoveFrame = 30;
constexpr int kSceneLoadTimeoutFrames = 3600;
constexpr int kCpuSampleEvery = 30;

void SampleCpu(Segment& s) {
    const int khz = ReadCpuKHz();
    if (khz < 0) {
        return;
    }
    if (s.khzMin < 0 || khz < s.khzMin) {
        s.khzMin = khz;
    }
    if (khz > s.khzMax) {
        s.khzMax = khz;
    }
}

void ApplyPlayerOverrides() {
    if (gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }
    if (gCfg.havePos) {
        player->actor.world.pos.x = gCfg.pos[0];
        player->actor.world.pos.z = gCfg.pos[2];
        if (!gPlayerMoved) {
            player->actor.world.pos.y = gCfg.pos[1];
        }
    }
    if (gCfg.haveYaw) {
        player->actor.world.rot.y = (int16_t)gCfg.yaw;
        player->actor.shape.rot.y = (int16_t)gCfg.yaw;
    }
    gPlayerMoved = true;
}

void ApplyCameraOverride() {
    if (gPlayState == nullptr || !gCfg.haveEye || !gCfg.haveAt) {
        return;
    }
    gPlayState->view.eye.x = gCfg.eye[0];
    gPlayState->view.eye.y = gCfg.eye[1];
    gPlayState->view.eye.z = gCfg.eye[2];
    gPlayState->view.lookAt.x = gCfg.at[0];
    gPlayState->view.lookAt.y = gCfg.at[1];
    gPlayState->view.lookAt.z = gCfg.at[2];
    gPlayState->view.up.x = 0.0f;
    gPlayState->view.up.y = 1.0f;
    gPlayState->view.up.z = 0.0f;
}

void StartSegment() {
    Segment s;
    s.temp0 = ReadTempMilliC();
    s.khz0 = ReadCpuKHz();
    s.khzMin = s.khzMax = s.khz0;
    s.fbW0 = OTRGetGameRenderWidth();
    s.fbH0 = OTRGetGameRenderHeight();
    s.threadCpu0 = CpuMicros(CLOCK_THREAD_CPUTIME_ID);
    s.procCpu0 = CpuMicros(CLOCK_PROCESS_CPUTIME_ID);
    s.wall0 = Clock::now();
    gLiteSegments.push_back(s);
}

void WriteResults() {
    std::string text;
    char line[512];
    snprintf(line, sizeof(line),
             "BENCHL config label=%s entrance=0x%04X uncap=%d window=%ux%u fullscreen=%d reps=%d measure=%d soak_c=%d\n",
             gCfg.label.c_str(), gCfg.entrance, gCfg.uncap ? 1 : 0, gWnd ? gWnd->GetWidth() : 0u,
             gWnd ? gWnd->GetHeight() : 0u, (gWnd && gWnd->IsFullscreen()) ? 1 : 0, gCfg.reps, gCfg.measureFrames,
             gCfg.soakTargetC);
    text += line;
    double sum = 0;
    for (size_t i = 0; i < gLiteSegments.size(); i++) {
        const Segment& s = gLiteSegments[i];
        const double period = s.frames ? (double)s.periodSum / s.frames : 0.0;
        sum += period;
        snprintf(line, sizeof(line),
                 "BENCHL segment %zu label=%s frames=%d period_us=%.1f period_fps=%.2f thread_us=%.1f proc_us=%.1f "
                 "over40=%u temp_c=%.1f->%.1f cpu_mhz=%d->%d cpu_min_mhz=%d cpu_max_mhz=%d fb=%ux%u->%ux%u\n",
                 i, gCfg.label.c_str(), s.frames, period, period > 0 ? 1e6 / period : 0.0, s.threadUs, s.procUs,
                 s.over40, s.temp0 / 1000.0, s.temp1 / 1000.0, s.khz0 / 1000, s.khz1 / 1000, s.khzMin / 1000,
                 s.khzMax / 1000, s.fbW0, s.fbH0, s.fbW1, s.fbH1);
        text += line;
    }
    snprintf(line, sizeof(line), "BENCHL mean label=%s period_us=%.1f segments=%zu\n", gCfg.label.c_str(),
             gLiteSegments.empty() ? 0.0 : sum / gLiteSegments.size(), gLiteSegments.size());
    text += line;
    SPDLOG_INFO("{}", text);
    if (!gCfg.outPath.empty()) {
        if (FILE* f = fopen(gCfg.outPath.c_str(), "w")) {
            fputs(text.c_str(), f);
            fclose(f);
        }
    }
}

} // namespace

extern "C" bool Benchmark_IsActive(void) {
    return gCfg.enabled;
}

extern "C" void Benchmark_Init(void) {
    gCfg.enabled = getenv("SOH_BENCH") != nullptr && strcmp(getenv("SOH_BENCH"), "1") == 0;
    if (!gCfg.enabled) {
        return;
    }
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDrawBegin>([]() {
        if (gPhase == Phase::Warmup || gPhase == Phase::Measuring) {
            ApplyCameraOverride();
        }
    });
    gCfg.entrance = (uint16_t)EnvInt("SOH_BENCH_ENTRANCE", gCfg.entrance);
    gCfg.linkAge = EnvInt("SOH_BENCH_AGE", gCfg.linkAge);
    gCfg.settleFrames = EnvInt("SOH_BENCH_SETTLE", gCfg.settleFrames);
    gCfg.warmupFrames = EnvInt("SOH_BENCH_WARMUP", gCfg.warmupFrames);
    gCfg.warmupMaxFrames = EnvInt("SOH_BENCH_WARMUP_MAX", gCfg.warmupMaxFrames);
    gCfg.soakTargetC = EnvInt("SOH_BENCH_SOAK_C", gCfg.soakTargetC);
    gCfg.measureFrames = EnvInt("SOH_BENCH_FRAMES", gCfg.measureFrames);
    gCfg.reps = EnvInt("SOH_BENCH_REPS", gCfg.reps);
    gCfg.uncap = EnvInt("SOH_BENCH_UNCAP", 1) != 0;
    gCfg.outPath = EnvOr("SOH_BENCH_OUT", "");
    gCfg.label = EnvOr("SOH_BENCH_LABEL", "single");
    gCfg.havePos = ParseVec3("SOH_BENCH_POS", gCfg.pos);
    gCfg.haveEye = ParseVec3("SOH_BENCH_EYE", gCfg.eye);
    gCfg.haveAt = ParseVec3("SOH_BENCH_AT", gCfg.at);
    if (getenv("SOH_BENCH_YAW") != nullptr) {
        gCfg.haveYaw = true;
        gCfg.yaw = EnvInt("SOH_BENCH_YAW", 0);
    }
    SPDLOG_INFO("Benchmark (lite) active: label={} entrance=0x{:04X} settle={} warmup={} soak={}C frames={} reps={} "
                "uncap={}",
                gCfg.label, gCfg.entrance, gCfg.settleFrames, gCfg.warmupFrames, gCfg.soakTargetC,
                gCfg.measureFrames, gCfg.reps, gCfg.uncap);
}

extern "C" void Benchmark_SetupDebugSave(void) {
    if (!gCfg.enabled) {
        return;
    }
    // As benchmark.cpp: mirrors Select_LoadGame in ovl_select.
    gSaveContext.fileNum = 0xFF;
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    Sram_InitDebugSave();
    gSaveContext.magicFillTarget = gSaveContext.magic;
    gSaveContext.magic = 0;
    gSaveContext.magicCapacity = 0;
    gSaveContext.magicLevel = gSaveContext.magic;
    GameInteractor_ExecuteOnLoadGame(gSaveContext.fileNum);
    for (size_t i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++) {
        gSaveContext.buttonStatus[i] = BTN_ENABLED;
    }
    gSaveContext.forceRisingButtonAlphas = gSaveContext.unk_13E8 = gSaveContext.unk_13EA = gSaveContext.unk_13EC = 0;
    gSaveContext.linkAge = gCfg.linkAge;
    gSaveContext.dayTime = gCfg.dayTime;
    gSaveContext.nightFlag = 0;
    gSaveContext.entranceIndex = gCfg.entrance;
    gSaveContext.respawnFlag = 0;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = ENTR_LOAD_OPENING;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.showTitleCard = false;
    gWeatherMode = 0;
}

extern "C" void Benchmark_FrameBegin(void) {
    if (!gCfg.enabled || gPhase == Phase::Done) {
        return;
    }
    if (gCfg.uncap && gWnd != nullptr) {
        gWnd->SetTargetFps(gCfg.targetFps);
    }
    const Clock::time_point now = Clock::now();
    gPeriodMicros = gPrevFrameStart.time_since_epoch().count() == 0
                        ? 0
                        : (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(now - gPrevFrameStart).count();
    gPrevFrameStart = now;
}

extern "C" void Benchmark_FrameEnd(void) {
    if (!gCfg.enabled || gPhase == Phase::Done) {
        return;
    }
    if (!gInitialised) {
        gInitialised = true;
        auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
        if (wnd != nullptr) {
            gWnd = wnd.get();
            if (gCfg.uncap) {
                wnd->SetTargetFps(gCfg.targetFps);
            }
            if (!wnd->IsFullscreen()) {
                wnd->SetFullscreen(true);
            }
        }
    }
    switch (gPhase) {
        case Phase::WaitingForScene:
            if (gPlayState != nullptr) {
                gPhase = Phase::Settling;
                gPhaseFrames = 0;
            } else if (++gPhaseFrames > kSceneLoadTimeoutFrames) {
                SPDLOG_ERROR("Benchmark (lite): no PlayState after {} frames; giving up", kSceneLoadTimeoutFrames);
                gPhase = Phase::Done;
                Ship::Context::GetInstance()->GetWindow()->Close();
            }
            break;
        case Phase::Settling:
            if (++gPhaseFrames >= kPlayerMoveFrame && (gCfg.havePos || gCfg.haveYaw)) {
                ApplyPlayerOverrides();
            }
            if (gPhaseFrames >= gCfg.settleFrames) {
                if (gPlayState != nullptr) {
                    gPlayState->frameAdvCtx.enabled = true;
                    Player* player = GET_PLAYER(gPlayState);
                    if (player != nullptr && gCfg.havePos) {
                        const float dx = player->actor.world.pos.x - gCfg.pos[0];
                        const float dz = player->actor.world.pos.z - gCfg.pos[2];
                        if (std::sqrt(dx * dx + dz * dz) > 4.0f) {
                            SPDLOG_ERROR("Benchmark (lite): the player pin did NOT hold");
                        }
                    }
                }
                ApplyCameraOverride();
                gPhase = Phase::Warmup;
                gPhaseFrames = 0;
            }
            break;
        case Phase::Warmup: {
            ApplyCameraOverride();
            ++gPhaseFrames;
            const int milli = gCfg.soakTargetC > 0 ? ReadTempMilliC() : -1;
            const bool warm = gCfg.soakTargetC <= 0 || milli < 0 || milli >= gCfg.soakTargetC * 1000;
            if ((gPhaseFrames >= gCfg.warmupFrames && warm) || gPhaseFrames >= gCfg.warmupMaxFrames) {
                if (!warm) {
                    SPDLOG_WARN("Benchmark (lite) soak: gave up at {:.1f} C", milli / 1000.0);
                }
                SPDLOG_INFO("Benchmark (lite) measuring: {} segments of {} frames, die {:.1f} C", gCfg.reps,
                            gCfg.measureFrames, ReadTempMilliC() / 1000.0);
                gPhase = Phase::Measuring;
                gPhaseFrames = 0;
                StartSegment();
            }
            break;
        }
        case Phase::Measuring: {
            ApplyCameraOverride();
            Segment& s = gLiteSegments.back();
            if (gPeriodMicros > 0) {
                s.frames++;
                s.periodSum += gPeriodMicros;
                if (gPeriodMicros > 40000) {
                    s.over40++;
                }
            }
            if (++gPhaseFrames % kCpuSampleEvery == 0) {
                SampleCpu(s);
            }
            if (gPhaseFrames >= gCfg.measureFrames) {
                const double wall =
                    (double)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s.wall0).count();
                const double frames = s.frames ? (double)s.frames : 1.0;
                const double period = s.frames ? (double)s.periodSum / s.frames : 0.0;
                // CPU per frame period: CPU time over the segment's wall time, scaled to one period.
                s.threadUs = wall > 0 ? (double)(CpuMicros(CLOCK_THREAD_CPUTIME_ID) - s.threadCpu0) / wall * period : 0;
                s.procUs = wall > 0 ? (double)(CpuMicros(CLOCK_PROCESS_CPUTIME_ID) - s.procCpu0) / wall * period : 0;
                (void)frames;
                s.wallUs = wall;
                s.temp1 = ReadTempMilliC();
                s.khz1 = ReadCpuKHz();
                SampleCpu(s);
                s.fbW1 = OTRGetGameRenderWidth();
                s.fbH1 = OTRGetGameRenderHeight();
                if ((int)gLiteSegments.size() >= gCfg.reps) {
                    gPhase = Phase::Done;
                    WriteResults();
                    Ship::Context::GetInstance()->GetWindow()->Close();
                } else {
                    gPhaseFrames = 0;
                    StartSegment();
                }
            }
            break;
        }
        case Phase::Done:
            break;
    }
}

extern "C" void Benchmark_Shutdown(void) {
}

extern "C" void Benchmark_SplitMark(int mark, int frames) {
    (void)mark;
    (void)frames;
}

#endif // SOH_BENCHMARK_LITE
