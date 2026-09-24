// Microbenchmark (§8.28): GfxSpVertex's directional lighting, four variants, bit-exactness checked.
// Build: docker run --rm -v "$PWD:/mb" -w /mb soh-pyra-cross arm-linux-gnueabihf-g++ -O3 \
//   -march=armv7ve+neon-vfpv4 -mtune=cortex-a15 -mfloat-abi=hard -o lightbench lightbench.cpp
// Run on the device with the CPU pinned (userspace governor, scaling_setspeed=1250000): taskset -c 1 ./lightbench
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

struct Vtx { int8_t n[3]; };
struct Out { uint8_t r, g, b; };

static const int NV = 6144, NL = 2;
static Vtx vtx[NV];
static float coeffs[NL][3];
static uint8_t col[NL + 1][3];
static Out outs[5][NV];

// A: the shape GfxSpVertex has today (int r, branch per light).
__attribute__((noinline)) void lightA(const Vtx* v, Out* o, int n, int nlights) {
    for (int k = 0; k < n; k++) {
        int r = col[nlights - 1][0], g = col[nlights - 1][1], b = col[nlights - 1][2];
        for (int i = 0; i < nlights - 1; i++) {
            float intensity = 0;
            intensity += v[k].n[0] * coeffs[i][0];
            intensity += v[k].n[1] * coeffs[i][1];
            intensity += v[k].n[2] * coeffs[i][2];
            intensity /= 127.0f;
            if (intensity > 0.0f) {
                r += intensity * col[i][0];
                g += intensity * col[i][1];
                b += intensity * col[i][2];
            }
        }
        o[k].r = r > 255 ? 255 : r;
        o[k].g = g > 255 ? 255 : g;
        o[k].b = b > 255 ? 255 : b;
    }
}

// Clear a float to +0 unless it is > 0, in integer registers: no FP compare, no VMRS.
static inline float PosOrZero(float x) {
    int32_t bits;
    memcpy(&bits, &x, 4);
    bits &= ~(bits >> 31);
    memcpy(&x, &bits, 4);
    return x;
}

// B: same shape, branchless. fma(col, +0, r) == r exactly, and r holds an int.
__attribute__((noinline)) void lightB(const Vtx* v, Out* o, int n, int nlights) {
    for (int k = 0; k < n; k++) {
        int r = col[nlights - 1][0], g = col[nlights - 1][1], b = col[nlights - 1][2];
        for (int i = 0; i < nlights - 1; i++) {
            float in = fmaf(v[k].n[2], coeffs[i][2], fmaf(v[k].n[1], coeffs[i][1], fmaf(v[k].n[0], coeffs[i][0], 0.0f))) / 127.0f;
            in = PosOrZero(in);
            r = (int)fmaf((float)col[i][0], in, (float)r);
            g = (int)fmaf((float)col[i][1], in, (float)g);
            b = (int)fmaf((float)col[i][2], in, (float)b);
        }
        o[k].r = r > 255 ? 255 : r;
        o[k].g = g > 255 ? 255 : g;
        o[k].b = b > 255 ? 255 : b;
    }
}

// C/D: four vertices at a time (what LeanLight=1 does), with the branch (C) or without (D).
template <bool Branchless>
__attribute__((noinline)) void lightBatch(const Vtx* v, Out* o, int n, int nlights) {
    const int nl = nlights - 1;
    float cf[8][3];
    for (int l = 0; l < nl; l++) for (int c = 0; c < 3; c++) cf[l][c] = col[l][c];
    for (int base = 0; base < n; base += 4) {
        float nx[4], ny[4], nz[4], r[4], g[4], b[4];
#pragma GCC unroll 4
        for (int j = 0; j < 4; j++) {
            nx[j] = v[base + j].n[0]; ny[j] = v[base + j].n[1]; nz[j] = v[base + j].n[2];
            r[j] = col[nl][0]; g[j] = col[nl][1]; b[j] = col[nl][2];
        }
        for (int l = 0; l < nl; l++) {
            const float* c = coeffs[l];
            float in[4];
#pragma GCC unroll 4
            for (int j = 0; j < 4; j++) in[j] = fmaf(nz[j], c[2], fmaf(ny[j], c[1], fmaf(nx[j], c[0], 0.0f))) / 127.0f;
#pragma GCC unroll 4
            for (int j = 0; j < 4; j++) {
                if (Branchless) {
                    const float p = PosOrZero(in[j]);
                    r[j] = (float)(int)fmaf(cf[l][0], p, r[j]);
                    g[j] = (float)(int)fmaf(cf[l][1], p, g[j]);
                    b[j] = (float)(int)fmaf(cf[l][2], p, b[j]);
                } else if (in[j] > 0.0f) {
                    r[j] = (float)(int)fmaf(cf[l][0], in[j], r[j]);
                    g[j] = (float)(int)fmaf(cf[l][1], in[j], g[j]);
                    b[j] = (float)(int)fmaf(cf[l][2], in[j], b[j]);
                }
            }
        }
        for (int j = 0; j < 4; j++) {
            int ri = (int)r[j], gi = (int)g[j], bi = (int)b[j];
            o[base + j].r = ri > 255 ? 255 : ri;
            o[base + j].g = gi > 255 ? 255 : gi;
            o[base + j].b = bi > 255 ? 255 : bi;
        }
    }
}

