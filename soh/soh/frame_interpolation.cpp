#include <libultraship/bridge.h>

#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <chrono>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "frame_interpolation.h"
#include "soh/OTRGlobals.h"

/*
Frame interpolation.

The idea of this code is to interpolate all matrices.

The code contains two approaches. The first is to interpolate
all inputs in transformations, such as angles, scale and distances,
and then perform the same transformations with the interpolated values.
After evaluation for some reason some animations such rolling look strange.

The second approach is to simply interpolate the final matrices. This will
more or less simply interpolate the world coordinates for movements.
This will however make rotations ~180 degrees get the "paper effect".
The mitigation is to identify this case for actors and interpolate the
matrix but in model coordinates instead, by "removing" the rotation-
translation before interpolating, create a rotation matrix with the
interpolated angle which is then applied to the matrix.

Currently the code contains both methods but only the second one is currently
used.

Both approaches build a tree of instructions, containing matrices
at leaves. Every node is built from OPEN_DISPS/CLOSE_DISPS and manually
inserted FrameInterpolation_OpenChild/FrameInterpolation_Close child calls.
These nodes contain information that should suffice to identify the matrix,
so we can find it in an adjacent frame.

We can interpolate an arbitrary amount of frames between two original frames,
given a specific interpolation factor (0=old frame, 0.5=average of frames,
1.0=new frame).
*/

extern "C" {

void Matrix_Init(struct GameState* gameState);
void Matrix_Push(void);
void Matrix_Pop(void);
void Matrix_Get(MtxF* dest);
void Matrix_Put(MtxF* src);
void Matrix_Mult(MtxF* mf, u8 mode);
void Matrix_Translate(f32 x, f32 y, f32 z, u8 mode);
void Matrix_Scale(f32 x, f32 y, f32 z, u8 mode);
void Matrix_RotateX(f32 x, u8 mode);
void Matrix_RotateY(f32 y, u8 mode);
void Matrix_RotateZ(f32 z, u8 mode);
void Matrix_RotateZYX(s16 x, s16 y, s16 z, u8 mode);
void Matrix_TranslateRotateZYX(Vec3f* translation, Vec3s* rotation);
void Matrix_SetTranslateRotateYXZ(f32 translateX, f32 translateY, f32 translateZ, Vec3s* rot);
Mtx* Matrix_MtxFToMtx(MtxF* src, Mtx* dest);
Mtx* Matrix_ToMtx(Mtx* dest, char* file, s32 line);
Mtx* Matrix_NewMtx(struct GraphicsContext* gfxCtx, char* file, s32 line);
Mtx* Matrix_MtxFToNewMtx(MtxF* src, struct GraphicsContext* gfxCtx);
void Matrix_MultVec3f(Vec3f* src, Vec3f* dest);
void Matrix_MtxFCopy(MtxF* dest, MtxF* src);
void Matrix_MtxToMtxF(Mtx* src, MtxF* dest);
void Matrix_MultVec3fExt(Vec3f* src, Vec3f* dest, MtxF* mf);
void Matrix_Transpose(MtxF* mf);
void Matrix_ReplaceRotation(MtxF* mf);
void Matrix_MtxFToYXZRotS(MtxF* mf, Vec3s* rotDest, s32 flag);
void Matrix_MtxFToZYXRotS(MtxF* mf, Vec3s* rotDest, s32 flag);
void Matrix_RotateAxis(f32 angle, Vec3f* axis, u8 mode);
MtxF* Matrix_CheckFloats(MtxF* mf, char* file, s32 line);
void Matrix_SetTranslateScaleMtx2(Mtx* mtx, f32 scaleX, f32 scaleY, f32 scaleZ, f32 translateX, f32 translateY,
                                  f32 translateZ);

MtxF* Matrix_GetCurrent(void);

void SkinMatrix_MtxFMtxFMult(MtxF* mfA, MtxF* mfB, MtxF* dest);
}

static bool invert_matrix(const float m[16], float invOut[16]);

using namespace std;

#ifdef SOH_BENCHMARK
uint64_t gBenchInterpChildren;
uint64_t gBenchInterpOps;

// SOH_BENCH_INTERP_TRACE=<path> (§8.30): write the recording's call stream for game ticks N and N+1
// (N = SOH_BENCH_INTERP_TRACE_TICK, default 1000, counted from the first recorded tick), so
// docs/pyra/tools/interpbench.cpp can replay the game's real tree shape. Lines: "T" starts a tick,
// "O <ptr> <int>" opens a child, "C" closes one, "o <op>" appends an op, "a"/"m" set the last
// op's has_adjusted/has_mtx flag.
static FILE* sInterpTrace;
static long sInterpTraceTick = -1;
static long sInterpTraceFirst;

static void InterpTraceTick() {
    if (sInterpTraceTick == -1) {
        const char* path = getenv("SOH_BENCH_INTERP_TRACE");
        const char* first = getenv("SOH_BENCH_INTERP_TRACE_TICK");
        sInterpTraceFirst = first != nullptr ? atol(first) : 1000;
        sInterpTrace = path != nullptr ? fopen(path, "w") : nullptr;
        sInterpTraceTick = 0;
    }
    if (sInterpTrace == nullptr) {
        return;
    }
    long tick = sInterpTraceTick++;
    if (tick == sInterpTraceFirst + 2) {
        fclose(sInterpTrace);
        sInterpTrace = nullptr;
    } else if (tick >= sInterpTraceFirst) {
        fprintf(sInterpTrace, "T\n");
    }
}
#define INTERP_TRACE(...)                                                               \
    do {                                                                                \
        if (sInterpTrace != nullptr && sInterpTraceTick > sInterpTraceFirst) {          \
            fprintf(sInterpTrace, __VA_ARGS__);                                         \
        }                                                                               \
    } while (0)
