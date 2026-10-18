#include "global.h"

#if defined(SOH_BENCHMARK) || defined(SOH_BENCHMARK_LITE)
#include "soh/Enhancements/benchmark.h"
#endif

void TitleSetup_InitImpl(GameState* gameState) {
    osSyncPrintf("ゼルダ共通データ初期化\n"); // "Zelda common data initalization"
    SaveContext_Init();
    gameState->running = false;
#if defined(SOH_BENCHMARK) || defined(SOH_BENCHMARK_LITE)
    // Skip the logo, title screen and file select entirely and drop straight into the fixture
    // scene on a debug save, the same way the debug warp screen reaches Play_Init.
    if (Benchmark_IsActive()) {
        Benchmark_SetupDebugSave();
        SET_NEXT_GAMESTATE(gameState, Play_Init, PlayState);
        return;
    }
#endif
    SET_NEXT_GAMESTATE(gameState, Title_Init, TitleContext);
}

void TitleSetup_Destroy(GameState* gameState) {
}

void TitleSetup_Init(GameState* gameState) {
    gameState->destroy = TitleSetup_Destroy;
    TitleSetup_InitImpl(gameState);
}
