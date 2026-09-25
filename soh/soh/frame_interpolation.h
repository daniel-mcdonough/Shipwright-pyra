#pragma once

#include "include/z64math.h"

#ifdef __cplusplus

#include <unordered_map>
#include <vector>

std::unordered_map<Mtx*, MtxF> FrameInterpolation_Interpolate(float step);

// Hands the tick's maps back once their frames are drawn, for LeanInterpMap=2 to reuse (§8.36).
void FrameInterpolation_RecycleMaps(std::vector<std::unordered_map<Mtx*, MtxF>>& maps);

#ifdef SOH_BENCHMARK
// Recording work since launch, monotonic (§8.29): child nodes opened and ops appended. Recording
// happens in Play_Draw, outside the timed window, so the benchmark reads these at a segment's
// ends rather than through the per-frame counters.
extern uint64_t gBenchInterpChildren;
extern uint64_t gBenchInterpOps;
extern uint64_t gBenchInterpCompared;
extern uint64_t gBenchInterpMismatch;
#endif

extern "C" {

#endif

void FrameInterpolation_StartRecord(void);

void FrameInterpolation_StopRecord(void);

void FrameInterpolation_RecordOpenChild(const void* a, int b);

void FrameInterpolation_RecordCloseChild(void);

void FrameInterpolation_DontInterpolateCamera(void);

int FrameInterpolation_GetCameraEpoch(void);

void FrameInterpolation_RecordActorPosRotMatrix(void);

void FrameInterpolation_RecordMatrixPush(void);

void FrameInterpolation_RecordMatrixPop(void);

void FrameInterpolation_RecordMatrixPut(MtxF* src);

void FrameInterpolation_RecordMatrixMult(MtxF* mf, u8 mode);

void FrameInterpolation_RecordMatrixTranslate(f32 x, f32 y, f32 z, u8 mode);

void FrameInterpolation_RecordMatrixScale(f32 x, f32 y, f32 z, u8 mode);

void FrameInterpolation_RecordMatrixRotate1Coord(u32 coord, f32 value, u8 mode);

void FrameInterpolation_RecordMatrixRotateZYX(s16 x, s16 y, s16 z, u8 mode);

void FrameInterpolation_RecordMatrixTranslateRotateZYX(Vec3f* translation, Vec3s* rotation);

void FrameInterpolation_RecordMatrixSetTranslateRotateYXZ(f32 translateX, f32 translateY, f32 translateZ, Vec3s* rot);

void FrameInterpolation_RecordMatrixMtxFToMtx(MtxF* src, Mtx* dest);

void FrameInterpolation_RecordMatrixToMtx(Mtx* dest, char* file, s32 line);

void FrameInterpolation_RecordMatrixReplaceRotation(MtxF* mf);

void FrameInterpolation_RecordMatrixRotateAxis(f32 angle, Vec3f* axis, u8 mode);

void FrameInterpolation_RecordSkinMatrixMtxFToMtx(MtxF* src, Mtx* dest);

#ifdef __cplusplus
}
#endif