#else
#define INTERP_TRACE(...)
#endif

namespace {

enum class Op {
    OpenChild,
    CloseChild,

    MatrixPush,
    MatrixPop,
    MatrixPut,
    MatrixMult,
    MatrixTranslate,
    MatrixScale,
    MatrixRotate1Coord,
    MatrixRotateZYX,
    MatrixTranslateRotateZYX,
    MatrixSetTranslateRotateYXZ,
    MatrixMtxFToMtx,
    MatrixToMtx,
    MatrixReplaceRotation,
    MatrixRotateAxis,
    SkinMatrixMtxFToMtx
};

typedef pair<const void*, int> label;

union Data {
    Data() {
    }

    struct {
        MtxF src;
    } matrix_put;

    struct {
        MtxF mf;
        u8 mode;
    } matrix_mult;

    struct {
        f32 x, y, z;
        u8 mode;
    } matrix_translate, matrix_scale;

    struct {
        u32 coord;
        f32 value;
        u8 mode;
    } matrix_rotate_1_coord;

    struct {
        s16 x, y, z;
        u8 mode;
    } matrix_rotate_zyx;

    struct {
        Vec3f translation;
        Vec3s rotation;
    } matrix_translate_rotate_zyx;

    struct {
        f32 translateX, translateY, translateZ;
        Vec3s rot;
        // MtxF mtx;
        bool has_mtx;
    } matrix_set_translate_rotate_yxz;

    struct {
        MtxF src;
        Mtx* dest;
    } matrix_mtxf_to_mtx;

    struct {
        Mtx* dest;
        MtxF src;
        bool has_adjusted;
    } matrix_to_mtx;

    struct {
        MtxF mf;
    } matrix_replace_rotation;

    struct {
        f32 angle;
        Vec3f axis;
        u8 mode;
    } matrix_rotate_axis;

    struct {
        label key;
        size_t idx;
    } open_child;
};

// The recording tree's containers. LeanInterp=0 is the original (set it only to A/B against it): every node and op vector is its
// own heap allocation, and the whole older tree is freed node by node at the start of every tick
// (~3600 operator new per tick at the Market, §8.30). LeanInterp=1 is the same tree, matched the
// same way, in std::pmr containers on a per-recording monotonic buffer: the older recording's
// buffer is reused wholesale and its tree is never destructed (every byte of it is in the buffer).
struct StdFamily {
    template <class T> using vec = vector<T>;
    template <class K, class V> using dict = map<K, V>;
};
struct PmrFamily {
    template <class T> using vec = std::pmr::vector<T>;
    template <class K, class V> using dict = std::pmr::map<K, V>;
};

template <class F> struct PathT {
    typename F::template dict<label, typename F::template vec<PathT>> children;
    typename F::template dict<Op, typename F::template vec<Data>> ops;
    typename F::template vec<pair<Op, size_t>> items;
};

// Allocator-aware, so the containers nested in a node share the recording's buffer.
template <> struct PathT<PmrFamily> {
    using allocator_type = std::pmr::polymorphic_allocator<>;
    std::pmr::map<label, std::pmr::vector<PathT>> children;
    std::pmr::map<Op, std::pmr::vector<Data>> ops;
    std::pmr::vector<pair<Op, size_t>> items;
    explicit PathT(const allocator_type& a = {}) : children(a), ops(a), items(a) {
    }
    PathT(PathT&& o, const allocator_type& a)
        : children(std::move(o.children), a), ops(std::move(o.ops), a), items(std::move(o.items), a) {
    }
    PathT(PathT&& o) noexcept = default;
};

// One recorder's state and record functions: the original globals and functions, per store.
template <class F> struct Recorder {
    typedef PathT<F> Path;

    vector<Path*> current_path;
    Path* current_root = nullptr;
    Path* previous_root = nullptr;
    bool next_is_actor_pos_rot_matrix = false;
    bool has_inv_actor_mtx = false;
    MtxF inv_actor_mtx;
    size_t inv_actor_mtx_path_index = 0;

    Data& append(Op op) {
        auto& m = current_path.back()->ops[op];
        current_path.back()->items.emplace_back(op, m.size());
        return m.emplace_back();
    }

    void OpenChild(const void* a, int b) {
        label key = { a, b };
        auto& m = current_path.back()->children[key];
        append(Op::OpenChild).open_child = { key, m.size() };
        current_path.push_back(&m.emplace_back());
    }

    void CloseChild() {
        // append(Op::CloseChild);
        if (has_inv_actor_mtx && current_path.size() == inv_actor_mtx_path_index) {
            has_inv_actor_mtx = false;
        }
        current_path.pop_back();
    }

    void SetTranslateRotateYXZ(f32 translateX, f32 translateY, f32 translateZ, Vec3s* rot) {
        auto& d = append(Op::MatrixSetTranslateRotateYXZ).matrix_set_translate_rotate_yxz = { translateX, translateY,
                                                                                              translateZ, *rot };
        if (next_is_actor_pos_rot_matrix) {
            d.has_mtx = true;
            // d.mtx = *Matrix_GetCurrent();
            invert_matrix((const float*)Matrix_GetCurrent()->mf, (float*)inv_actor_mtx.mf);
            next_is_actor_pos_rot_matrix = false;
            has_inv_actor_mtx = true;
            inv_actor_mtx_path_index = current_path.size();
        }
    }

    auto& ToMtx(Mtx* dest) {
        auto& d = append(Op::MatrixToMtx).matrix_to_mtx = { dest };
        if (has_inv_actor_mtx) {
            d.has_adjusted = true;
            SkinMatrix_MtxFMtxFMult(&inv_actor_mtx, Matrix_GetCurrent(), &d.src);
        } else {
            d.src = *Matrix_GetCurrent();
        }
        return d;
    }

    void BeginRecording() {
        current_path.clear();
        current_path.push_back(current_root);
    }
};

struct StdRecorder : Recorder<StdFamily> {
    struct Recording {
        Path root_path;
    };
    Recording current_recording;
    Recording previous_recording;

