#include "benchmark.h"

#ifdef SOH_BENCHMARK

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "soh/OTRGlobals.h"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include <libultraship/bridge.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
void Sram_InitDebugSave(void);
// Declared in OTRGlobals.h, which is a C++ header here; these are the C entry points.
uint32_t OTRGetGameRenderWidth(void);
uint32_t OTRGetGameRenderHeight(void);
}

namespace {

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

struct Config {
    bool enabled = false;

    // Fixture. Default is Kokiri Forest, child, fixed noon. Note that the entrance only picks
    // the scene; the viewpoint it spawns with is incidental and, for this entrance, unusually
    // cheap - see the viewpoint block below.
    uint16_t entrance = 0x00EE; // ENTR_KOKIRI_FOREST_0
    int linkAge = 1;            // 0 adult, 1 child
    uint16_t dayTime = 0x8000;  // CLOCK_TIME(12, 0)
    int nightFlag = 0;

    // Frame budget, all counted in rendered frames rather than game ticks.
    int settleFrames = 240;  // scene load, title card, actors reaching rest
    int warmupFrames = 600;  // shader compilation, and the minimum thermal soak
    int measureFrames = 600; // per segment
    // Thermal soak. The warm-up frame count alone does not get this part anywhere near steady
    // state - §8.14 watched the die go 46 -> 77 C across a run with 16000 warm-up frames, and
    // §9.7 watched a pinned run go 65.7 -> 81.1 C. ABBA cancels that drift inside a run; what it
    // cannot do is make two runs start at the same temperature, which is what makes one run's
    // absolute frame time comparable with another's. Holding warm-up until the die reaches a
    // target does. 0 disables it and restores the old fixed-frame behaviour.
    int soakTargetC = 0;
    // The soak cannot wait forever: on a cool day, or with the fixture light enough that the
    // part never gets there, an unbounded wait is a hang that looks like a slow run.
    int warmupMaxFrames = 12000;
    // Switching arms drops the shader cache, so the first frames of every segment after the
    // first pay recompilation. Discarding them is the difference between measuring the change
    // and measuring the IMG shader compiler.
    int segmentWarmupFrames = 180;

    // Freeze game logic so the display list is identical every frame.
    bool staticScene = true;

    // Viewpoint. The spawn that comes with an entrance is not necessarily representative - the
    // default Kokiri Forest entrance puts Link on the treehouse platform with the camera tight
    // behind him and the hut filling much of the frame, which is a far cheaper view than the
    // scene can produce. Overriding the player position and the camera outright is what makes
    // the fixture a deliberate choice rather than an accident of the entrance table.
    bool havePos = false;
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    bool haveYaw = false;
    int yaw = 0; // binang, 0x0000 - 0xFFFF
    bool haveEye = false;
    float eye[3] = { 0.0f, 0.0f, 0.0f };
    bool haveAt = false;
    float at[3] = { 0.0f, 0.0f, 0.0f };

    // Intra-process A/B. Empty abCVar means a single-arm throughput run.
    std::string abCVar;
    std::vector<std::string> abValues;
    int reps = 5; // pairs, so reps * abValues.size() segments

    // The window size is the render target size, so a window that is not always the same size
    // is a resolution confound sitting inside every measurement. Fullscreen is the only size
    // that is reproducible without knowing anything about the desktop.
    bool fullscreen = true;

    std::string outPath;
    bool exitWhenDone = true;
    int targetFps = 240; // high enough that the frame limiter never sleeps
    // Graph_ProcessGfxCommands calls SetTargetFps(InterpolationFPS) on every game tick
    // (OTRGlobals.cpp), so setting targetFps once is overwritten at the next tick and the 30 FPS
    // limiter sleeps inside the timed window after all. Every run before this existed measured
    // "33.3 ms minus the untimed work" whenever a frame fitted under the cap - which is why a
    // light scene showed no response to anything. Uncapping re-asserts the target at the start of
    // every frame. 0 reproduces the old, capped behaviour for comparison with earlier numbers.
    bool uncap = true;

    // Authoring mode. Nothing is frozen, nothing is measured and no override is applied: the
    // game just runs, and the viewpoint is logged every couple of seconds. Walk the player to
    // a good spot with the controls, read the last line out of the log, and pin those numbers.
    // Picking a fixture any other way means picking it blind - see report 9 §9.9.
    bool watch = false;
};

Config gCfg;

const char* EnvOr(const char* name, const char* fallback) {
    const char* v = getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return fallback;
    }
    // Accept 0x-prefixed values so entrance indices can be written the way the entrance table
    // and the `entrance` console command write them.
    return (int)strtol(v, nullptr, 0);
}

bool EnvBool(const char* name, bool fallback) {
    const char* v = getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return fallback;
    }
    return !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0);
}

// Parses "x,y,z" into three floats. Returns false and writes nothing on anything malformed, so
// a typo in the environment fails loudly at start-up rather than silently benchmarking the
// default viewpoint.
bool ParseVec3(const char* name, float out[3]);

