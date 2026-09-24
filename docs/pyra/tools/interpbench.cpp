// Microbenchmark (§8.30): frame interpolation's recording store. Replays a real game tick's call
// stream (SOH_BENCH_INTERP_TRACE, see soh/soh/frame_interpolation.cpp) through three stores and
// times record, the per-tick reset (previous = move(current), which destroys the older tree) and
// the interpolate walk. Replacement maps are compared entry by entry against the shipped store.
//
//   A  the shipped tree: Path of std::map/std::vector, copied from frame_interpolation.cpp
//   B  the same tree in std::pmr containers on a per-recording monotonic buffer, released
//      wholesale (no destructors run; the buffer is kept for the next tick)
//   C  the same tree in std containers on a bump allocator (static, no virtual calls), released
//      wholesale the same way
//
// The trace gives the shape: which children open where, keys, ops and their order. Values are
// synthesised (they do not change the containers' work), and the two traced ticks alternate so the
// walk always matches old against new. Keys keep the game's pointer order.
//
// Build (device), one line: docker run --rm -v "$PWD:/mb" -w /mb soh-pyra-cross arm-linux-gnueabihf-g++ -O3
//   -DNDEBUG -std=c++20 -march=armv7ve+neon-vfpv4 -mtune=cortex-a15 -mfloat-abi=hard -o interpbench interpbench.cpp
// Run on the device with the CPU pinned (userspace governor, scaling_setspeed=1250000):
//   taskset -c 1 ./interpbench market.trace [ticks]
// A positive control for the comparison: ./interpbench market.trace 200 corrupt
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <map>
#include <memory_resource>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

typedef float f32;
typedef int16_t s16;
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
struct MtxF { float mf[4][4]; };
struct Mtx { int32_t m[4][4]; };
struct Vec3f { f32 x, y, z; };
struct Vec3s { s16 x, y, z; };

// Allocation counter: every operator new the stores make (map nodes, vector growth).
static uint64_t gNews;
void* operator new(size_t n) {
    gNews++;
    if (void* p = malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

// ---------------------------------------------------------------------------------------------
// A stand-in for sys_matrix.c: a real matrix stack doing comparable arithmetic, so the walk has
// the game's work around its lookups. Identical for every store.
// ---------------------------------------------------------------------------------------------
static MtxF sStack[64];
static MtxF* sCur = sStack;
static void Identity(MtxF* m) { memset(m, 0, sizeof(*m)); for (int i = 0; i < 4; i++) m->mf[i][i] = 1; }
static void SkinMatrix_MtxFMtxFMult(MtxF* a, MtxF* b, MtxF* dest) {
    MtxF r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.mf[i][j] = a->mf[0][j] * b->mf[i][0] + a->mf[1][j] * b->mf[i][1] + a->mf[2][j] * b->mf[i][2] + a->mf[3][j] * b->mf[i][3];
    *dest = r;
}
static MtxF* Matrix_GetCurrent() { return sCur; }
static void Matrix_Push() { sCur[1] = sCur[0]; sCur++; if (sCur >= sStack + 63) sCur = sStack + 62; }
static void Matrix_Pop() { if (sCur > sStack) sCur--; }
static void Matrix_Put(MtxF* s) { *sCur = *s; }
static void Matrix_Mult(MtxF* m, u8 mode) { if (mode) SkinMatrix_MtxFMtxFMult(sCur, m, sCur); else *sCur = *m; }
static void Matrix_Translate(f32 x, f32 y, f32 z, u8 mode) {
    MtxF t; Identity(&t); t.mf[3][0] = x; t.mf[3][1] = y; t.mf[3][2] = z; Matrix_Mult(&t, mode);
}
static void Matrix_Scale(f32 x, f32 y, f32 z, u8 mode) {
    MtxF t; Identity(&t); t.mf[0][0] = x; t.mf[1][1] = y; t.mf[2][2] = z; Matrix_Mult(&t, mode);
}
static void Rot(int axis, f32 a, u8 mode) {
    MtxF t; Identity(&t); float s = sinf(a), c = cosf(a);
    int p = (axis + 1) % 3, q = (axis + 2) % 3;
    t.mf[p][p] = c; t.mf[p][q] = s; t.mf[q][p] = -s; t.mf[q][q] = c; Matrix_Mult(&t, mode);
}
static void Matrix_RotateX(f32 a, u8 m) { Rot(0, a, m); }
static void Matrix_RotateY(f32 a, u8 m) { Rot(1, a, m); }
static void Matrix_RotateZ(f32 a, u8 m) { Rot(2, a, m); }
static const float kS2R = 3.14159265f / 32768.0f;
static void Matrix_RotateZYX(s16 x, s16 y, s16 z, u8 mode) { Rot(2, z * kS2R, mode); Rot(1, y * kS2R, 1); Rot(0, x * kS2R, 1); }
static void Matrix_TranslateRotateZYX(Vec3f* t, Vec3s* r) { Matrix_Translate(t->x, t->y, t->z, 1); Matrix_RotateZYX(r->x, r->y, r->z, 1); }
static void Matrix_SetTranslateRotateYXZ(f32 x, f32 y, f32 z, Vec3s* r) {
    Matrix_Translate(x, y, z, 0); Rot(1, r->y * kS2R, 1); Rot(0, r->x * kS2R, 1); Rot(2, r->z * kS2R, 1);
}
static void Matrix_ReplaceRotation(MtxF* m) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) sCur->mf[i][j] = m->mf[i][j]; }
static void Matrix_RotateAxis(f32 a, Vec3f* ax, u8 mode) { Rot(0, a * ax->x, mode); Rot(1, a * ax->y, 1); }