    StdRecorder() {
        current_root = &current_recording.root_path;
        previous_root = &previous_recording.root_path;
    }
    void StartRecord() {
        previous_recording = std::move(current_recording);
        current_recording = {};
        BeginRecording();
    }
    void Clear() {
        previous_recording = {};
        current_recording = {};
    }
};

// Counts what a recording spilled past its buffer, so the next use of that buffer can be sized to fit.
struct SpillCounter : std::pmr::memory_resource {
    size_t bytes = 0;
    void* do_allocate(size_t n, size_t align) override {
        bytes += n;
        return std::pmr::new_delete_resource()->allocate(n, align);
    }
    void do_deallocate(void* p, size_t n, size_t align) override {
        std::pmr::new_delete_resource()->deallocate(p, n, align);
    }
    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }
};

struct PmrRecorder : Recorder<PmrFamily> {
    struct Arena {
        unique_ptr<std::byte[]> buf;
        size_t size = 0;
        SpillCounter spill;
        optional<std::pmr::monotonic_buffer_resource> res;
        alignas(Path) unsigned char root_storage[sizeof(Path)];

        // Forget the tree (its memory is all in buf or in spill chunks res frees) and start an empty one.
        Path* Reset() {
            res.reset();
            if (buf == nullptr || spill.bytes > 0) {
                // The Market needs ~230 KB; grow to twice what the last recording used.
                size = std::max<size_t>(256 * 1024, (size + spill.bytes) * 2);
                buf.reset(new std::byte[size]);
                spill.bytes = 0;
            }
            res.emplace(buf.get(), size, &spill);
            return new (root_storage) Path(std::pmr::polymorphic_allocator<>(&*res));
        }
    };
    Arena arena[2];
    int cur = 0;

    PmrRecorder() {
        current_root = arena[0].Reset();
        previous_root = arena[1].Reset();
    }
    void StartRecord() {
        cur ^= 1;
        previous_root = current_root;
        current_root = arena[cur].Reset();
        BeginRecording();
    }
    void Clear() {
        current_root = arena[cur].Reset();
        previous_root = arena[cur ^ 1].Reset();
    }
};

bool is_recording;
uint32_t camera_epoch;
uint32_t previous_camera_epoch;

// Which recorders run: bit 0 the original, bit 1 the pmr one. Read from gSettings.LeanInterp once
// per tick; a change clears both, so a recorder never matches against a recording from before it
// was switched off (for one tick nothing interpolates, as after a scene change).
int active_stores = 0;
StdRecorder std_recorder;
PmrRecorder pmr_recorder;

#define FOR_EACH_RECORDER(r, ...)            \
    do {                                     \
        if (active_stores & 1) {             \
            auto& r = std_recorder;          \
            __VA_ARGS__;                     \
        }                                    \
        if (active_stores & 2) {             \
            auto& r = pmr_recorder;          \
            __VA_ARGS__;                     \
        }                                    \
    } while (0)

#ifdef SOH_BENCHMARK
bool corrupt_pending;

// §8.34, benchmark builds only: two probes for why LeanInterp=1 moved the Market's timed window
// (+295 us, §8.31). Both run here, once per game tick, outside the window, and change no picture.
// gSettings.ProbeSpinUs=N spins N us of CPU: gives back the game-side time the arena saved, to test
// whether the window grew because the GPU had less slack. gSettings.ProbeTouchKB=N writes one of two
// N KB buffers (alternating per tick, as the arena's two recordings do) and reads both: the arena's
// cache traffic without the arena, to test the L2 guess.
void BenchWindowProbes() {
    const int spin_us = CVarGetInteger("gSettings.ProbeSpinUs", 0);
    if (spin_us > 0) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
        while (std::chrono::steady_clock::now() < end) {
        }
    }
    const int touch_kb = CVarGetInteger("gSettings.ProbeTouchKB", 0);
    if (touch_kb > 0) {
        static vector<uint32_t> buf[2];
        static int which;
        const size_t words = (size_t)touch_kb * 1024 / sizeof(uint32_t);
        for (auto& b : buf) {
            if (b.size() != words) {
                b.assign(words, 0);
            }
        }
        which ^= 1;
        static volatile uint32_t sink;
        uint32_t sum = 0;
        for (size_t i = 0; i < words; i += 16) { // one store per 64-byte line
            buf[which][i] = (uint32_t)i;
        }
        for (auto& b : buf) {
            for (size_t i = 0; i < words; i += 16) {
                sum += b[i];
            }
        }
        sink = sum;
    }
}
#endif

struct InterpolateCtx {
    float step;
    float w;
    unordered_map<Mtx*, MtxF> mtx_replacements;
    MtxF tmp_mtxf, tmp_mtxf2;
    Vec3f tmp_vec3f;
    Vec3s tmp_vec3s;
    MtxF actor_mtx;

#ifdef SOH_BENCHMARK
    // gSettings.ProbeSkipInterp=3 (§8.36): the walk writes every replacement here instead of into
    // the map, so the map is never built, copied or searched. A global, so the stores stay.
    static inline MtxF probe_scratch;
    bool probe_no_map = false;
#endif

    // LeanInterpMap=2 (§8.36): nodes extracted from the previous tick's maps, reused here instead of
    // allocating. A recycled node holds a stale matrix; every caller of new_replacement writes all 16
    // floats (interpolate_mtxf, SkinMatrix_MtxFMtxFMult), so none of it survives.
    vector<unordered_map<Mtx*, MtxF>::node_type>* spare_nodes = nullptr;