std::vector<std::string> SplitCommas(const std::string& in) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= in.size()) {
        size_t comma = in.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(in.substr(start));
            break;
        }
        out.push_back(in.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

bool ParseVec3(const char* name, float out[3]) {
    const char* v = getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return false;
    }
    std::vector<std::string> parts = SplitCommas(v);
    if (parts.size() != 3) {
        SPDLOG_ERROR("{} needs three comma-separated numbers, got \"{}\" - ignoring", name, v);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        out[i] = strtof(parts[(size_t)i].c_str(), nullptr);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Die temperature
// ---------------------------------------------------------------------------------------------

// The thermal zone is resolved once by name rather than hard-coded by index: zone numbering is
// not stable across kernels, and reading the wrong zone would report the PMIC or the battery and
// look entirely plausible. SOH_BENCH_TEMP_PATH overrides it for a board that names its zone
// something else.
std::string ResolveTempPath() {
    const char* override_path = getenv("SOH_BENCH_TEMP_PATH");
    if (override_path != nullptr && override_path[0] != '\0') {
        return override_path;
    }
    for (int i = 0; i < 32; i++) {
        char typePath[64];
        snprintf(typePath, sizeof(typePath), "/sys/class/thermal/thermal_zone%d/type", i);
        FILE* f = fopen(typePath, "r");
        if (f == nullptr) {
            break; // zones are numbered contiguously; the first gap is the end
        }
        char type[64] = { 0 };
        const bool read = fgets(type, sizeof(type), f) != nullptr;
        fclose(f);
        if (!read) {
            continue;
        }
        if (strncmp(type, "cpu", 3) == 0) {
            char tempPath[64];
            snprintf(tempPath, sizeof(tempPath), "/sys/class/thermal/thermal_zone%d/temp", i);
            return tempPath;
        }
    }
    return "";
}

// Milli-degrees C, or -1 if there is nothing to read. Opened per call rather than held: this runs
// once per segment boundary and once a second during the soak, so the syscall is free next to
// being one more descriptor open for the whole run - see report 10 on descriptors in this process.
int ReadTempMilliC() {
    static const std::string path = ResolveTempPath();
    if (path.empty()) {
        return -1;
    }
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) {
        return -1;
    }
    int milli = -1;
    if (fscanf(f, "%d", &milli) != 1) {
        milli = -1;
    }
    fclose(f);
    return milli;
}

// kHz, or -1 if unreadable. The policy's current frequency for cpu0: bench.sh pins min == max, so
// this only moves if something rewrites the limits underneath the pin - pyra-thermal.service or a
// kernel cooling device. Same open-per-call reasoning as ReadTempMilliC.
int ReadCpuKHz() {
    FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f == nullptr) {
        return -1;
    }
    int khz = -1;
    if (fscanf(f, "%d", &khz) != 1) {
        khz = -1;
    }
    fclose(f);
    return khz;
}

// Mid-segment clock samples, every this many measured frames. Start and end alone cannot see a
// throttle that recovers before the segment ends; one sysfs read per 30 frames is noise.
constexpr int kCpuSampleEvery = 30;

// ---------------------------------------------------------------------------------------------
// Run state
// ---------------------------------------------------------------------------------------------

enum class Phase {
    WaitingForScene,
    Settling,
    Warmup,
    SegmentWarmup,
    Measuring,
    Done,
};

struct Segment {
    std::string arm;
    std::vector<uint32_t> frameMicros;
    // Recorded for the same reason the window size is: it is a confound that moves the number a
    // long way and otherwise differs between runs silently. Milli-degrees C, -1 when unreadable.
    int tempStartMilliC = -1;
    int tempEndMilliC = -1;
    // cpu0's clock at the segment's start and end, and the lowest seen by sampling in between.
    // One uncapped run at a pinned 1.25 GHz came out 15% slow with nothing to say why (report 8
    // §8.19); a segment whose clock moved is discarded, not averaged. kHz, -1 when unreadable.
    int cpuStartKHz = -1;
    int cpuEndKHz = -1;
    int cpuMinKHz = -1;
    // The game framebuffer as it actually was while this segment was timed, read at its first and
    // last measured frame. An arm that changes resolution and silently fails to looks exactly like
    // a scene that is not fill-bound, so the size is evidence rather than assumed from the CVar.
    uint32_t fbStartW = 0, fbStartH = 0, fbEndW = 0, fbEndH = 0;
    // Sums over the measured frames, divided by the frame count when reported. Draws and
    // triangles are what the interpreter handed to GL; swap is the time blocked presenting.
    uint64_t counters[BENCH_COUNTER_COUNT] = {};
    uint64_t swapMicros = 0;
    uint64_t finishMicros = 0;
    // Start of one rendered frame to the start of the next: the timed window plus everything
    // outside it - Play_Draw, the frozen Play_Update, audio hand-off. This is what the frame
    // rate is made of; the timed window alone is not.
    uint64_t periodMicros = 0;
};

using Clock = std::chrono::steady_clock;

// Reads the clock and folds it into the segment's minimum; returns the reading.
int SampleCpu(Segment& seg) {
    const int khz = ReadCpuKHz();
    if (khz >= 0 && (seg.cpuMinKHz < 0 || khz < seg.cpuMinKHz)) {
        seg.cpuMinKHz = khz;
    }
    return khz;
}

void SampleCpuStart(Segment& seg) {
    seg.cpuStartKHz = SampleCpu(seg);
}

int KHzToMHz(int khz) {
    return khz < 0 ? -1 : khz / 1000;
}