// https://stackoverflow.com/questions/1148309/inverting-a-4x4-matrix - as in the game, abridged to
// the same amount of arithmetic (the record path calls it once per actor).
static bool invert_matrix(const float m[16], float out[16]) {
    float inv[16];
    for (int i = 0; i < 16; i++) {
        int r = i >> 2, c = i & 3;
        inv[i] = m[(r + 1) % 4 * 4 + (c + 1) % 4] * m[(r + 2) % 4 * 4 + (c + 2) % 4] * m[(r + 3) % 4 * 4 + (c + 3) % 4] -
                 m[(r + 1) % 4 * 4 + (c + 3) % 4] * m[(r + 2) % 4 * 4 + (c + 2) % 4] * m[(r + 3) % 4 * 4 + (c + 1) % 4] +
                 m[(r + 2) % 4 * 4 + (c + 1) % 4] * m[(r + 3) % 4 * 4 + (c + 2) % 4] * m[(r + 1) % 4 * 4 + (c + 3) % 4] -
                 m[(r + 2) % 4 * 4 + (c + 3) % 4] * m[(r + 3) % 4 * 4 + (c + 2) % 4] * m[(r + 1) % 4 * 4 + (c + 1) % 4];
    }
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
    return true;
}

// ---------------------------------------------------------------------------------------------
// The recording's data types, verbatim from frame_interpolation.cpp.
// ---------------------------------------------------------------------------------------------
enum class Op {
    OpenChild, CloseChild, MatrixPush, MatrixPop, MatrixPut, MatrixMult, MatrixTranslate, MatrixScale,
    MatrixRotate1Coord, MatrixRotateZYX, MatrixTranslateRotateZYX, MatrixSetTranslateRotateYXZ,
    MatrixMtxFToMtx, MatrixToMtx, MatrixReplaceRotation, MatrixRotateAxis, SkinMatrixMtxFToMtx
};

typedef std::pair<const void*, int> label;

union Data {
    Data() {}
    struct { MtxF src; } matrix_put;
    struct { MtxF mf; u8 mode; } matrix_mult;
    struct { f32 x, y, z; u8 mode; } matrix_translate, matrix_scale;
    struct { u32 coord; f32 value; u8 mode; } matrix_rotate_1_coord;
    struct { s16 x, y, z; u8 mode; } matrix_rotate_zyx;
    struct { Vec3f translation; Vec3s rotation; } matrix_translate_rotate_zyx;
    struct { f32 translateX, translateY, translateZ; Vec3s rot; bool has_mtx; } matrix_set_translate_rotate_yxz;
    struct { MtxF src; Mtx* dest; } matrix_mtxf_to_mtx;
    struct { Mtx* dest; MtxF src; bool has_adjusted; } matrix_to_mtx;
    struct { MtxF mf; } matrix_replace_rotation;
    struct { f32 angle; Vec3f axis; u8 mode; } matrix_rotate_axis;
    struct { label key; size_t idx; } open_child;
};

// ---------------------------------------------------------------------------------------------
// Container families. Path is the game's struct, parameterised on the containers it is made of.
// ---------------------------------------------------------------------------------------------
struct StdFamily {
    template <class T> using vector = std::vector<T>;
    template <class K, class V> using map = std::map<K, V>;
};

struct PmrFamily {
    template <class T> using vector = std::pmr::vector<T>;
    template <class K, class V> using map = std::pmr::map<K, V>;
};

// C's allocator: bumps through the current recording's arena, never frees.
struct Arena {
    char* base = nullptr;
    size_t size = 0, used = 0, high = 0;
    std::vector<char*> spill;
    void* alloc(size_t n, size_t align) {
        size_t p = (used + align - 1) & ~(align - 1);
        if (p + n > size) { // overflow: rare after the first ticks, which size the arena
            char* s = (char*)::operator new(n);
            spill.push_back(s);
            high = std::max(high, p + n);
            return s;
        }
        used = p + n;
        high = std::max(high, used);
        return base + p;
    }
    void reset() {
        for (char* s : spill) ::operator delete(s);
        spill.clear();
        if (high > size) { ::operator delete(base); size = high * 2; base = (char*)::operator new(size); }
        used = 0;
    }
};
static Arena* gBumpArena;
template <class T> struct BumpAlloc {
    typedef T value_type;
    BumpAlloc() = default;
    template <class U> BumpAlloc(const BumpAlloc<U>&) {}
    T* allocate(size_t n) { return (T*)gBumpArena->alloc(n * sizeof(T), alignof(T)); }
    void deallocate(T*, size_t) {}
    template <class U> bool operator==(const BumpAlloc<U>&) const { return true; }
    template <class U> bool operator!=(const BumpAlloc<U>&) const { return false; }
};
struct BumpFamily {
    template <class T> using vector = std::vector<T, BumpAlloc<T>>;
    template <class K, class V> using map = std::map<K, V, std::less<K>, BumpAlloc<std::pair<const K, V>>>;
};