    MtxF* new_replacement(Mtx* addr) {
#ifdef SOH_BENCHMARK
        if (probe_no_map) {
            return &probe_scratch;
        }
#endif
        if (spare_nodes != nullptr && !spare_nodes->empty()) {
            if (auto it = mtx_replacements.find(addr); it != mtx_replacements.end()) {
                return &it->second;
            }
            auto node = std::move(spare_nodes->back());
            spare_nodes->pop_back();
            node.key() = addr;
            return &mtx_replacements.insert(std::move(node)).position->second;
        }
        return &mtx_replacements[addr];
    }

#ifdef SOH_BENCHMARK
    // SOH_BENCH_INTERP_CAM=<file> (§8.32): every MatrixMtxFToMtx whose two recordings differ, as
    // text - the step, then the old and new matrices' rows.
    static void BenchLogMtxFPair(float step, const MtxF* o, const MtxF* n) {
        static FILE* log = [] {
            const char* path = getenv("SOH_BENCH_INTERP_CAM");
            return path != nullptr ? fopen(path, "w") : nullptr;
        }();
        if (log == nullptr || memcmp(o, n, sizeof(MtxF)) == 0) {
            return;
        }
        fprintf(log, "step %.4f\n", step);
        for (int i = 0; i < 4; i++) {
            fprintf(log, "  old %.7f %.7f %.7f %.7f  new %.7f %.7f %.7f %.7f\n", o->mf[i][0], o->mf[i][1],
                    o->mf[i][2], o->mf[i][3], n->mf[i][0], n->mf[i][1], n->mf[i][2], n->mf[i][3]);
        }
        fflush(log);
    }
#endif

    void interpolate_mtxf(MtxF* res, MtxF* o, MtxF* n) {
        for (size_t i = 0; i < 4; i++) {
            for (size_t j = 0; j < 4; j++) {
                res->mf[i][j] = w * o->mf[i][j] + step * n->mf[i][j];
            }
        }
    }

    float lerp(f32 o, f32 n) {
        return w * o + step * n;
    }

    void lerp_vec3f(Vec3f* res, Vec3f* o, Vec3f* n) {
        res->x = lerp(o->x, n->x);
        res->y = lerp(o->y, n->y);
        res->z = lerp(o->z, n->z);
    }

    float interpolate_angle(f32 o, f32 n) {
        if (o == n)
            return n;
        o = fmodf(o, 2 * M_PI);
        if (o < 0.0f) {
            o += 2 * M_PI;
        }
        n = fmodf(n, 2 * M_PI);
        if (n < 0.0f) {
            n += 2 * M_PI;
        }
        if (fabsf(o - n) > M_PI) {
            if (o < n) {
                o += 2 * M_PI;
            } else {
                n += 2 * M_PI;
            }
        }
        if (fabsf(o - n) > M_PI / 2) {
            // return n;
        }
        return lerp(o, n);
    }

    s16 interpolate_angle(s16 os, s16 ns) {
        if (os == ns)
            return ns;
        int o = (u16)os;
        int n = (u16)ns;
        u16 res;
        int diff = o - n;
        if (-0x8000 <= diff && diff <= 0x8000) {
            if (diff < -0x4000 || diff > 0x4000) {
                return ns;
            }
            res = (u16)(w * o + step * n);
        } else {
            if (o < n) {
                o += 0x10000;
            } else {
                n += 0x10000;
            }
            diff = o - n;
            if (diff < -0x4000 || diff > 0x4000) {
                return ns;
            }
            res = (u16)(w * o + step * n);
        }
        if (os / 327 == ns / 327 && (s16)res / 327 != os / 327) {
            int bp = 0;
        }
        return res;
    }

    void interpolate_angles(Vec3s* res, Vec3s* o, Vec3s* n) {
        res->x = interpolate_angle(o->x, n->x);
        res->y = interpolate_angle(o->y, n->y);
        res->z = interpolate_angle(o->z, n->z);
    }