Phase gPhase = Phase::WaitingForScene;
int gPhaseFrames = 0;
int gSegmentIndex = 0;
Clock::time_point gFrameStart;
Clock::time_point gPrevFrameStart;
uint32_t gPeriodMicros = 0;
Fast::Fast3dWindow* gWnd = nullptr;
std::vector<Segment> gRunSegments;
bool gInitialised = false;
bool gPlayerMoved = false;
// The A/B CVar is left exactly as it was found. Without this a run would quietly persist
// whichever arm happened to be last into the user's config on exit.
bool gHaveOriginalArm = false;
std::string gOriginalArm;

int ArmCount() {
    return gCfg.abCVar.empty() ? 1 : (int)gCfg.abValues.size();
}

int TotalSegments() {
    return gCfg.abCVar.empty() ? 1 : gCfg.reps * ArmCount();
}

// Which arm segment i runs. Strict ABAB alternation leaves a systematic drift - the die warming
// through the run, say - biasing every pair the same way, because B always follows A. Running
// ABBAABBA instead means each arm is first in half the pairs, which cancels a linear drift
// exactly rather than hoping repetition averages it out. Only meaningful for two arms.
size_t ArmIndexForSegment(int segment) {
    if (ArmCount() != 2) {
        return (size_t)(segment % ArmCount());
    }
    const int pair = segment / 2;
    const int posInPair = segment % 2;
    return (size_t)((pair % 2 == 0) ? posInPair : (1 - posInPair));
}

// Applying a CVar mid-run is only correct for settings that are read per frame. Texture filtering
// is not one of them - it is compiled into the shader source - so it needs the cache dropped.
// SetTextureFilter does that for us. CVAR_SHADER_OPTS is compiled in the same way and drops the
// cache directly. Anything else here is assumed to be read live; if a future A/B target is not,
// it needs a case adding rather than silently measuring nothing.
bool ArmIsFloat() {
    return gCfg.abCVar == std::string(CVAR_INTERNAL_RESOLUTION);
}

void ApplyArm(const std::string& value) {
    if (ArmIsFloat()) {
        CVarSetFloat(gCfg.abCVar.c_str(), strtof(value.c_str(), nullptr));
    } else {
        CVarSetInteger(gCfg.abCVar.c_str(), (int)strtol(value.c_str(), nullptr, 0));
    }

    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
    if (wnd == nullptr) {
        return;
    }

    if (gCfg.abCVar == std::string(CVAR_TEXTURE_FILTER)) {
        wnd->SetTextureFilter((Fast::FilteringMode)strtol(value.c_str(), nullptr, 0));
    } else if (gCfg.abCVar == std::string(CVAR_SHADER_OPTS)) {
        wnd->ResetShaderCache();
    } else if (ArmIsFloat()) {
        // The interpreter copies the multiplier once at Init; the CVar alone changes nothing.
        // Gui::CalculateGameViewport rescales the framebuffer from it on the next frame.
        wnd->SetResolutionMultiplier(strtof(value.c_str(), nullptr));
    }
}

// Two-sided 5% critical value of Student's t. Small table rather than a computed inverse: the
// only consumer is the reported minimum detectable effect, the run always has a handful of pairs,
// and 30+ is close enough to normal that the last entry covers every larger design.
double TCrit95(size_t df) {
    static const double kT[] = { 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306,
                                 2.262,  2.228, 2.201, 2.179, 2.160, 2.145, 2.131 };
    if (df < 1) {
        return 0.0;
    }
    const size_t n = sizeof(kT) / sizeof(kT[0]);
    return df <= n ? kT[df - 1] : 2.042;
}

double PerFrame(const Segment& seg, uint64_t sum) {
    return seg.frameMicros.empty() ? 0.0 : (double)sum / (double)seg.frameMicros.size();
}

