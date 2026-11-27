/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef RACEGUARD_TW
#include <unistd.h>

#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_msg.h"
#include "appspawn_utils.h"
#include "parameter.h"
#include "securec.h"

#define RACEGUARD_LD_PRELOAD "/system/asan/lib64/chipset-sdk-sp/libraceguard_rt.z.so"
#define RACEGUARD_PRELOAD_BUF_LEN 512
#define HOOK_PRIO_RACEGUARD 2600
#define RACEGUARD_COLD_RUN_PATH "/system/asan/bin/appspawn"

#define RACEGUARD_WRAP_VALUE_MAX_LENGTH 96
#define RACEGUARD_WRAPPER_VALUE "raceguard_wrapper"
#define RACEGUARD_ERR_FAILED (-1)

static int CheckSupportRaceGuardColdStart(const char *bundleName)
{
    if (bundleName == NULL) {
        return RACEGUARD_ERR_FAILED;
    }
    char wrapKey[RACEGUARD_WRAP_VALUE_MAX_LENGTH] = {0};
    char wrapValue[RACEGUARD_WRAP_VALUE_MAX_LENGTH] = {0};

    // Differs from asan_detector.c which passes the macro constant WRAP_VALUE_MAX_LENGTH:
    // sizeof(wrapKey) is type-safe (size_t) and refactoring-safe, matching this file's style.
    int len = sprintf_s(wrapKey, sizeof(wrapKey), "wrap.%s", bundleName);
    if (len <= 0 || (size_t)len >= sizeof(wrapKey)) {
        APPSPAWN_LOGW("CheckSupportRaceGuardColdStart format wrapKey fail, len=%{public}d", len);
        return RACEGUARD_ERR_FAILED;
    }

    // Differs from asan_detector.c which passes the macro constant WRAP_VALUE_MAX_LENGTH:
    // sizeof(wrapValue) is type-safe (size_t) and refactoring-safe, matching this file's style.
    int ret = GetParameter(wrapKey, "", wrapValue, sizeof(wrapValue));
    // Expected value is "raceguard_wrapper" (vs "asan_wrapper" in asan_detector.c): each wrap value
    // tags a distinct cold-start capability, so an app opts into raceguard or asan independently.
    APPSPAWN_CHECK_LOGV(ret > 0 && (!strcmp(wrapValue, RACEGUARD_WRAPPER_VALUE)), return RACEGUARD_ERR_FAILED,
        "Not raceguard wrap %{public}s.", bundleName);
    APPSPAWN_LOGI("RaceGuard: GetParameter %{public}s value is %{public}s.", wrapKey, wrapValue);
    return 0;
}

APPSPAWN_STATIC int BuildRaceGuardPreloadValue(char *buf, size_t bufLen)
{
    if (buf == NULL || bufLen == 0) {
        return RACEGUARD_ERR_FAILED;
    }
    const char *cur = getenv("LD_PRELOAD");
    if (cur == NULL || cur[0] == '\0') {
        int len = sprintf_s(buf, bufLen, "%s", RACEGUARD_LD_PRELOAD);
        APPSPAWN_CHECK(len > 0 && (size_t)len < bufLen, return RACEGUARD_ERR_FAILED, "PreloadValue sprintf fail");
        return 0;
    }
    if (strstr(cur, RACEGUARD_LD_PRELOAD) != NULL) {
        return RACEGUARD_ERR_FAILED;
    }
    int len = sprintf_s(buf, bufLen, "%s:%s", cur, RACEGUARD_LD_PRELOAD);
    if (len <= 0 || (size_t)len >= bufLen) {
        APPSPAWN_LOGW("BuildRaceGuardPreloadValue append truncated, skip inject");
        return RACEGUARD_ERR_FAILED;
    }
    return 0;
}