    template <class Path> void interpolate_branch(Path* old_path, Path* new_path) {
        for (auto& item : new_path->items) {
            Data& new_op = new_path->ops[item.first][item.second];

            if (item.first == Op::OpenChild) {
                if (auto it = old_path->children.find(new_op.open_child.key);
                    it != old_path->children.end() && new_op.open_child.idx < it->second.size()) {
                    interpolate_branch(&it->second[new_op.open_child.idx],
                                       &new_path->children.find(new_op.open_child.key)->second[new_op.open_child.idx]);
                } else {
                    interpolate_branch(&new_path->children.find(new_op.open_child.key)->second[new_op.open_child.idx],
                                       &new_path->children.find(new_op.open_child.key)->second[new_op.open_child.idx]);
                }
                continue;
            }

            if (auto it = old_path->ops.find(item.first); it != old_path->ops.end()) {
                if (item.second < it->second.size()) {
                    Data& old_op = it->second[item.second];
                    switch (item.first) {
                        case Op::OpenChild:
                            break;
                        case Op::CloseChild:
                            break;

                        case Op::MatrixPush:
                            Matrix_Push();
                            break;

                        case Op::MatrixPop:
                            Matrix_Pop();
                            break;

                        case Op::MatrixPut:
                            interpolate_mtxf(&tmp_mtxf, &old_op.matrix_put.src, &new_op.matrix_put.src);
                            Matrix_Put(&tmp_mtxf);
                            break;

                        case Op::MatrixMult:
                            interpolate_mtxf(&tmp_mtxf, &old_op.matrix_mult.mf, &new_op.matrix_mult.mf);
                            Matrix_Mult(&tmp_mtxf, new_op.matrix_mult.mode);
                            break;

                        case Op::MatrixTranslate:
                            Matrix_Translate(lerp(old_op.matrix_translate.x, new_op.matrix_translate.x),
                                             lerp(old_op.matrix_translate.y, new_op.matrix_translate.y),
                                             lerp(old_op.matrix_translate.z, new_op.matrix_translate.z),
                                             new_op.matrix_translate.mode);
                            break;

                        case Op::MatrixScale:
                            Matrix_Scale(lerp(old_op.matrix_scale.x, new_op.matrix_scale.x),
                                         lerp(old_op.matrix_scale.y, new_op.matrix_scale.y),
                                         lerp(old_op.matrix_scale.z, new_op.matrix_scale.z), new_op.matrix_scale.mode);
                            break;

                        case Op::MatrixRotate1Coord: {
                            float v = interpolate_angle(old_op.matrix_rotate_1_coord.value,
                                                        new_op.matrix_rotate_1_coord.value);
                            u8 mode = new_op.matrix_rotate_1_coord.mode;
                            switch (new_op.matrix_rotate_1_coord.coord) {
                                case 0:
                                    Matrix_RotateX(v, mode);
                                    break;

                                case 1:
                                    Matrix_RotateY(v, mode);
                                    break;

                                case 2:
                                    Matrix_RotateZ(v, mode);
                                    break;
                            }
                            break;
                        }

                        case Op::MatrixRotateZYX:
                            Matrix_RotateZYX(interpolate_angle(old_op.matrix_rotate_zyx.x, new_op.matrix_rotate_zyx.x),
                                             interpolate_angle(old_op.matrix_rotate_zyx.y, new_op.matrix_rotate_zyx.y),
                                             interpolate_angle(old_op.matrix_rotate_zyx.z, new_op.matrix_rotate_zyx.z),
                                             new_op.matrix_rotate_zyx.mode);
                            break;

                        case Op::MatrixTranslateRotateZYX:
                            lerp_vec3f(&tmp_vec3f, &old_op.matrix_translate_rotate_zyx.translation,
                                       &new_op.matrix_translate_rotate_zyx.translation);
                            interpolate_angles(&tmp_vec3s, &old_op.matrix_translate_rotate_zyx.rotation,
                                               &new_op.matrix_translate_rotate_zyx.rotation);
                            Matrix_TranslateRotateZYX(&tmp_vec3f, &tmp_vec3s);
                            break;

                        case Op::MatrixSetTranslateRotateYXZ:
                            interpolate_angles(&tmp_vec3s, &old_op.matrix_set_translate_rotate_yxz.rot,
                                               &new_op.matrix_set_translate_rotate_yxz.rot);
                            Matrix_SetTranslateRotateYXZ(lerp(old_op.matrix_set_translate_rotate_yxz.translateX,
                                                              new_op.matrix_set_translate_rotate_yxz.translateX),
                                                         lerp(old_op.matrix_set_translate_rotate_yxz.translateY,
                                                              new_op.matrix_set_translate_rotate_yxz.translateY),
                                                         lerp(old_op.matrix_set_translate_rotate_yxz.translateZ,
                                                              new_op.matrix_set_translate_rotate_yxz.translateZ),
                                                         &tmp_vec3s);
                            if (new_op.matrix_set_translate_rotate_yxz.has_mtx &&
                                old_op.matrix_set_translate_rotate_yxz.has_mtx) {
                                actor_mtx = *Matrix_GetCurrent();
                            }
                            break;

                        case Op::MatrixMtxFToMtx:
                            interpolate_mtxf(new_replacement(new_op.matrix_mtxf_to_mtx.dest),
                                             &old_op.matrix_mtxf_to_mtx.src, &new_op.matrix_mtxf_to_mtx.src);
#ifdef SOH_BENCHMARK
                            BenchLogMtxFPair(step, &old_op.matrix_mtxf_to_mtx.src, &new_op.matrix_mtxf_to_mtx.src);
#endif
                            break;

                        case Op::MatrixToMtx: {
                            //*new_replacement(new_op.matrix_to_mtx.dest) = *Matrix_GetCurrent();
                            if (old_op.matrix_to_mtx.has_adjusted && new_op.matrix_to_mtx.has_adjusted) {
                                interpolate_mtxf(&tmp_mtxf, &old_op.matrix_to_mtx.src, &new_op.matrix_to_mtx.src);
                                SkinMatrix_MtxFMtxFMult(&actor_mtx, &tmp_mtxf,
                                                        new_replacement(new_op.matrix_to_mtx.dest));
                            } else {
                                interpolate_mtxf(new_replacement(new_op.matrix_to_mtx.dest), &old_op.matrix_to_mtx.src,
                                                 &new_op.matrix_to_mtx.src);
                            }
                            break;
                        }

                        case Op::MatrixReplaceRotation:
                            interpolate_mtxf(&tmp_mtxf, &old_op.matrix_replace_rotation.mf,
                                             &new_op.matrix_replace_rotation.mf);
                            Matrix_ReplaceRotation(&tmp_mtxf);
                            break;

                        case Op::MatrixRotateAxis:
                            lerp_vec3f(&tmp_vec3f, &old_op.matrix_rotate_axis.axis, &new_op.matrix_rotate_axis.axis);
                            Matrix_RotateAxis(
                                interpolate_angle(old_op.matrix_rotate_axis.angle, new_op.matrix_rotate_axis.angle),
                                &tmp_vec3f, new_op.matrix_rotate_axis.mode);
                            break;

                        case Op::SkinMatrixMtxFToMtx:
                            break;
                    }
                }
            }
        }
    }
};

} // anonymous namespace

// LeanInterpMap=2 (§8.36): maps and nodes handed back after the frames that used them were drawn.
// Bounded, so a scene with an unusual burst of matrices does not pin its peak forever.
static bool recycle_maps;
static constexpr size_t kMaxSpareNodes = 4096;
static constexpr size_t kMaxSpareMaps = 4;
static vector<unordered_map<Mtx*, MtxF>> spare_maps;
static vector<unordered_map<Mtx*, MtxF>::node_type> spare_nodes;