double MeanMicros(const std::vector<uint32_t>& v) {
    if (v.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (uint32_t x : v) {
        sum += x;
    }
    return sum / (double)v.size();
}

double MedianMicros(std::vector<uint32_t> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double PercentileMicros(std::vector<uint32_t> v, double pct) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(pct * (double)(v.size() - 1));
    return v[idx];
}

void WriteResults() {
    std::string text;
    char line[512];

    snprintf(line, sizeof(line), "BENCH config entrance=0x%04X age=%d dayTime=0x%04X night=%d\n", gCfg.entrance,
             gCfg.linkAge, gCfg.dayTime, gCfg.nightFlag);
    text += line;
    snprintf(line, sizeof(line), "BENCH config settle=%d warmup=%d measure=%d static=%d targetFps=%d uncap=%d\n",
             gCfg.settleFrames, gCfg.warmupFrames, gCfg.measureFrames, gCfg.staticScene ? 1 : 0, gCfg.targetFps,
             gCfg.uncap ? 1 : 0);
    text += line;
    // Recorded because the render target size is the single biggest determinant of the number
    // above it, and because it is the thing most likely to differ silently between two runs.
    {
        auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
        snprintf(line, sizeof(line), "BENCH config window=%ux%u game_fb=%ux%u fullscreen=%d\n",
                 wnd != nullptr ? wnd->GetWidth() : 0u, wnd != nullptr ? wnd->GetHeight() : 0u,
                 OTRGetGameRenderWidth(), OTRGetGameRenderHeight(),
                 (wnd != nullptr && wnd->IsFullscreen()) ? 1 : 0);
        text += line;
    }

    snprintf(line, sizeof(line), "BENCH config abCVar=%s reps=%d\n",
             gCfg.abCVar.empty() ? "(none)" : gCfg.abCVar.c_str(), gCfg.reps);
    text += line;

    // Without this a run's thermal state is not in its own results, and two runs taken at
    // different temperatures compare as though they were taken at the same one.
    snprintf(line, sizeof(line), "BENCH config soak_target_c=%d warmup_max=%d temp_now_c=%.1f\n",
             gCfg.soakTargetC, gCfg.warmupMaxFrames, ReadTempMilliC() / 1000.0);
    text += line;

    for (size_t i = 0; i < gRunSegments.size(); i++) {
        const Segment& seg = gRunSegments[i];
        snprintf(line, sizeof(line),
                 "BENCH segment %zu arm=%s frames=%zu mean_us=%.1f median_us=%.1f p95_us=%.1f mean_fps=%.3f "
                 "temp_c=%.1f->%.1f cpu_mhz=%d->%d cpu_min_mhz=%d fb=%ux%u->%ux%u swap_us=%.1f finish_us=%.1f period_us=%.1f period_fps=%.2f\n",
                 i, seg.arm.c_str(), seg.frameMicros.size(), MeanMicros(seg.frameMicros),
                 MedianMicros(seg.frameMicros), PercentileMicros(seg.frameMicros, 0.95),
                 MeanMicros(seg.frameMicros) > 0.0 ? 1e6 / MeanMicros(seg.frameMicros) : 0.0,
                 seg.tempStartMilliC / 1000.0, seg.tempEndMilliC / 1000.0, KHzToMHz(seg.cpuStartKHz),
                 KHzToMHz(seg.cpuEndKHz), KHzToMHz(seg.cpuMinKHz), seg.fbStartW, seg.fbStartH,
                 seg.fbEndW, seg.fbEndH, PerFrame(seg, seg.swapMicros), PerFrame(seg, seg.finishMicros),
                 PerFrame(seg, seg.periodMicros),
                 PerFrame(seg, seg.periodMicros) > 0.0 ? 1e6 / PerFrame(seg, seg.periodMicros) : 0.0);
        text += line;
        // Per-frame means of the GL work the interpreter issued. Kept off the segment line so that
        // line stays readable; the counters matter when asking what a frame is spent on.
        snprintf(line, sizeof(line),
                 "BENCH work %zu draws=%.1f tris=%.1f tex_uploads=%.2f tex_upload_kb=%.1f fb_binds=%.1f "
                 "clears=%.1f fb_copies=%.2f fb_readbacks=%.2f shader_loads=%.1f fb_reallocs=%.2f\n",
                 i, PerFrame(seg, seg.counters[BENCH_DRAWS]), PerFrame(seg, seg.counters[BENCH_TRIS]),
                 PerFrame(seg, seg.counters[BENCH_TEX_UPLOADS]), PerFrame(seg, seg.counters[BENCH_TEX_UPLOAD_KB]),
                 PerFrame(seg, seg.counters[BENCH_FB_BINDS]), PerFrame(seg, seg.counters[BENCH_CLEARS]),
                 PerFrame(seg, seg.counters[BENCH_FB_COPIES]), PerFrame(seg, seg.counters[BENCH_FB_READBACKS]),
                 PerFrame(seg, seg.counters[BENCH_SHADER_LOADS]), PerFrame(seg, seg.counters[BENCH_FB_REALLOCS]));
        text += line;
    }

    // Paired summary. Segments were emitted arm-major within each rep, so rep r arm a is at
    // index r * arms + a. Pairing within a rep is what makes this robust to thermal drift: both
    // arms of a pair are seconds apart rather than minutes.
    // Run once on the timed window (the historical metric, lines unprefixed) and once on the
    // frame period ("period" prefix). Under the frame cap only the period means anything; uncapped
    // they differ by the work outside the window, and a change that moves one but not the other
    // says where it acted.
    const auto pairedSummary = [&](const char* prefix, double (*metric)(const Segment&)) {
        std::vector<double> deltas;
        for (int r = 0; r < gCfg.reps; r++) {
            size_t i0 = (size_t)(r * 2);
            size_t i1 = i0 + 1;
            if (i1 >= gRunSegments.size()) {
                break;
            }
            // The arms swap order every other pair (see ArmIndexForSegment), so which one is A
            // is read from the recorded arm rather than assumed from the position.
            const bool firstIsA = gRunSegments[i0].arm == gCfg.abValues[0];
            double a = metric(gRunSegments[firstIsA ? i0 : i1]);
            double b = metric(gRunSegments[firstIsA ? i1 : i0]);
            deltas.push_back(b - a);
            snprintf(line, sizeof(line), "BENCH %spair %d a_us=%.1f b_us=%.1f delta_us=%.1f delta_pct=%.2f\n", prefix,
                     r, a, b, b - a, a > 0.0 ? 100.0 * (b - a) / a : 0.0);
            text += line;
        }

        if (deltas.size() >= 2) {
            double mean = 0.0;
            for (double d : deltas) {
                mean += d;
            }
            mean /= (double)deltas.size();
            double ss = 0.0;
            for (double d : deltas) {
                ss += (d - mean) * (d - mean);
            }
            double sd = std::sqrt(ss / (double)(deltas.size() - 1));
            double se = sd / std::sqrt((double)deltas.size());
            snprintf(line, sizeof(line), "BENCH %spaired n=%zu mean_delta_us=%.2f sd_us=%.2f t=%.3f df=%zu\n", prefix,
                     deltas.size(), mean, sd, se > 0.0 ? mean / se : 0.0, deltas.size() - 1);
            text += line;
            // The smallest effect this design could have resolved, at the usual two-sided 5%
            // level. Reporting it alongside a null result is the difference between "no effect"
            // and "no effect this rig could see". The critical value has to follow the actual
            // degrees of freedom: hardcoding df=4's 2.776 overstated the detectable effect by
            // 17% on an eight-pair run, which reads as a weaker rig than the one that ran.
            snprintf(line, sizeof(line), "BENCH %smde_us=%.2f (%.3f*se at df=%zu)\n", prefix,
                     TCrit95(deltas.size() - 1) * se, TCrit95(deltas.size() - 1), deltas.size() - 1);
            text += line;
        }
    };

    if (ArmCount() == 2 && gRunSegments.size() >= 2) {
        pairedSummary("", [](const Segment& seg) { return MeanMicros(seg.frameMicros); });
        pairedSummary("period ", [](const Segment& seg) { return PerFrame(seg, seg.periodMicros); });
    }

    SPDLOG_INFO("\n{}", text);

    if (!gCfg.outPath.empty()) {
        FILE* f = fopen(gCfg.outPath.c_str(), "w");
        if (f != nullptr) {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            SPDLOG_INFO("Benchmark results written to {}", gCfg.outPath);
        } else {
            SPDLOG_ERROR("Benchmark could not open {} for writing", gCfg.outPath);
        }
    }
}

// Far enough into the scene that the player actor exists, early enough that the remaining
// settle frames still cover the camera catching up.
constexpr int kPlayerMoveFrame = 30;

// Generous: a cold scene load on this hardware is slow, but not minutes slow.
constexpr int kSceneLoadTimeoutFrames = 3600;

// Returns true once the override has actually been applied. The player actor does not exist for
// the first frames of a scene, so this is retried rather than fired once and assumed to have
// worked - a silent miss here would benchmark the wrong viewpoint and look entirely normal.
bool ApplyPlayerOverrides() {
    if (!gCfg.havePos && !gCfg.haveYaw) {
        return true; // nothing to do
    }
    if (gPlayState == nullptr) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return false;
    }
    if (gCfg.havePos) {
        player->actor.world.pos.x = gCfg.pos[0];
        player->actor.world.pos.z = gCfg.pos[2];
        // Height is placed once and then left to the game. Holding y down every frame the way
        // x and z are held would hover the player above sloping ground or bury him in it, which
        // makes a viewpoint impossible to author by hand - you would have to know the floor
        // height at every candidate position. Dropping him in and letting the floor catch him
        // means SOH_BENCH_POS can be read as a ground position.
        if (!gPlayerMoved) {
            player->actor.world.pos.y = gCfg.pos[1];
        }
    }
    if (gCfg.haveYaw) {
        player->actor.world.rot.y = (int16_t)gCfg.yaw;
        player->actor.shape.rot.y = (int16_t)gCfg.yaw;
    }
    return true;
}