template <class F> struct PathT {
    typename F::template map<label, typename F::template vector<PathT>> children;
    typename F::template map<Op, typename F::template vector<Data>> ops;
    typename F::template vector<std::pair<Op, size_t>> items;
};

// pmr needs Path to take an allocator so the containers nested in it share the recording's.
template <> struct PathT<PmrFamily> {
    using allocator_type = std::pmr::polymorphic_allocator<>;
    std::pmr::map<label, std::pmr::vector<PathT>> children;
    std::pmr::map<Op, std::pmr::vector<Data>> ops;
    std::pmr::vector<std::pair<Op, size_t>> items;
    explicit PathT(const allocator_type& a = {}) : children(a), ops(a), items(a) {}
    PathT(PathT&& o, const allocator_type& a)
        : children(std::move(o.children), a), ops(std::move(o.ops), a), items(std::move(o.items), a) {}
    PathT(PathT&& o) noexcept = default;
};

// ---------------------------------------------------------------------------------------------
// The recorder and the walk, the game's code over a Path family. Only StartRecord (the reset)
// differs between stores.
// ---------------------------------------------------------------------------------------------
template <class F> struct Store {
    typedef PathT<F> Path;

    // Game state (frame_interpolation.cpp's globals).
    bool is_recording = false;
    std::vector<Path*> current_path;
    Path* current_root = nullptr;
    Path* previous_root = nullptr;
    bool next_is_actor_pos_rot_matrix = false;
    bool has_inv_actor_mtx = false;
    MtxF inv_actor_mtx = {};
    size_t inv_actor_mtx_path_index = 0;
    uint64_t adjusted = 0; // how many ToMtx took the has_adjusted path; must match the trace's "a" lines

    Data& append(Op op) {
        auto& m = current_path.back()->ops[op];
        current_path.back()->items.emplace_back(op, m.size());
        return m.emplace_back();
    }

    void RecordOpenChild(const void* a, int b) {
        label key = { a, b };
        auto& m = current_path.back()->children[key];
        append(Op::OpenChild).open_child = { key, m.size() };
        current_path.push_back(&m.emplace_back());
    }
    void RecordCloseChild() {
        if (has_inv_actor_mtx && current_path.size() == inv_actor_mtx_path_index) has_inv_actor_mtx = false;
        current_path.pop_back();
    }
    void RecordMatrixPush() { append(Op::MatrixPush); }
    void RecordMatrixPop() { append(Op::MatrixPop); }
    void RecordMatrixPut(MtxF* src) { append(Op::MatrixPut).matrix_put = { *src }; }
    void RecordMatrixMult(MtxF* mf, u8 mode) { append(Op::MatrixMult).matrix_mult = { *mf, mode }; }
    void RecordMatrixTranslate(f32 x, f32 y, f32 z, u8 mode) { append(Op::MatrixTranslate).matrix_translate = { x, y, z, mode }; }
    void RecordMatrixScale(f32 x, f32 y, f32 z, u8 mode) { append(Op::MatrixScale).matrix_scale = { x, y, z, mode }; }
    void RecordMatrixRotate1Coord(u32 coord, f32 value, u8 mode) { append(Op::MatrixRotate1Coord).matrix_rotate_1_coord = { coord, value, mode }; }
    void RecordMatrixRotateZYX(s16 x, s16 y, s16 z, u8 mode) { append(Op::MatrixRotateZYX).matrix_rotate_zyx = { x, y, z, mode }; }
    void RecordMatrixTranslateRotateZYX(Vec3f* t, Vec3s* r) { append(Op::MatrixTranslateRotateZYX).matrix_translate_rotate_zyx = { *t, *r }; }
    void RecordMatrixSetTranslateRotateYXZ(f32 tx, f32 ty, f32 tz, Vec3s* rot) {
        auto& d = append(Op::MatrixSetTranslateRotateYXZ).matrix_set_translate_rotate_yxz = { tx, ty, tz, *rot };
        if (next_is_actor_pos_rot_matrix) {
            d.has_mtx = true;
            invert_matrix((const float*)Matrix_GetCurrent()->mf, (float*)inv_actor_mtx.mf);
            next_is_actor_pos_rot_matrix = false;
            has_inv_actor_mtx = true;
            inv_actor_mtx_path_index = current_path.size();
        }
    }
    void RecordMatrixMtxFToMtx(MtxF* src, Mtx* dest) { append(Op::MatrixMtxFToMtx).matrix_mtxf_to_mtx = { *src, dest }; }
    void RecordMatrixToMtx(Mtx* dest) {
        auto& d = append(Op::MatrixToMtx).matrix_to_mtx = { dest };
        if (has_inv_actor_mtx) {
            d.has_adjusted = true;
            adjusted++;
            SkinMatrix_MtxFMtxFMult(&inv_actor_mtx, Matrix_GetCurrent(), &d.src);
        } else {
            d.src = *Matrix_GetCurrent();
        }
    }
    void RecordMatrixReplaceRotation(MtxF* mf) { append(Op::MatrixReplaceRotation).matrix_replace_rotation = { *mf }; }
    void RecordMatrixRotateAxis(f32 angle, Vec3f* axis, u8 mode) { append(Op::MatrixRotateAxis).matrix_rotate_axis = { angle, *axis, mode }; }

    // ---- the walk, verbatim ----
    struct InterpolateCtx {
        float step, w;
        std::unordered_map<Mtx*, MtxF> mtx_replacements;
        MtxF tmp_mtxf, tmp_mtxf2;
        Vec3f tmp_vec3f;
        Vec3s tmp_vec3s;
        MtxF actor_mtx;

        MtxF* new_replacement(Mtx* addr) { return &mtx_replacements[addr]; }
        void interpolate_mtxf(MtxF* res, MtxF* o, MtxF* n) {
            for (size_t i = 0; i < 4; i++)
                for (size_t j = 0; j < 4; j++) res->mf[i][j] = w * o->mf[i][j] + step * n->mf[i][j];
        }
        float lerp(f32 o, f32 n) { return w * o + step * n; }
        void lerp_vec3f(Vec3f* res, Vec3f* o, Vec3f* n) { res->x = lerp(o->x, n->x); res->y = lerp(o->y, n->y); res->z = lerp(o->z, n->z); }
        float interpolate_angle(f32 o, f32 n) {
            if (o == n) return n;
            o = fmodf(o, 2 * M_PI); if (o < 0.0f) o += 2 * M_PI;
            n = fmodf(n, 2 * M_PI); if (n < 0.0f) n += 2 * M_PI;
            if (fabsf(o - n) > M_PI) { if (o < n) o += 2 * M_PI; else n += 2 * M_PI; }
            return lerp(o, n);
        }
        s16 interpolate_angle(s16 os, s16 ns) {
            if (os == ns) return ns;
            int o = (uint16_t)os, n = (uint16_t)ns;
            uint16_t res;
            int diff = o - n;
            if (-0x8000 <= diff && diff <= 0x8000) {
                if (diff < -0x4000 || diff > 0x4000) return ns;
                res = (uint16_t)(w * o + step * n);
            } else {
                if (o < n) o += 0x10000; else n += 0x10000;
                diff = o - n;
                if (diff < -0x4000 || diff > 0x4000) return ns;
                res = (uint16_t)(w * o + step * n);
            }
            return res;
        }
        void interpolate_angles(Vec3s* res, Vec3s* o, Vec3s* n) {
            res->x = interpolate_angle(o->x, n->x); res->y = interpolate_angle(o->y, n->y); res->z = interpolate_angle(o->z, n->z);
        }

        void interpolate_branch(Path* old_path, Path* new_path) {
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
                            case Op::OpenChild: case Op::CloseChild: case Op::SkinMatrixMtxFToMtx: break;
                            case Op::MatrixPush: Matrix_Push(); break;
                            case Op::MatrixPop: Matrix_Pop(); break;
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
                                                 lerp(old_op.matrix_translate.z, new_op.matrix_translate.z), new_op.matrix_translate.mode);
                                break;
                            case Op::MatrixScale:
                                Matrix_Scale(lerp(old_op.matrix_scale.x, new_op.matrix_scale.x), lerp(old_op.matrix_scale.y, new_op.matrix_scale.y),
                                             lerp(old_op.matrix_scale.z, new_op.matrix_scale.z), new_op.matrix_scale.mode);
                                break;
                            case Op::MatrixRotate1Coord: {
                                float v = interpolate_angle(old_op.matrix_rotate_1_coord.value, new_op.matrix_rotate_1_coord.value);
                                u8 mode = new_op.matrix_rotate_1_coord.mode;
                                switch (new_op.matrix_rotate_1_coord.coord) {
                                    case 0: Matrix_RotateX(v, mode); break;
                                    case 1: Matrix_RotateY(v, mode); break;
                                    case 2: Matrix_RotateZ(v, mode); break;
                                }
                                break;
                            }
                            case Op::MatrixRotateZYX:
                                Matrix_RotateZYX(interpolate_angle(old_op.matrix_rotate_zyx.x, new_op.matrix_rotate_zyx.x),
                                                 interpolate_angle(old_op.matrix_rotate_zyx.y, new_op.matrix_rotate_zyx.y),
                                                 interpolate_angle(old_op.matrix_rotate_zyx.z, new_op.matrix_rotate_zyx.z), new_op.matrix_rotate_zyx.mode);
                                break;
                            case Op::MatrixTranslateRotateZYX:
                                lerp_vec3f(&tmp_vec3f, &old_op.matrix_translate_rotate_zyx.translation, &new_op.matrix_translate_rotate_zyx.translation);
                                interpolate_angles(&tmp_vec3s, &old_op.matrix_translate_rotate_zyx.rotation, &new_op.matrix_translate_rotate_zyx.rotation);
                                Matrix_TranslateRotateZYX(&tmp_vec3f, &tmp_vec3s);
                                break;
                            case Op::MatrixSetTranslateRotateYXZ:
                                interpolate_angles(&tmp_vec3s, &old_op.matrix_set_translate_rotate_yxz.rot, &new_op.matrix_set_translate_rotate_yxz.rot);
                                Matrix_SetTranslateRotateYXZ(lerp(old_op.matrix_set_translate_rotate_yxz.translateX, new_op.matrix_set_translate_rotate_yxz.translateX),
                                                             lerp(old_op.matrix_set_translate_rotate_yxz.translateY, new_op.matrix_set_translate_rotate_yxz.translateY),
                                                             lerp(old_op.matrix_set_translate_rotate_yxz.translateZ, new_op.matrix_set_translate_rotate_yxz.translateZ),
                                                             &tmp_vec3s);
                                if (new_op.matrix_set_translate_rotate_yxz.has_mtx && old_op.matrix_set_translate_rotate_yxz.has_mtx)
                                    actor_mtx = *Matrix_GetCurrent();
                                break;
                            case Op::MatrixMtxFToMtx:
                                interpolate_mtxf(new_replacement(new_op.matrix_mtxf_to_mtx.dest), &old_op.matrix_mtxf_to_mtx.src, &new_op.matrix_mtxf_to_mtx.src);
                                break;
                            case Op::MatrixToMtx:
                                if (old_op.matrix_to_mtx.has_adjusted && new_op.matrix_to_mtx.has_adjusted) {
                                    interpolate_mtxf(&tmp_mtxf, &old_op.matrix_to_mtx.src, &new_op.matrix_to_mtx.src);
                                    SkinMatrix_MtxFMtxFMult(&actor_mtx, &tmp_mtxf, new_replacement(new_op.matrix_to_mtx.dest));
                                } else {
                                    interpolate_mtxf(new_replacement(new_op.matrix_to_mtx.dest), &old_op.matrix_to_mtx.src, &new_op.matrix_to_mtx.src);
                                }
                                break;
                            case Op::MatrixReplaceRotation:
                                interpolate_mtxf(&tmp_mtxf, &old_op.matrix_replace_rotation.mf, &new_op.matrix_replace_rotation.mf);
                                Matrix_ReplaceRotation(&tmp_mtxf);
                                break;
                            case Op::MatrixRotateAxis:
                                lerp_vec3f(&tmp_vec3f, &old_op.matrix_rotate_axis.axis, &new_op.matrix_rotate_axis.axis);
                                Matrix_RotateAxis(interpolate_angle(old_op.matrix_rotate_axis.angle, new_op.matrix_rotate_axis.angle), &tmp_vec3f,
                                                  new_op.matrix_rotate_axis.mode);
                                break;
                        }
                    }
                }
            }
        }
    };

    std::unordered_map<Mtx*, MtxF> Interpolate(float step) {
        InterpolateCtx ctx;
        ctx.step = step;
        ctx.w = 1.0f - step;
        ctx.interpolate_branch(previous_root, current_root);
        return ctx.mtx_replacements;
    }
};

