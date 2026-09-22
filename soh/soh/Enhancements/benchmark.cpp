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
    int warmupFrames = 600;  // shader compilation and thermal settle
    int measureFrames = 600; // per segment
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
};

using Clock = std::chrono::steady_clock;

Phase gPhase = Phase::WaitingForScene;
int gPhaseFrames = 0;
int gSegmentIndex = 0;
Clock::time_point gFrameStart;
std::vector<Segment> gRunSegments;
bool gInitialised = false;
bool gPlayerMoved = false;
// The A/B CVar is left exactly as it was found. Without this a run would quietly persist
// whichever arm happened to be last into the user's config on exit.
bool gHaveOriginalArm = false;
int gOriginalArm = 0;

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
// SetTextureFilter does that for us. Anything else here is assumed to be read live; if a future
// A/B target is not, it needs a case adding rather than silently measuring nothing.
void ApplyArm(const std::string& value) {
    CVarSetInteger(gCfg.abCVar.c_str(), (int)strtol(value.c_str(), nullptr, 0));

    if (gCfg.abCVar == std::string(CVAR_TEXTURE_FILTER)) {
        auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
        if (wnd != nullptr) {
            wnd->SetTextureFilter((Fast::FilteringMode)strtol(value.c_str(), nullptr, 0));
        }
    }
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
    snprintf(line, sizeof(line), "BENCH config settle=%d warmup=%d measure=%d static=%d targetFps=%d\n",
             gCfg.settleFrames, gCfg.warmupFrames, gCfg.measureFrames, gCfg.staticScene ? 1 : 0, gCfg.targetFps);
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

    for (size_t i = 0; i < gRunSegments.size(); i++) {
        const Segment& seg = gRunSegments[i];
        snprintf(line, sizeof(line),
                 "BENCH segment %zu arm=%s frames=%zu mean_us=%.1f median_us=%.1f p95_us=%.1f mean_fps=%.3f\n", i,
                 seg.arm.c_str(), seg.frameMicros.size(), MeanMicros(seg.frameMicros),
                 MedianMicros(seg.frameMicros), PercentileMicros(seg.frameMicros, 0.95),
                 MeanMicros(seg.frameMicros) > 0.0 ? 1e6 / MeanMicros(seg.frameMicros) : 0.0);
        text += line;
    }

    // Paired summary. Segments were emitted arm-major within each rep, so rep r arm a is at
    // index r * arms + a. Pairing within a rep is what makes this robust to thermal drift: both
    // arms of a pair are seconds apart rather than minutes.
    if (ArmCount() == 2 && gRunSegments.size() >= 2) {
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
            double a = MeanMicros(gRunSegments[firstIsA ? i0 : i1].frameMicros);
            double b = MeanMicros(gRunSegments[firstIsA ? i1 : i0].frameMicros);
            deltas.push_back(b - a);
            snprintf(line, sizeof(line), "BENCH pair %d a_us=%.1f b_us=%.1f delta_us=%.1f delta_pct=%.2f\n", r, a, b,
                     b - a, a > 0.0 ? 100.0 * (b - a) / a : 0.0);
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
            snprintf(line, sizeof(line), "BENCH paired n=%zu mean_delta_us=%.2f sd_us=%.2f t=%.3f df=%zu\n",
                     deltas.size(), mean, sd, se > 0.0 ? mean / se : 0.0, deltas.size() - 1);
            text += line;
            // The smallest effect this design could have resolved, at the usual two-sided 5%
            // level. Reporting it alongside a null result is the difference between "no effect"
            // and "no effect this rig could see".
            snprintf(line, sizeof(line), "BENCH mde_us=%.2f (approx, 2.78*se at df=4)\n", 2.78 * se);
            text += line;
        }
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
        gOriginalArm = CVarGetInteger(gCfg.abCVar.c_str(), 0);
        gHaveOriginalArm = true;
        ApplyArm(gCfg.abValues[0]);
        // Same reasoning as AdvanceSegment: arm 0 may differ from whatever the config booted
        // with, so the first segment gets the same recompilation grace as the rest.
        gPhase = Phase::SegmentWarmup;
    }
    SPDLOG_INFO("Benchmark measuring: {} segments of {} frames", TotalSegments(), gCfg.measureFrames);
}

void AdvanceSegment() {
    gSegmentIndex++;
    if (gSegmentIndex >= TotalSegments()) {
        gPhase = Phase::Done;
        WriteResults();
        if (gHaveOriginalArm) {
            ApplyArm(std::to_string(gOriginalArm));
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
    gCfg.segmentWarmupFrames = EnvInt("SOH_BENCH_SEG_WARMUP", gCfg.segmentWarmupFrames);
    gCfg.measureFrames = EnvInt("SOH_BENCH_FRAMES", gCfg.measureFrames);
    gCfg.staticScene = EnvBool("SOH_BENCH_STATIC", gCfg.staticScene);
    gCfg.reps = EnvInt("SOH_BENCH_REPS", gCfg.reps);
    gCfg.outPath = EnvOr("SOH_BENCH_OUT", "");
    gCfg.exitWhenDone = EnvBool("SOH_BENCH_EXIT", gCfg.exitWhenDone);
    gCfg.watch = EnvBool("SOH_BENCH_WATCH", gCfg.watch);
    gCfg.fullscreen = EnvBool("SOH_BENCH_FULLSCREEN", gCfg.fullscreen);
    gCfg.targetFps = EnvInt("SOH_BENCH_TARGET_FPS", gCfg.targetFps);

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

    SPDLOG_INFO("Benchmark active: entrance=0x{:04X} age={} static={} settle={} warmup={} frames={} reps={} ab={}",
                gCfg.entrance, gCfg.linkAge, gCfg.staticScene, gCfg.settleFrames, gCfg.warmupFrames,
                gCfg.measureFrames, gCfg.reps, gCfg.abCVar.empty() ? "(none)" : gCfg.abCVar);
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
    gFrameStart = Clock::now();
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
            // harness would faithfully measure the frame limiter.
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

        case Phase::Warmup:
            ApplyCameraOverride();
            if (++gPhaseFrames >= gCfg.warmupFrames) {
                EnterMeasuring();
            }
            break;

        case Phase::SegmentWarmup:
            if (++gPhaseFrames >= gCfg.segmentWarmupFrames) {
                gPhase = Phase::Measuring;
                gPhaseFrames = 0;
            }
            break;

        case Phase::Measuring:
            ApplyCameraOverride();
            gRunSegments.back().frameMicros.push_back(micros);
            if (++gPhaseFrames >= gCfg.measureFrames) {
                AdvanceSegment();
            }
            break;

        case Phase::Done:
            break;
    }
}

#endif // SOH_BENCHMARK