#ifdef SOH_BENCHMARK
uint64_t gBenchInterpCompared;
uint64_t gBenchInterpMismatch;
#endif

unordered_map<Mtx*, MtxF> FrameInterpolation_Interpolate(float step) {
#ifdef SOH_BENCHMARK
    // gSettings.ProbeSkipInterp=2: record as usual, but hand back no replacements. Against mode 1
    // it splits the cost between building the recordings and walking them.
    if (CVarGetInteger("gSettings.ProbeSkipInterp", 0) == 2) {
        return {};
    }
#endif
    InterpolateCtx ctx;
    ctx.step = step;
    ctx.w = 1.0f - step;
#ifdef SOH_BENCHMARK
    // gSettings.ProbeSkipInterp=3 (§8.36): walk as usual, but into a scratch matrix, and hand back no
    // replacements. Against mode 0 it prices the replacement map: building it, copying it out, and
    // the interpreter's hits. Pixel-exact only in a frozen fixture, like modes 1 and 2.
    if (CVarGetInteger("gSettings.ProbeSkipInterp", 0) == 3) {
        ctx.probe_no_map = true;
        if (active_stores == 2) {
            ctx.interpolate_branch(pmr_recorder.previous_root, pmr_recorder.current_root);
        } else {
            ctx.interpolate_branch(std_recorder.previous_root, std_recorder.current_root);
        }
        return {};
    }
#endif
    // gSettings.LeanInterpMap (§8.36). 1: reserve the map at the previous one's size, so it is not
    // rehashed as it grows, and move it out instead of copying it (a member of a local is not moved
    // implicitly) - the same entries, half the allocations. 2: also reuse the previous tick's maps
    // and their nodes (FrameInterpolation_RecycleMaps) - the same entries, no allocations. 3
    // (benchmark builds): mode 2, plus a map built the original way, compared entry by entry.
    static size_t last_size;
    int lean_map = CVarGetInteger("gSettings.LeanInterpMap", 0);
#ifndef SOH_BENCHMARK
    lean_map = lean_map == 3 ? 2 : lean_map;
#endif
    recycle_maps = lean_map >= 2;
    if (recycle_maps) {
        if (!spare_maps.empty()) {
            ctx.mtx_replacements = std::move(spare_maps.back());
            spare_maps.pop_back();
        }
        ctx.spare_nodes = &spare_nodes;
    }
    if (lean_map != 0) {
        ctx.mtx_replacements.reserve(last_size);
    }
    if (active_stores == 2) {
        ctx.interpolate_branch(pmr_recorder.previous_root, pmr_recorder.current_root);
    } else {
        ctx.interpolate_branch(std_recorder.previous_root, std_recorder.current_root);
    }
#ifdef SOH_BENCHMARK
    // LeanInterp=2: walk the pmr recording too and compare the two replacement maps entry by entry.
    // LeanInterpMap=3: walk the same recording again into a fresh map and compare it with the
    // recycled one. Either way the first map is the one used. gSettings.ProbeInterpCorrupt=2 is the
    // positive control for the second: one entry of the recycled map is perturbed for the compare and
    // restored after it, so exactly one mismatch per interpolated frame must show.
    if (active_stores == 3 || lean_map == 3) {
        InterpolateCtx other;
        other.step = step;
        other.w = 1.0f - step;
        if (active_stores == 3) {
            other.interpolate_branch(pmr_recorder.previous_root, pmr_recorder.current_root);
        } else if (active_stores == 2) {
            other.interpolate_branch(pmr_recorder.previous_root, pmr_recorder.current_root);
        } else {
            other.interpolate_branch(std_recorder.previous_root, std_recorder.current_root);
        }
        auto& used = ctx.mtx_replacements;
        MtxF saved{};
        MtxF* perturbed = nullptr;
        if (lean_map == 3 && CVarGetInteger("gSettings.ProbeInterpCorrupt", 0) == 2 && !used.empty()) {
            perturbed = &used.begin()->second;
            saved = *perturbed;
            perturbed->mf[0][0] += 1.0f;
        }
        gBenchInterpCompared += used.size();
        if (other.mtx_replacements.size() != used.size()) {
            gBenchInterpMismatch++;
        }
        for (auto& [dest, mf] : used) {
            auto it = other.mtx_replacements.find(dest);
            if (it == other.mtx_replacements.end() || memcmp(&it->second, &mf, sizeof(mf)) != 0) {
                gBenchInterpMismatch++;
            }
        }
        if (perturbed != nullptr) {
            *perturbed = saved;
        }
    }
#endif
    if (lean_map != 0) {
        last_size = ctx.mtx_replacements.size();
        return std::move(ctx.mtx_replacements);
    }
    return ctx.mtx_replacements;
}

void FrameInterpolation_RecycleMaps(std::vector<std::unordered_map<Mtx*, MtxF>>& maps) {
    if (!recycle_maps) {
        return;
    }
    for (auto& m : maps) {
        if (m.empty()) {
            continue; // a frame that did not interpolate: a default map, which never allocated
        }
        while (!m.empty() && spare_nodes.size() < kMaxSpareNodes) {
            spare_nodes.push_back(m.extract(m.begin()));
        }
        m.clear();
        if (spare_maps.size() < kMaxSpareMaps) {
            spare_maps.push_back(std::move(m)); // keeps its bucket array
        }
    }
}