// A: previous_recording = std::move(current_recording); current_recording = {}; - the game's reset.
struct StoreA : Store<StdFamily> {
    struct Recording { Path root_path; };
    Recording current_recording, previous_recording;
    StoreA() { current_root = &current_recording.root_path; previous_root = &previous_recording.root_path; }
    void StartRecord() {
        previous_recording = std::move(current_recording);
        current_recording = {};
        current_path.clear();
        current_path.push_back(&current_recording.root_path);
    }
};

// B: two recordings, each a Path in its own monotonic buffer. The reset drops the older one's
// buffer wholesale and starts the next recording in it; no destructor walks the tree.
struct StoreB : Store<PmrFamily> {
    struct Rec {
        std::vector<char> buf;
        std::pmr::monotonic_buffer_resource* res = nullptr;
        Path* root = nullptr;
        size_t grow = 256 * 1024;
        void start() {
            if (res != nullptr) {
                res->~monotonic_buffer_resource(); // frees any upstream overflow chunks
            }
            res = new (resStorage) std::pmr::monotonic_buffer_resource(buf.data(), buf.size());
            root = new (rootStorage) Path(std::pmr::polymorphic_allocator<>(res));
        }
        alignas(std::pmr::monotonic_buffer_resource) char resStorage[sizeof(std::pmr::monotonic_buffer_resource)];
        alignas(Path) char rootStorage[sizeof(Path)];
    };
    Rec rec[2];
    int cur = 0;
    StoreB(size_t bytes) {
        for (auto& r : rec) { r.buf.resize(bytes); r.start(); }
        current_root = rec[0].root;
        previous_root = rec[1].root;
    }
    void StartRecord() {
        cur ^= 1;
        rec[cur].start(); // the older recording's memory is reused; its tree is simply forgotten
        previous_root = rec[cur ^ 1].root;
        current_root = rec[cur].root;
        current_path.clear();
        current_path.push_back(current_root);
    }
};

