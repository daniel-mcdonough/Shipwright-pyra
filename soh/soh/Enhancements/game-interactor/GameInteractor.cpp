/*
GameInteractor is meant to be used for interacting with the game (yup...).
It exposes functions that directly modify, add or remove game related elements.

GameInteractionEffects.cpp is used when code that needs these
functions also need a check wether a command can be run or not.

If these checks need to happen wherever GameInteractor functions are needed, the
GameInteractor functions can be called directly.
*/

#include "GameInteractor.h"
#ifdef SOH_BENCHMARK
#include <algorithm>
#include <map>
#include <time.h>
#include <tuple>
#include <vector>

int gBenchHookTime = 0;

namespace {
struct BenchHookStat {
    uint64_t ns = 0;
    uint64_t calls = 0;
};
// Keyed by (type, file, line): the names are string literals, so their pointers are stable.
std::map<std::tuple<const char*, const char*, uint32_t>, BenchHookStat> sBenchHooks;
} // namespace

uint64_t BenchHookClockNs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void BenchHookAccount(const char* type, const HookRegisteringInfo& where, uint64_t ns) {
    auto& s = sBenchHooks[{ type, where.file, (uint32_t)where.line }];
    s.ns += ns;
    s.calls++;
}

std::string BenchHookReport() {
    std::vector<std::pair<std::tuple<const char*, const char*, uint32_t>, BenchHookStat>> rows(sBenchHooks.begin(),
                                                                                             sBenchHooks.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::string out;
    char line[512];
    for (size_t i = 0; i < rows.size() && i < 40; i++) {
        const auto& [key, s] = rows[i];
        snprintf(line, sizeof(line), "BENCH hook type=%s site=%s:%u calls=%llu total_ms=%.1f ns_per_call=%.0f\n",
                 std::get<0>(key), std::get<1>(key), std::get<2>(key), (unsigned long long)s.calls, s.ns / 1e6,
                 s.calls ? (double)s.ns / s.calls : 0.0);
        out += line;
    }
    return out;
}
#endif
#include <libultraship/bridge.h>

extern "C" {
#include "variables.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// MARK: - Effects

GameInteractionEffectQueryResult GameInteractor::CanApplyEffect(GameInteractionEffectBase& effect) {
    return effect.CanBeApplied();
}

GameInteractionEffectQueryResult GameInteractor::ApplyEffect(GameInteractionEffectBase& effect) {
    return effect.Apply();
}

GameInteractionEffectQueryResult GameInteractor::RemoveEffect(RemovableGameInteractionEffect& effect) {
    return effect.Remove();
}

// MARK: - Helpers

bool GameInteractor::IsSaveLoaded(bool allowDbgSave) {
    Player* player;
    if (gPlayState != NULL) {
        player = GET_PLAYER(gPlayState);
    }

    // Checking for normal game mode prevents debug saves from reporting true on title screen
    if (gPlayState == NULL || player == NULL || gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return false;
    }

    // Valid save file or debug save
    return (gSaveContext.fileNum >= 0 && gSaveContext.fileNum <= 2) || (allowDbgSave && gSaveContext.fileNum == 0xFF);
}

bool GameInteractor::IsGameplayPaused() {
    Player* player = GET_PLAYER(gPlayState);
    return (Player_InBlockingCsMode(gPlayState, player) || gPlayState->pauseCtx.state != 0 ||
            gPlayState->msgCtx.msgMode != 0)
               ? true
               : false;
}

bool GameInteractor::CanSpawnActor() {
    return GameInteractor::IsSaveLoaded() && !GameInteractor::IsGameplayPaused();
}

bool GameInteractor::CanAddOrTakeAmmo(int16_t amount, int16_t item) {
    int16_t upgradeToCheck = 0;

    switch (item) {
        case ITEM_STICK:
            upgradeToCheck = UPG_STICKS;
            break;
        case ITEM_NUT:
            upgradeToCheck = UPG_NUTS;
            break;
        case ITEM_BOW:
            upgradeToCheck = UPG_QUIVER;
            break;
        case ITEM_SLINGSHOT:
            upgradeToCheck = UPG_BULLET_BAG;
            break;
        case ITEM_BOMB:
            upgradeToCheck = UPG_BOMB_BAG;
            break;
        default:
            break;
    }

    if (amount < 0 && AMMO(item) == 0) {
        return false;
    }

    if (item != ITEM_BOMBCHU && item != ITEM_BEAN) {
        if ((CUR_CAPACITY(upgradeToCheck) == 0) || (amount > 0 && AMMO(item) == CUR_CAPACITY(upgradeToCheck))) {
            return false;
        }
        return true;
    } else {
        // Separate checks for beans and bombchus because they don't have capacity upgrades
        if (INV_CONTENT(item) != item ||
            (amount > 0 && ((item == ITEM_BOMBCHU && AMMO(item) == 50) || (item == ITEM_BEAN && AMMO(item) == 10)))) {
            return false;
        }
        return true;
    }
}