void FrameInterpolation_StartRecord(void) {
    // Default 1 since §8.30: exact, and -8.0% render-thread CPU at the Market.
    int lean = CVarGetInteger("gSettings.LeanInterp", 1);
#ifndef SOH_BENCHMARK
    lean = lean == 2 ? 0 : lean;
#endif
    const int stores = lean == 1 ? 2 : lean == 2 ? 3 : 1;
    if (stores != active_stores) {
        std_recorder.Clear();
        pmr_recorder.Clear();
        active_stores = stores;
    }
    FOR_EACH_RECORDER(r, r.StartRecord());
#ifdef SOH_BENCHMARK
    // gSettings.ProbeInterpCorrupt=1, benchmark builds only: the positive control for LeanInterp=2.
    // Perturbs the first ToMtx of each tick in the pmr recording.
    corrupt_pending = CVarGetInteger("gSettings.ProbeInterpCorrupt", 0) != 0;
    BenchWindowProbes();
    // gSettings.ProbeSkipInterp=1 (§8.29), benchmark builds only, read once per game tick. Records
    // nothing, so FrameInterpolation_Interpolate returns an empty map and GfxSpMatrix never finds a
    // replacement - while InterpolationFPS, and with it the number of frames each tick emits, is
    // untouched. In a frozen fixture the two recordings would be identical, so this prices
    // interpolation without changing the picture - a premise the visual check has to confirm.
    // Wrong in play: nothing moves between game ticks. Never ship it.
    if (CVarGetInteger("gSettings.ProbeSkipInterp", 0) == 1) {
        return;
    }
#endif
    if (OTRGlobals::Instance->GetInterpolationFPS() != 20) {
        is_recording = true;
#ifdef SOH_BENCHMARK
        InterpTraceTick();
#endif
    }
}

void FrameInterpolation_StopRecord(void) {
    previous_camera_epoch = camera_epoch;
    is_recording = false;
}

void FrameInterpolation_RecordOpenChild(const void* a, int b) {
    if (!is_recording)
        return;
#ifdef SOH_BENCHMARK
    gBenchInterpChildren++;
    gBenchInterpOps++;
#endif
    INTERP_TRACE("O %p %d\n", a, b);
    FOR_EACH_RECORDER(r, r.OpenChild(a, b));
}

void FrameInterpolation_RecordCloseChild(void) {
    if (!is_recording)
        return;
    INTERP_TRACE("C\n");
    FOR_EACH_RECORDER(r, r.CloseChild());
}

void FrameInterpolation_DontInterpolateCamera(void) {
    camera_epoch = previous_camera_epoch + 1;
}

int FrameInterpolation_GetCameraEpoch(void) {
    return (int)camera_epoch;
}

void FrameInterpolation_RecordActorPosRotMatrix(void) {
    if (!is_recording)
        return;
    FOR_EACH_RECORDER(r, r.next_is_actor_pos_rot_matrix = true);
}

// Every op but OpenChild, ToMtx and SetTranslateRotateYXZ: count, trace, append to each recorder.
#ifdef SOH_BENCHMARK
#define RECORD_OP(op, ...)                         \
    do {                                           \
        if (!is_recording)                         \
            return;                                \
        gBenchInterpOps++;                         \
        INTERP_TRACE("o %d\n", (int)(op));         \
        FOR_EACH_RECORDER(r, r.append(op) __VA_ARGS__); \
    } while (0)
#else
#define RECORD_OP(op, ...)                         \
    do {                                           \
        if (!is_recording)                         \
            return;                                \
        FOR_EACH_RECORDER(r, r.append(op) __VA_ARGS__); \
    } while (0)
#endif

void FrameInterpolation_RecordMatrixPush(void) {
    RECORD_OP(Op::MatrixPush);
}

void FrameInterpolation_RecordMatrixPop(void) {
    RECORD_OP(Op::MatrixPop);
}

void FrameInterpolation_RecordMatrixPut(MtxF* src) {
    RECORD_OP(Op::MatrixPut, .matrix_put = { *src });
}

void FrameInterpolation_RecordMatrixMult(MtxF* mf, u8 mode) {
    RECORD_OP(Op::MatrixMult, .matrix_mult = { *mf, mode });
}

void FrameInterpolation_RecordMatrixTranslate(f32 x, f32 y, f32 z, u8 mode) {
    RECORD_OP(Op::MatrixTranslate, .matrix_translate = { x, y, z, mode });
}

void FrameInterpolation_RecordMatrixScale(f32 x, f32 y, f32 z, u8 mode) {
    RECORD_OP(Op::MatrixScale, .matrix_scale = { x, y, z, mode });
}

void FrameInterpolation_RecordMatrixRotate1Coord(u32 coord, f32 value, u8 mode) {
    RECORD_OP(Op::MatrixRotate1Coord, .matrix_rotate_1_coord = { coord, value, mode });
}

void FrameInterpolation_RecordMatrixRotateZYX(s16 x, s16 y, s16 z, u8 mode) {
    RECORD_OP(Op::MatrixRotateZYX, .matrix_rotate_zyx = { x, y, z, mode });
}

void FrameInterpolation_RecordMatrixTranslateRotateZYX(Vec3f* translation, Vec3s* rotation) {
    RECORD_OP(Op::MatrixTranslateRotateZYX, .matrix_translate_rotate_zyx = { *translation, *rotation });
}

void FrameInterpolation_RecordMatrixSetTranslateRotateYXZ(f32 translateX, f32 translateY, f32 translateZ, Vec3s* rot) {
    if (!is_recording)
        return;
#ifdef SOH_BENCHMARK
    gBenchInterpOps++;
    INTERP_TRACE("o %d\n", (int)Op::MatrixSetTranslateRotateYXZ);
    const bool traced_mtx = (active_stores & 1) ? std_recorder.next_is_actor_pos_rot_matrix
                                                 : pmr_recorder.next_is_actor_pos_rot_matrix;
    if (traced_mtx) {
        INTERP_TRACE("m\n");
    }
#endif
    FOR_EACH_RECORDER(r, r.SetTranslateRotateYXZ(translateX, translateY, translateZ, rot));
}