// C: the same, with a bump allocator in place of pmr.
struct StoreC : Store<BumpFamily> {
    Arena arena[2];
    alignas(Path) char rootStorage[2][sizeof(Path)];
    int cur = 0;
    StoreC(size_t bytes) {
        for (int i = 0; i < 2; i++) {
            arena[i].size = bytes;
            arena[i].base = (char*)::operator new(bytes);
            gBumpArena = &arena[i];
            new (rootStorage[i]) Path();
        }
        gBumpArena = &arena[0];
        current_root = (Path*)rootStorage[0];
        previous_root = (Path*)rootStorage[1];
    }
    void StartRecord() {
        cur ^= 1;
        arena[cur].reset();
        gBumpArena = &arena[cur];
        new (rootStorage[cur]) Path();
        previous_root = (Path*)rootStorage[cur ^ 1];
        current_root = (Path*)rootStorage[cur];
        current_path.clear();
        current_path.push_back(current_root);
    }
};

// ---------------------------------------------------------------------------------------------
// The trace and its replay.
// ---------------------------------------------------------------------------------------------
struct Ev {
    char kind; // 'O', 'C', 'o'
    int op;    // for 'o'
    const void* ptr;
    int arg;
    bool flag_m; // SetTranslateRotateYXZ preceded by RecordActorPosRotMatrix
};
struct Tick { std::vector<Ev> ev; uint64_t adjusted = 0; size_t children = 0, ops = 0; };