// The light-direction recompute: CalculateNormalDir (3 divides, transposed 3x3, normalise).
static float mtx[4][4];
__attribute__((noinline)) void recalc(const int8_t dir[3], float out[3]) {
    float d[3] = { dir[0] / 127.0f, dir[1] / 127.0f, dir[2] / 127.0f };
    for (int i = 0; i < 3; i++) out[i] = mtx[i][0] * d[0] + mtx[i][1] * d[1] + mtx[i][2] * d[2];
    float s = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    out[0] /= s; out[1] /= s; out[2] /= s;
}

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void setup(bool coherent) {
    srand(1);
    for (int k = 0; k < NV; k++) {
        float a = coherent ? k * 0.01f : (rand() / (float)RAND_MAX) * 6.283f;
        float e = coherent ? sinf(k * 0.003f) : (rand() / (float)RAND_MAX) * 2 - 1;
        float x = cosf(a) * sqrtf(1 - e * e), y = sinf(a) * sqrtf(1 - e * e), z = e;
        vtx[k].n[0] = (int8_t)(x * 127); vtx[k].n[1] = (int8_t)(y * 127); vtx[k].n[2] = (int8_t)(z * 127);
    }
    const float c0[3] = { 0.57f, 0.57f, 0.59f }, c1[3] = { -0.3f, 0.9f, -0.31f };
    for (int c = 0; c < 3; c++) { coeffs[0][c] = c0[c]; coeffs[1][c] = c1[c]; }
    const uint8_t cc[3][3] = { { 180, 170, 150 }, { 60, 70, 90 }, { 50, 50, 60 } };
    memcpy(col, cc, sizeof(cc));
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) mtx[i][j] = (i == j) ? 0.9f : 0.1f * (i - j);
}

int main(int argc, char** argv) {
    const int reps = argc > 1 ? atoi(argv[1]) : 400;
    for (int mode = 0; mode < 2; mode++) {
        setup(mode == 0);
        typedef void (*F)(const Vtx*, Out*, int, int);
        F fs[4] = { lightA, lightB, lightBatch<false>, lightBatch<true> };
        const char* names[4] = { "A inline+branch (today)", "B inline branchless", "C batch4+branch (LeanLight=1)", "D batch4 branchless" };
        double best[4] = { 1e9, 1e9, 1e9, 1e9 };
        for (int round = 0; round < 7; round++) {
            for (int f = 0; f < 4; f++) {
                double t0 = now();
                for (int r = 0; r < reps; r++) { fs[f](vtx, outs[f], NV, NL + 1); __asm__ volatile("" ::: "memory"); }
                double dt = (now() - t0) / reps / NV * 1e9;
                if (dt < best[f]) best[f] = dt;
            }
        }
        int pos = 0;
        for (int k = 0; k < NV; k++) for (int i = 0; i < NL; i++) {
            float in = (vtx[k].n[0] * coeffs[i][0] + vtx[k].n[1] * coeffs[i][1] + vtx[k].n[2] * coeffs[i][2]);
            pos += in > 0;
        }
        printf("%s normals, %.0f%% of (vertex,light) lit:\n", mode == 0 ? "coherent" : "random", 100.0 * pos / (NV * NL));
        for (int f = 0; f < 4; f++) {
            int mism = 0;
            for (int k = 0; k < NV; k++) mism += memcmp(&outs[f][k], &outs[0][k], 3) != 0;
            printf("  %-30s %6.1f ns/vertex  %5.0f cycles@1.25GHz  mismatches vs A: %d\n", names[f], best[f], best[f] * 1.25, mism);
        }
    }
    float o[3]; int8_t d[3] = { 40, -90, 70 };
    double t0 = now();
    for (int r = 0; r < 1000000; r++) { d[0] = r & 63; recalc(d, o); __asm__ volatile("" ::: "memory"); }
    printf("recalc (CalculateNormalDir-like): %.1f ns/call\n", (now() - t0) / 1e6 * 1e9);
    return 0;
}