/*
 * SetRaceGuardSpawningFlag - Decide the raceguard cold start in the parent process
 * (invoked at STAGE_PARENT_PRE_FORK, after asan's HOOK_PRIO_COMMON hook of the same
 * stage; sets coldRunPath/APP_COLD_START/APP_RACEGUARD_DETECTOR so the app
 * cold-starts via /system/asan/bin/appspawn with the raceguard runtime loaded).
 * The decision must live here: IsChildColdRun() controls the watcher timeout tier
 * and the prefork bypass before fork, so a child-only APP_COLD_START would be
 * invisible (asan model, cf. AsanSpawnGetSpawningFlag). Always returns 0
 * (HOOK_STOP_WHEN_ERROR): degrade, never abort the spawn.
 */
APPSPAWN_STATIC int SetRaceGuardSpawningFlag(AppSpawnMgr *content, AppSpawningCtx *property)
{
    APPSPAWN_CHECK(content != NULL && property != NULL, return 0, "SetRaceGuardSpawningFlag invalid arg");
    // Only appspawn standard mode is supported: nweb/cj/native modes do not use the app cold-start path
    if (!IsAppSpawnMode(content)) {
        return 0;
    }
    // Only app-spawn messages are supported: native process messages do not use the app cold-start path
    if (GetAppSpawnMsgType(property) == MSG_SPAWN_NATIVE_PROCESS) {
        return 0;
    }
    // Exclude daemon spawn (MSG_SPAWN_SERVICE_BIN + APP_FLAGS_DAEMON_BIN): a daemon runs
    // as uid 0 with full capabilities and must not be injected with the raceguard runtime
    // nor cold-started via /system/asan/bin/appspawn (aligned with the dfx preload exclusion).
    if (IsDaemonBinMsg(property->message)) {
        return 0;
    }
    // Only apps explicitly configured with raceguard are supported:
    // the wrap.<bundleName> system parameter must be raceguard_wrapper
    if (CheckSupportRaceGuardColdStart(GetBundleName(property)) != 0) {
        return 0;
    }
    // Yield to an existing cold start: do not override when APP_COLD_START is already set
    // (by asan etc. at this same parent stage, HOOK_PRIO_COMMON runs earlier), to avoid
    // duplicate cold start or entry conflict
    if (property->client.flags & APP_COLD_START) {
        return 0;
    }
    // Degrade when the raceguard so is missing: keep the normal (non-cold) spawn.
    // F_OK only on purpose: a preload .so is mmap'd from a read fd and does not need an
    // execute bit, so an X_OK check here would wrongly reject valid mode-0644 libs.
    if (access(RACEGUARD_LD_PRELOAD, F_OK) != 0) {
        APPSPAWN_LOGW("RaceGuard: so missing, degrade %{public}s", RACEGUARD_LD_PRELOAD);
        return 0;
    }
    // Degrade when the cold-start entry is missing or not executable: execv would fail in
    // the child (only logged there, never notified to the parent), leaving the request
    // stuck until the watcher timeout. X_OK also catches a present-but-non-executable
    // entry (for root it succeeds when any exec bit is set, still stricter than F_OK).
    if (access(RACEGUARD_COLD_RUN_PATH, F_OK) != 0 || access(RACEGUARD_COLD_RUN_PATH, X_OK) != 0) {
        APPSPAWN_LOGW("RaceGuard: cold run entry not executable, degrade %{public}s", RACEGUARD_COLD_RUN_PATH);
        return 0;
    }
    // Defensive free only: today no reachable writer leaves a non-NULL coldRunPath here —
    // the sole other producer (asan_detector.c) always sets APP_COLD_START together with
    // the path, and this hook already yielded to that flag above. Still free before
    // overwriting so a future writer that sets coldRunPath WITHOUT the flag cannot turn
    // into a memory leak (defensive pattern shared with asan_detector.c).
    if (property->forkCtx.coldRunPath != NULL) {
        free(property->forkCtx.coldRunPath);
        property->forkCtx.coldRunPath = NULL;
    }
    property->forkCtx.coldRunPath = strdup(RACEGUARD_COLD_RUN_PATH);
    if (property->forkCtx.coldRunPath == NULL) {
        APPSPAWN_LOGE("RaceGuard: strdup coldRunPath fail, degrade for %{public}s", GetProcessName(property));
        return 0;
    }
    // Parent-visible cold start: selects the 10s watcher tier and bypasses prefork.
    property->client.flags |= APP_COLD_START;
    // Child-stage marker: SetRaceGuardPreloadEnv injects LD_PRELOAD for this request only.
    property->client.flags |= APP_RACEGUARD_DETECTOR;
    APPSPAWN_LOGI("RaceGuard: cold start decided for %{public}s", GetProcessName(property));
    return 0;
}