static std::vector<Tick> LoadTrace(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    std::vector<Tick> ticks;
    std::vector<std::pair<uintptr_t, std::pair<size_t, size_t>>> ptrs; // value -> (tick, ev)
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'T') { ticks.emplace_back(); continue; }
        if (ticks.empty()) continue;
        Tick& t = ticks.back();
        if (line[0] == 'O') {
            unsigned long long p = 0; int b = 0;
            char* s = line + 2;
            if (strncmp(s, "(nil)", 5) == 0) { p = 0; s += 5; } else p = strtoull(s, &s, 16);
            b = atoi(s);
            t.ev.push_back({ 'O', 0, nullptr, b, false });
            ptrs.push_back({ (uintptr_t)p, { ticks.size() - 1, t.ev.size() - 1 } });
            t.children++;
        } else if (line[0] == 'C') {
            t.ev.push_back({ 'C', 0, nullptr, 0, false });
        } else if (line[0] == 'o') {
            t.ev.push_back({ 'o', atoi(line + 2), nullptr, 0, false });
            t.ops++;
        } else if (line[0] == 'a') {
            t.adjusted++;
        } else if (line[0] == 'm') {
            // written after the op it belongs to
            if (!t.ev.empty()) t.ev.back().flag_m = true;
        }
    }
    fclose(f);
    // Keys become fake pointers that keep the game's order (0 stays 0).
    std::vector<uintptr_t> vals;
    for (auto& p : ptrs) vals.push_back(p.first);
    std::sort(vals.begin(), vals.end());
    vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    for (auto& p : ptrs) {
        size_t rank = std::lower_bound(vals.begin(), vals.end(), p.first) - vals.begin();
        ticks[p.second.first].ev[p.second.second].ptr = p.first == 0 ? nullptr : (const void*)(uintptr_t)(0x100000 + rank * 64);
    }
    return ticks;
}

// Synthesised values, a function of (tick, op sequence), so both stores see identical input and
// consecutive ticks differ.
static MtxF gMtxPool[2][8];
static Mtx gDest[2][4096];

// The game does milliseconds of other work between record calls; the shipped tree's nodes are
// scattered across the heap and go cold in between. `thrash` KB of unrelated memory is touched after
// every event to model that; its own cost is timed alone and subtracted.
static std::vector<char> gThrash;
static size_t gThrashPos;
static inline void Thrash() {
    const size_t n = gThrash.size();
    if (n == 0) return;
    for (size_t i = 0; i < n; i += 64) {
        gThrash[gThrashPos] += 1;
        gThrashPos += 64 * 17; // defeat the prefetcher a little
        if (gThrashPos >= n) gThrashPos -= n;
    }
}