// Applying an override is not the same as it having stuck, and the difference is invisible
// in the results. Check that the player really is where the run asked for, so a fixture that has
// drifted says so instead of quietly benchmarking somewhere else.
void VerifyPlayerOverrides() {
    if (!gCfg.havePos || gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }
    // Only x and z are checked, because y is deliberately left to the floor - see
    // ApplyPlayerOverrides. The settled height is logged so a fixture can be re-pinned to
    // exactly where the player actually stood.
    const float dx = player->actor.world.pos.x - gCfg.pos[0];
    const float dz = player->actor.world.pos.z - gCfg.pos[2];
    const float drift = std::sqrt(dx * dx + dz * dz);
    if (drift > 4.0f) {
        SPDLOG_ERROR("Benchmark: player ended {:.1f} units from SOH_BENCH_POS in x/z - the pinned position "
                     "did NOT hold, and the frame being measured is not the one that was asked for",
                     drift);
    }
    SPDLOG_INFO("Benchmark: player settled at y={:.1f} (asked for {:.1f})", player->actor.world.pos.y,
                gCfg.pos[1]);
}

void ApplyCameraOverride() {
    if (gPlayState == nullptr || !gCfg.haveEye || !gCfg.haveAt) {
        return;
    }
    // Play_Draw rebuilds the viewing matrix from view.eye/at/up every frame, and Play_Update -
    // which is what normally writes them - is precisely what the freeze skips. So writing them
    // here holds the camera exactly still at a chosen viewpoint.
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

// Printed so a run is reproducible from its own log: position the view once by hand or by
// entrance, read these numbers, then pin them with SOH_BENCH_POS/EYE/AT for every run after.
void LogViewpoint() {
    if (gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    SPDLOG_INFO("Benchmark viewpoint: scene={} pos=[{:.1f},{:.1f},{:.1f}] yaw=0x{:04X} "
                "eye=[{:.1f},{:.1f},{:.1f}] at=[{:.1f},{:.1f},{:.1f}] fovy={:.1f}",
                gPlayState->sceneNum, player != nullptr ? player->actor.world.pos.x : 0.0f,
                player != nullptr ? player->actor.world.pos.y : 0.0f,
                player != nullptr ? player->actor.world.pos.z : 0.0f,
                player != nullptr ? (uint16_t)player->actor.shape.rot.y : 0, gPlayState->view.eye.x,
                gPlayState->view.eye.y, gPlayState->view.eye.z, gPlayState->view.lookAt.x, gPlayState->view.lookAt.y,
                gPlayState->view.lookAt.z, gPlayState->view.fovy);
}

void EnterMeasuring() {
    gPhase = Phase::Measuring;
    gPhaseFrames = 0;
    gSegmentIndex = 0;
    gRunSegments.clear();
    gRunSegments.push_back(Segment{ gCfg.abCVar.empty() ? std::string("single") : gCfg.abValues[0], {} });
    gRunSegments.back().frameMicros.reserve((size_t)gCfg.measureFrames);
    if (!gCfg.abCVar.empty()) {
        gOriginalArm = ArmIsFloat() ? std::to_string(CVarGetFloat(gCfg.abCVar.c_str(), 1.0f))
                                    : std::to_string(CVarGetInteger(gCfg.abCVar.c_str(), 0));
        gHaveOriginalArm = true;
        ApplyArm(gCfg.abValues[0]);
        // Same reasoning as AdvanceSegment: arm 0 may differ from whatever the config booted
        // with, so the first segment gets the same recompilation grace as the rest.
        gPhase = Phase::SegmentWarmup;
    } else {
        // No arm to apply and no cache to rebuild, so this one starts timing straight away and
        // its start temperature is now rather than after a segment warm-up.
        gRunSegments.back().tempStartMilliC = ReadTempMilliC();
        SampleCpuStart(gRunSegments.back());
    }
    SPDLOG_INFO("Benchmark measuring: {} segments of {} frames, die {:.1f} C", TotalSegments(),
                gCfg.measureFrames, ReadTempMilliC() / 1000.0);
}

void AdvanceSegment() {
    gSegmentIndex++;
    if (gSegmentIndex >= TotalSegments()) {
        gPhase = Phase::Done;
        WriteResults();
        if (gHaveOriginalArm) {
            ApplyArm(gOriginalArm);
        }
        if (gCfg.exitWhenDone) {
            Ship::Context::GetInstance()->GetWindow()->Close();
        }
        return;
    }

    const std::string& arm = gCfg.abValues[ArmIndexForSegment(gSegmentIndex)];
    ApplyArm(arm);
    gRunSegments.push_back(Segment{ arm, {} });
    gRunSegments.back().frameMicros.reserve((size_t)gCfg.measureFrames);
    gPhaseFrames = 0;
    // ApplyArm may have thrown the shader cache away; let it rebuild before timing resumes.
    gPhase = Phase::SegmentWarmup;
}

} // namespace

// -------------------------------------------------------------------------------------------------
// Public interface
// -------------------------------------------------------------------------------------------------

extern "C" bool Benchmark_IsActive(void) {
    return gCfg.enabled;
}

extern "C" void Benchmark_Init(void) {
    gCfg.enabled = EnvBool("SOH_BENCH", false);
    if (!gCfg.enabled) {
        return;
    }

    gCfg.entrance = (uint16_t)EnvInt("SOH_BENCH_ENTRANCE", gCfg.entrance);
    gCfg.linkAge = EnvInt("SOH_BENCH_AGE", gCfg.linkAge);
    gCfg.dayTime = (uint16_t)EnvInt("SOH_BENCH_TIME", gCfg.dayTime);
    gCfg.nightFlag = EnvInt("SOH_BENCH_NIGHT", gCfg.nightFlag);
    gCfg.settleFrames = EnvInt("SOH_BENCH_SETTLE", gCfg.settleFrames);
    gCfg.warmupFrames = EnvInt("SOH_BENCH_WARMUP", gCfg.warmupFrames);
    gCfg.soakTargetC = EnvInt("SOH_BENCH_SOAK_C", gCfg.soakTargetC);
    gCfg.warmupMaxFrames = EnvInt("SOH_BENCH_WARMUP_MAX", gCfg.warmupMaxFrames);
    gCfg.segmentWarmupFrames = EnvInt("SOH_BENCH_SEG_WARMUP", gCfg.segmentWarmupFrames);
    gCfg.measureFrames = EnvInt("SOH_BENCH_FRAMES", gCfg.measureFrames);
    gCfg.staticScene = EnvBool("SOH_BENCH_STATIC", gCfg.staticScene);
    gCfg.reps = EnvInt("SOH_BENCH_REPS", gCfg.reps);
    gCfg.outPath = EnvOr("SOH_BENCH_OUT", "");
    gCfg.exitWhenDone = EnvBool("SOH_BENCH_EXIT", gCfg.exitWhenDone);
    gCfg.watch = EnvBool("SOH_BENCH_WATCH", gCfg.watch);
    gCfg.fullscreen = EnvBool("SOH_BENCH_FULLSCREEN", gCfg.fullscreen);
    gCfg.targetFps = EnvInt("SOH_BENCH_TARGET_FPS", gCfg.targetFps);
    gBenchFinish = EnvInt("SOH_BENCH_FINISH", 0);
    gCfg.uncap = EnvBool("SOH_BENCH_UNCAP", gCfg.uncap);

    gCfg.havePos = ParseVec3("SOH_BENCH_POS", gCfg.pos);
    gCfg.haveEye = ParseVec3("SOH_BENCH_EYE", gCfg.eye);
    gCfg.haveAt = ParseVec3("SOH_BENCH_AT", gCfg.at);
    if (getenv("SOH_BENCH_YAW") != nullptr) {
        gCfg.haveYaw = true;
        gCfg.yaw = EnvInt("SOH_BENCH_YAW", 0);
    }
    // An eye without an at (or the reverse) would aim the camera at the origin, which is never
    // what anyone meant. Take both or neither.
    if (gCfg.haveEye != gCfg.haveAt) {
        SPDLOG_ERROR("SOH_BENCH_EYE and SOH_BENCH_AT must be set together; ignoring both");
        gCfg.haveEye = gCfg.haveAt = false;
    }

    gCfg.abCVar = EnvOr("SOH_BENCH_AB_CVAR", "");
    std::string values = EnvOr("SOH_BENCH_AB_VALUES", "");
    if (!gCfg.abCVar.empty()) {
        gCfg.abValues = SplitCommas(values);
        if (gCfg.abValues.size() < 2) {
            SPDLOG_ERROR("SOH_BENCH_AB_CVAR set but SOH_BENCH_AB_VALUES needs at least two comma-separated values; "
                         "falling back to a single-arm run");
            gCfg.abCVar.clear();
            gCfg.abValues.clear();
        }
    }

    if (!gCfg.haveEye && !gCfg.havePos) {
        SPDLOG_WARN("Benchmark viewpoint is whatever entrance 0x{:04X} spawns with. That is a property of the "
                    "entrance table, not a considered choice, and some spawns face a wall. Run once, read the "
                    "logged eye/at/pos, and pin them with SOH_BENCH_EYE/SOH_BENCH_AT before trusting a number.",
                    gCfg.entrance);
    }

    if (gCfg.soakTargetC > 0 && ReadTempMilliC() < 0) {
        SPDLOG_ERROR("SOH_BENCH_SOAK_C is set but no CPU thermal zone could be read. The soak will not "
                     "happen and this run will start at whatever temperature it starts at.");
    }

    SPDLOG_INFO("Benchmark active: entrance=0x{:04X} age={} static={} settle={} warmup={} soak={}C frames={} "
                "reps={} ab={} die={:.1f}C",
                gCfg.entrance, gCfg.linkAge, gCfg.staticScene, gCfg.settleFrames, gCfg.warmupFrames,
                gCfg.soakTargetC, gCfg.measureFrames, gCfg.reps,
                gCfg.abCVar.empty() ? "(none)" : gCfg.abCVar, ReadTempMilliC() / 1000.0);
}

extern "C" void Benchmark_SetupDebugSave(void) {
    if (!gCfg.enabled) {
        return;
    }

    // Mirrors Select_LoadGame in ovl_select. fileNum 0xFF is what makes Sram_InitDebugSave the
    // right call rather than opening a save the device may or may not have.
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

    // Fixed lighting. Time of day changes the environment colours and therefore the fragment
    // work, so leaving it to the clock would put a slow drift inside every measurement.
    gSaveContext.linkAge = gCfg.linkAge;
    gSaveContext.dayTime = gCfg.dayTime;
    gSaveContext.nightFlag = gCfg.nightFlag;

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
    memset(gBenchCounters, 0, sizeof(gBenchCounters));
    gBenchSwapMicros = 0;
    gBenchFinishMicros = 0;
    if (gCfg.uncap && gWnd != nullptr) {
        gWnd->SetTargetFps(gCfg.targetFps);
    }
    gFrameStart = Clock::now();
    gPeriodMicros = gPrevFrameStart.time_since_epoch().count() == 0
                        ? 0
                        : (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(gFrameStart -
                                                                                          gPrevFrameStart)
                              .count();
    gPrevFrameStart = gFrameStart;
}

extern "C" void Benchmark_FrameEnd(void) {
    if (!gCfg.enabled || gPhase == Phase::Done) {
        return;
    }

    const uint32_t micros =
        (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - gFrameStart).count();

    if (!gInitialised) {
        gInitialised = true;
        auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
        if (wnd != nullptr) {
            // Raise the frame cap so SyncFramerateWithTime never sleeps. Without this the
            // harness would faithfully measure the frame limiter. The game lowers it again every
            // tick, so FrameBegin repeats this when uncapping; see Config::uncap.
            gWnd = wnd.get();
            wnd->SetTargetFps(gCfg.targetFps);
            // Window.Fullscreen.Enabled is read in Fast3dWindow::Init, which has already run by
            // the time Benchmark_Init is reached, so the window is resized here instead. A run
            // that quietly used a 640x480 desktop window would report a number that means
            // nothing next to one taken fullscreen.
            if (gCfg.fullscreen && !wnd->IsFullscreen()) {
                wnd->SetFullscreen(true);
                SPDLOG_INFO("Benchmark: switched to fullscreen");
            }
        }
    }

    switch (gPhase) {
        case Phase::WaitingForScene:
            if (gPlayState != nullptr) {
                gPhase = Phase::Settling;
                gPhaseFrames = 0;
            } else if (++gPhaseFrames > kSceneLoadTimeoutFrames) {
                // A bad entrance index leaves the game sitting somewhere that is not gameplay.
                // Failing loudly beats hanging until someone notices.
                SPDLOG_ERROR("Benchmark: no PlayState after {} frames - is entrance 0x{:04X} valid? Giving up.",
                             kSceneLoadTimeoutFrames, gCfg.entrance);
                gPhase = Phase::Done;
                if (gCfg.exitWhenDone) {
                    Ship::Context::GetInstance()->GetWindow()->Close();
                }
            }
            break;

        case Phase::Settling:
            gPhaseFrames++;
            if (gCfg.watch) {
                // Never leaves this phase: no freeze, no overrides, no measurement. Roughly
                // every two seconds, so the log is readable rather than a firehose.
                if (gPhaseFrames % 60 == 0) {
                    LogViewpoint();
                }
                break;
            }
            // Give the scene a moment to spawn actors before reaching for the player, then hold
            // the override down for the rest of the settle window. Applying it once is not
            // enough: ordinary player update - entrance animations, gravity, sliding off a
            // slope - walks the player straight back off the mark over the remaining frames,
            // and the log then reports a viewpoint nothing asked for. Holding it also lets the
            // camera and animations come to rest around the final position rather than chase it.
            if (gPhaseFrames >= kPlayerMoveFrame) {
                const bool applied = ApplyPlayerOverrides();
                if (applied && !gPlayerMoved) {
                    gPlayerMoved = true;
                    if (gCfg.havePos || gCfg.haveYaw) {
                        SPDLOG_INFO("Benchmark: player override applied at settle frame {}", gPhaseFrames);
                    }
                }
            }
            if (gPhaseFrames >= gCfg.settleFrames) {
                if ((gCfg.havePos || gCfg.haveYaw) && !gPlayerMoved) {
                    SPDLOG_ERROR("Benchmark: player override never applied - the measured viewpoint is NOT the "
                                 "one that was asked for");
                }
                VerifyPlayerOverrides();
                if (gCfg.staticScene && gPlayState != nullptr) {
                    // Frame advance stops Play_Update without stopping Play_Draw, so the same
                    // display list is submitted every frame from here on. See z_play.c:737 and
                    // Play_Main, where Play_Draw sits outside the update.
                    gPlayState->frameAdvCtx.enabled = true;
                    SPDLOG_INFO("Benchmark: game logic frozen, scene is now static");
                }
                // Only meaningful once the camera has stopped being updated, which is exactly
                // what freezing achieves - Camera_Update lives inside Play_Update.
                ApplyCameraOverride();
                LogViewpoint();
                gPhase = Phase::Warmup;
                gPhaseFrames = 0;
            }
            break;

        case Phase::Warmup: {
            ApplyCameraOverride();
            gPhaseFrames++;
            const int milli = gCfg.soakTargetC > 0 ? ReadTempMilliC() : -1;
            if (gCfg.soakTargetC > 0 && gPhaseFrames % 60 == 0) {
                SPDLOG_INFO("Benchmark soak: frame {}/{}, die {:.1f} C, target {} C", gPhaseFrames,
                            gCfg.warmupMaxFrames, milli / 1000.0, gCfg.soakTargetC);
            }
            // Both conditions, not either: the frame count is the shader-compilation floor and
            // the temperature is the thermal one, and a run that skipped the first because the
            // part happened to be hot would measure the IMG compiler instead.
            const bool warmEnough = gCfg.soakTargetC <= 0 || milli < 0 || milli >= gCfg.soakTargetC * 1000;
            if (gPhaseFrames >= gCfg.warmupFrames && warmEnough) {
                EnterMeasuring();
            } else if (gPhaseFrames >= gCfg.warmupMaxFrames) {
                // Say so rather than measuring cold in silence. The results still carry the
                // per-segment temperatures, so a run that ended up here is recognisable later.
                SPDLOG_WARN("Benchmark soak: gave up after {} frames at {:.1f} C without reaching {} C. "
                            "This run did NOT start at the same temperature as one that did.",
                            gPhaseFrames, milli / 1000.0, gCfg.soakTargetC);
                EnterMeasuring();
            }
            break;
        }

        case Phase::SegmentWarmup:
            if (++gPhaseFrames >= gCfg.segmentWarmupFrames) {
                gPhase = Phase::Measuring;
                gPhaseFrames = 0;
                gRunSegments.back().tempStartMilliC = ReadTempMilliC();
                SampleCpuStart(gRunSegments.back());
            }
            break;

        case Phase::Measuring:
            ApplyCameraOverride();
            gRunSegments.back().frameMicros.push_back(micros);
            for (int k = 0; k < BENCH_COUNTER_COUNT; k++) {
                gRunSegments.back().counters[k] += gBenchCounters[k];
            }
            gRunSegments.back().swapMicros += gBenchSwapMicros;
            gRunSegments.back().finishMicros += gBenchFinishMicros;
            gRunSegments.back().periodMicros += gPeriodMicros;
            if (gPhaseFrames == 0) {
                gRunSegments.back().fbStartW = OTRGetGameRenderWidth();
                gRunSegments.back().fbStartH = OTRGetGameRenderHeight();
            }
            if (gPhaseFrames > 0 && gPhaseFrames % kCpuSampleEvery == 0) {
                SampleCpu(gRunSegments.back());
            }
            if (++gPhaseFrames >= gCfg.measureFrames) {
                gRunSegments.back().cpuEndKHz = SampleCpu(gRunSegments.back());
                gRunSegments.back().fbEndW = OTRGetGameRenderWidth();
                gRunSegments.back().fbEndH = OTRGetGameRenderHeight();
                gRunSegments.back().tempEndMilliC = ReadTempMilliC();
                AdvanceSegment();
            }
            break;

        case Phase::Done:
            break;
    }
}

#endif // SOH_BENCHMARK