void FrameInterpolation_RecordMatrixMtxFToMtx(MtxF* src, Mtx* dest) {
    RECORD_OP(Op::MatrixMtxFToMtx, .matrix_mtxf_to_mtx = { *src, dest });
}

void FrameInterpolation_RecordMatrixToMtx(Mtx* dest, char* file, s32 line) {
    if (!is_recording)
        return;
#ifdef SOH_BENCHMARK
    gBenchInterpOps++;
    INTERP_TRACE("o %d\n", (int)Op::MatrixToMtx);
#endif
    if (active_stores & 1) {
        auto& d = std_recorder.ToMtx(dest);
        if (d.has_adjusted) {
            INTERP_TRACE("a\n");
        }
    }
    if (active_stores & 2) {
        auto& d = pmr_recorder.ToMtx(dest);
        if (d.has_adjusted && !(active_stores & 1)) {
            INTERP_TRACE("a\n");
        }
#ifdef SOH_BENCHMARK
        if (corrupt_pending) {
            d.src.mf[1][2] += 1.0f;
            corrupt_pending = false;
        }
#endif
    }
}

void FrameInterpolation_RecordMatrixReplaceRotation(MtxF* mf) {
    RECORD_OP(Op::MatrixReplaceRotation, .matrix_replace_rotation = { *mf });
}

void FrameInterpolation_RecordMatrixRotateAxis(f32 angle, Vec3f* axis, u8 mode) {
    RECORD_OP(Op::MatrixRotateAxis, .matrix_rotate_axis = { angle, *axis, mode });
}

void FrameInterpolation_RecordSkinMatrixMtxFToMtx(MtxF* src, Mtx* dest) {
    if (!is_recording)
        return;
    FrameInterpolation_RecordMatrixMtxFToMtx(src, dest);
}

// https://stackoverflow.com/questions/1148309/inverting-a-4x4-matrix
static bool invert_matrix(const float m[16], float invOut[16]) {
    float inv[16], det;
    int i;

    // clang-format off
    inv[0] = m[5]  * m[10] * m[15] -
             m[5]  * m[11] * m[14] -
             m[9]  * m[6]  * m[15] +
             m[9]  * m[7]  * m[14] +
             m[13] * m[6]  * m[11] -
             m[13] * m[7]  * m[10];

    inv[4] = -m[4]  * m[10] * m[15] +
              m[4]  * m[11] * m[14] +
              m[8]  * m[6]  * m[15] -
              m[8]  * m[7]  * m[14] -
              m[12] * m[6]  * m[11] +
              m[12] * m[7]  * m[10];

    inv[8] = m[4]  * m[9] * m[15] -
             m[4]  * m[11] * m[13] -
             m[8]  * m[5] * m[15] +
             m[8]  * m[7] * m[13] +
             m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];

    inv[12] = -m[4]  * m[9] * m[14] +
               m[4]  * m[10] * m[13] +
               m[8]  * m[5] * m[14] -
               m[8]  * m[6] * m[13] -
               m[12] * m[5] * m[10] +
               m[12] * m[6] * m[9];

    inv[1] = -m[1]  * m[10] * m[15] +
              m[1]  * m[11] * m[14] +
              m[9]  * m[2] * m[15] -
              m[9]  * m[3] * m[14] -
              m[13] * m[2] * m[11] +
              m[13] * m[3] * m[10];

    inv[5] = m[0]  * m[10] * m[15] -
             m[0]  * m[11] * m[14] -
             m[8]  * m[2] * m[15] +
             m[8]  * m[3] * m[14] +
             m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];

    inv[9] = -m[0]  * m[9] * m[15] +
              m[0]  * m[11] * m[13] +
              m[8]  * m[1] * m[15] -
              m[8]  * m[3] * m[13] -
              m[12] * m[1] * m[11] +
              m[12] * m[3] * m[9];

    inv[13] = m[0]  * m[9] * m[14] -
              m[0]  * m[10] * m[13] -
              m[8]  * m[1] * m[14] +
              m[8]  * m[2] * m[13] +
              m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];

    inv[2] = m[1]  * m[6] * m[15] -
             m[1]  * m[7] * m[14] -
             m[5]  * m[2] * m[15] +
             m[5]  * m[3] * m[14] +
             m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];

    inv[6] = -m[0]  * m[6] * m[15] +
              m[0]  * m[7] * m[14] +
              m[4]  * m[2] * m[15] -
              m[4]  * m[3] * m[14] -
              m[12] * m[2] * m[7] +
              m[12] * m[3] * m[6];

    inv[10] = m[0]  * m[5] * m[15] -
              m[0]  * m[7] * m[13] -
              m[4]  * m[1] * m[15] +
              m[4]  * m[3] * m[13] +
              m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];

    inv[14] = -m[0]  * m[5] * m[14] +
               m[0]  * m[6] * m[13] +
               m[4]  * m[1] * m[14] -
               m[4]  * m[2] * m[13] -
               m[12] * m[1] * m[6] +
               m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] +
              m[1] * m[7] * m[10] +
              m[5] * m[2] * m[11] -
              m[5] * m[3] * m[10] -
              m[9] * m[2] * m[7] +
              m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] -
             m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] +
               m[0] * m[7] * m[9] +
               m[4] * m[1] * m[11] -
               m[4] * m[3] * m[9] -
               m[8] * m[1] * m[7] +
               m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] -
              m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    // clang-format on

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0) {
        return false;
    }

    det = 1.0 / det;

    for (i = 0; i < 16; i++) {
        invOut[i] = inv[i] * det;
    }

    return true;
}