// One tick's recording, after StartRecord. `corrupt` perturbs the first ToMtx (positive control).
template <class S> static void ReplayBody(S& s, const Tick& t, int parity, bool corrupt) {
    s.is_recording = true;
    int seq = 0, dests = 0;
    for (const Ev& e : t.ev) {
        Thrash();
        seq++;
        MtxF* m = &gMtxPool[parity][seq & 7];
        float v = (seq & 63) * 0.25f + parity * 0.5f;
        switch (e.kind) {
            case 'O': s.RecordOpenChild(e.ptr, e.arg); break;
            case 'C': s.RecordCloseChild(); break;
            case 'o': {
                Op op = (Op)e.op;
                u8 mode = seq & 1;
                Vec3f vf = { v, -v, v * 2 };
                Vec3s vs = { (s16)(seq * 97 + parity * 300), (s16)(seq * 31), (s16)(parity * 1000 - seq) };
                switch (op) {
                    case Op::MatrixPush: s.RecordMatrixPush(); Matrix_Push(); break;
                    case Op::MatrixPop: s.RecordMatrixPop(); Matrix_Pop(); break;
                    case Op::MatrixPut: s.RecordMatrixPut(m); Matrix_Put(m); break;
                    case Op::MatrixMult: s.RecordMatrixMult(m, mode); break;
                    case Op::MatrixTranslate: s.RecordMatrixTranslate(v, v, -v, mode); break;
                    case Op::MatrixScale: s.RecordMatrixScale(v, v, v, mode); break;
                    case Op::MatrixRotate1Coord: s.RecordMatrixRotate1Coord(seq % 3, v, mode); break;
                    case Op::MatrixRotateZYX: s.RecordMatrixRotateZYX(vs.x, vs.y, vs.z, mode); break;
                    case Op::MatrixTranslateRotateZYX: s.RecordMatrixTranslateRotateZYX(&vf, &vs); break;
                    case Op::MatrixSetTranslateRotateYXZ:
                        if (e.flag_m) s.next_is_actor_pos_rot_matrix = true;
                        s.RecordMatrixSetTranslateRotateYXZ(v, v, v, &vs);
                        break;
                    case Op::MatrixMtxFToMtx: s.RecordMatrixMtxFToMtx(m, &gDest[parity][dests++ & 4095]); break;
                    case Op::MatrixToMtx: s.RecordMatrixToMtx(&gDest[parity][dests++ & 4095]); break;
                    case Op::MatrixReplaceRotation: s.RecordMatrixReplaceRotation(m); break;
                    case Op::MatrixRotateAxis: s.RecordMatrixRotateAxis(v, &vf, mode); break;
                    default: break;
                }
                if (corrupt && op == Op::MatrixToMtx) {
                    s.current_path.back()->ops[op].back().matrix_to_mtx.src.mf[1][2] += 1.0f;
                    corrupt = false;
                }
                break;
            }
        }
    }
    s.is_recording = false;
}

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

struct Times { double record = 1e9, reset = 1e9, walk = 1e9; };

// One store over `ticks` ticks, best of 5 rounds: the reset, the recording, and the mean of the two
// walks each tick feeds (InterpolationFPS=30: steps 1/3 and 2/3). Round 0 keeps the maps.
template <class S> static Times Run(S& s, const std::vector<Tick>& tr, int ticks, bool corrupt,
                                    std::vector<std::unordered_map<Mtx*, MtxF>>* maps) {
    Times best;
    for (int round = 0; round < 5; round++) {
        double rec = 0, rst = 0, walk = 0;
        for (int k = 0; k < ticks; k++) {
            const int parity = k & 1;
            const Tick& t = tr[parity % tr.size()];
            double t0 = now();
            s.StartRecord();
            double t1 = now();
            sCur = sStack; Identity(sCur);
            ReplayBody(s, t, parity, corrupt && round == 0 && k == ticks - 1);
            double t2 = now();
            sCur = sStack; Identity(sCur);
            auto m1 = s.Interpolate(1.0f / 3);
            sCur = sStack; Identity(sCur);
            auto m2 = s.Interpolate(2.0f / 3);
            double t3 = now();
            rst += t1 - t0; rec += t2 - t1; walk += (t3 - t2) / 2;
            if (maps && round == 0 && k >= 1) { maps->push_back(std::move(m1)); maps->push_back(std::move(m2)); }
            __asm__ volatile("" ::: "memory");
        }
        best.record = std::min(best.record, rec / ticks);
        best.reset = std::min(best.reset, rst / ticks);
        best.walk = std::min(best.walk, walk / ticks);
    }
    return best;
}

typedef std::vector<std::unordered_map<Mtx*, MtxF>> Maps;

// The thrash alone, per tick, for subtraction.
static double ThrashOnly(const std::vector<Tick>& tr, int ticks) {
    double best = 1e9;
    for (int round = 0; round < 5; round++) {
        double t0 = now();
        for (int k = 0; k < ticks; k++)
            for (size_t e = 0; e < tr[(k & 1) % tr.size()].ev.size(); e++) Thrash();
        best = std::min(best, (now() - t0) / ticks);
    }
    return best;
}