/*
 * SetRaceGuardPreloadEnv - Inject the raceguard runtime into LD_PRELOAD in the child
 * (invoked at STAGE_CHILD_PRE_COLDBOOT; appends libraceguard_rt.z.so to LD_PRELOAD
 * before AppSpawnChild checks APP_COLD_START and execv's the cold-start entry).
 *
 * Only the env change lives in the child: setenv cannot be done per-request in the
 * parent (env is process-wide and would pollute other requests), while the cold-start
 * decision itself was already made by SetRaceGuardSpawningFlag in the parent stage.
 */
APPSPAWN_STATIC int SetRaceGuardPreloadEnv(AppSpawnMgr *content, AppSpawningCtx *property)
{
    APPSPAWN_CHECK(content != NULL && property != NULL, return 0, "SetRaceGuardPreloadEnv invalid arg");
    // Defense in depth: the marker is only set under the standard app mode by the parent hook
    if (!IsAppSpawnMode(content)) {
        return 0;
    }
    // Inject only requests flagged by the parent-stage decision
    // (client.flags bit, cf. APP_ASAN_DETECTOR usage in asan_detector.c)
    if (!(property->client.flags & APP_RACEGUARD_DETECTOR)) {
        return 0;
    }
    // Re-check the so presence in the child; if it disappears between fork stages, keep
    // LD_PRELOAD untouched (the cold start still proceeds, the app spawns without the
    // raceguard runtime)
    if (access(RACEGUARD_LD_PRELOAD, F_OK) != 0) {
        APPSPAWN_LOGW("RaceGuard: so missing in child, skip inject %{public}s", RACEGUARD_LD_PRELOAD);
        return 0;
    }
    // Degrade when the cold-start entry vanished between the parent decision and here:
    // execv would fail in this child (failure is only logged, never notified to the
    // parent), so the request would hang until the watcher timeout. Drop the cold-start
    // decision (clear APP_COLD_START + free coldRunPath) to fall back to the normal
    // spawn path, which still reports the result to the parent.
    if (access(RACEGUARD_COLD_RUN_PATH, F_OK) != 0 || access(RACEGUARD_COLD_RUN_PATH, X_OK) != 0) {
        APPSPAWN_LOGW("RaceGuard: cold run entry missing in child, degrade to normal spawn");
        property->client.flags &= ~(APP_COLD_START | APP_RACEGUARD_DETECTOR);
        free(property->forkCtx.coldRunPath);
        property->forkCtx.coldRunPath = NULL;
        return 0;
    }
    // Build the LD_PRELOAD value (idempotent/append/truncate-guarded); skip on failure
    char preloadBuf[RACEGUARD_PRELOAD_BUF_LEN] = {0};
    if (BuildRaceGuardPreloadValue(preloadBuf, sizeof(preloadBuf)) != 0) {
        return 0;
    }
    if (setenv("LD_PRELOAD", preloadBuf, 1) != 0) {
        APPSPAWN_LOGW("RaceGuard: setenv LD_PRELOAD fail, degrade for %{public}s", GetProcessName(property));
        return 0;
    }
    APPSPAWN_LOGI("RaceGuard: preload enabled for %{public}s", GetProcessName(property));
    return 0;
}

MODULE_CONSTRUCTOR(void)
{
    APPSPAWN_LOGV("Load raceguard module ...");
    // Priority 2600 > asan HOOK_PRIO_COMMON(2000): decide AFTER asan so an asan cold
    // start (APP_COLD_START already set) wins over raceguard
    AddAppSpawnHook(STAGE_PARENT_PRE_FORK, HOOK_PRIO_RACEGUARD, SetRaceGuardSpawningFlag);
    AddAppSpawnHook(STAGE_CHILD_PRE_COLDBOOT, HOOK_PRIO_RACEGUARD, SetRaceGuardPreloadEnv);
}
#endif  // RACEGUARD_TW