// Entry-by-entry: same keys, bit-identical matrices.
static size_t Compare(const Maps& a, const Maps& b, size_t* entries) {
    size_t bad = 0;
    *entries = 0;
    if (a.size() != b.size()) return (size_t)-1;
    for (size_t i = 0; i < a.size(); i++) {
        *entries += a[i].size();
        if (a[i].size() != b[i].size()) { bad++; continue; }
        for (auto& [k, v] : a[i]) {
            auto it = b[i].find(k);
            if (it == b[i].end() || memcmp(&it->second, &v, sizeof v) != 0) bad++;
        }
    }
    return bad;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s trace [ticks] [corrupt | thrashKB] [mt]\n", argv[0]); return 1; }
    // "mt": park a second thread, as the game has many. glibc's malloc skips its arena locks while a
    // process is single-threaded, so a single-threaded benchmark prices allocation too low.
    std::thread parked;
    bool stop = false;
    if (argc > 4 && strcmp(argv[4], "mt") == 0) {
        parked = std::thread([&] { while (!__atomic_load_n(&stop, __ATOMIC_RELAXED)) usleep(100000); });
    }
    const std::vector<Tick> tr = LoadTrace(argv[1]);
    const int ticks = argc > 2 ? atoi(argv[2]) : 200;
    const bool corrupt = argc > 3 && strcmp(argv[3], "corrupt") == 0;
    const int thrashKB = argc > 3 && !corrupt ? atoi(argv[3]) : 0;
    gThrash.assign((size_t)thrashKB * 1024, 0);
    const double thrash = thrashKB ? ThrashOnly(tr, ticks) : 0;
    if (tr.empty()) { fprintf(stderr, "no ticks in trace\n"); return 1; }
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 8; i++)
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++) gMtxPool[p][i].mf[r][c] = (r == c) + 0.01f * (i + 1) * (r - c) + p * 0.125f;

    size_t items = 0;
    for (size_t i = 0; i < tr.size(); i++) {
        printf("trace tick %zu: %zu children, %zu ops (%zu items), %llu adjusted ToMtx\n", i, tr[i].children, tr[i].ops,
               tr[i].children + tr[i].ops, (unsigned long long)tr[i].adjusted);
        items += tr[i].children + tr[i].ops;
    }
    items /= tr.size();

    Maps ma, mb, mc;
    uint64_t n0 = gNews;
    StoreA a;
    Times ta = Run(a, tr, ticks, false, &ma);
    uint64_t n1 = gNews;
    StoreB b(1 << 20);
    Times tb = Run(b, tr, ticks, false, &mb);
    uint64_t n2 = gNews;
    StoreC c(1 << 20);
    Times tc = Run(c, tr, ticks, corrupt, &mc);
    uint64_t n3 = gNews;
    // Includes the walks' unordered_map nodes (two walks a tick), the same for every store.
    printf("operator new per tick, record+walk: A %.0f  B %.0f  C %.0f\n", (double)(n1 - n0) / (5 * ticks),
           (double)(n2 - n1) / (5 * ticks), (double)(n3 - n2) / (5 * ticks));
    // The replay's has_adjusted count must equal the game's, or the actor nesting is not modelled.
    printf("bump arena high-water per recording: %zu KB (x86: 64-bit pointers; less on the device)\n", std::max(c.arena[0].high, c.arena[1].high) / 1024);
    printf("replayed adjusted ToMtx per tick: A %.1f  B %.1f  C %.1f\n", (double)a.adjusted / (5 * ticks),
           (double)b.adjusted / (5 * ticks), (double)c.adjusted / (5 * ticks));

    const char* names[3] = { "A std::map/vector (shipped)", "B pmr monotonic, dropped whole", "C bump allocator, dropped whole" };
    Times ts[3] = { ta, tb, tc };
    printf("\nper tick, %zu recorded items, best of 5 rounds x %d ticks, %d KB touched per event (%.1f us/tick, subtracted from record):\n",
           items, ticks, thrashKB, thrash * 1e6);
    for (Times& t : ts) t.record -= thrash;
    printf("  %-32s %9s %9s %9s %11s %9s\n", "", "reset us", "record us", "walk us", "rec+rst ns/item", "");
    for (int i = 0; i < 3; i++)
        printf("  %-32s %9.1f %9.1f %9.1f %11.0f\n", names[i], ts[i].reset * 1e6, ts[i].record * 1e6, ts[i].walk * 1e6,
               (ts[i].reset + ts[i].record) / items * 1e9);
    size_t e;
    size_t bb = Compare(ma, mb, &e);
    size_t bc = Compare(ma, mc, &e);
    printf("\nreplacement maps vs A: %zu maps, %zu entries; mismatched entries: B %zu, C %zu%s\n", ma.size(), e, bb, bc,
           corrupt ? "  (C corrupted: must be non-zero)" : "");
    __atomic_store_n(&stop, true, __ATOMIC_RELAXED);
    if (parked.joinable()) parked.join();
    return 0;
}
