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

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_msg.h"
#include "appspawn_servicebin_adapter.h"
#include "appspawn_utils.h"
#include "securec.h"
#include "spawn_policy.h"

#define DAEMON_BIN_TRUSTED_UID_INSTALLS 3060

static bool IsDaemonBinTrustedOwner(uid_t uid)
{
    return uid == 0 || uid == DAEMON_BIN_TRUSTED_UID_INSTALLS;
}

APPSPAWN_STATIC void ClearServiceBinEnvAndReturnSuccess(AppSpawnContent *content, AppSpawnClient *client)
{
    AppSpawningCtx *property = (AppSpawningCtx *)client;
    APPSPAWN_CHECK(content != nullptr && property != nullptr, return, "invalid param in clearEnv");
    int fd = property->forkCtx.fd[1];
    property->forkCtx.fd[1] = -1;
    AppSpawnEnvClear(content, client);
    APPSPAWN_CHECK(fd >= 0, return, "invalid fd for notify parent");
    int ret = 0;
    ssize_t len = write(fd, &ret, sizeof(ret));
    (void)close(fd);
    APPSPAWN_CHECK_ONLY_DUMPW(len == sizeof(ret), "error %{public}d len %{public}zd", errno, len);
}

extern "C" {
static int GetDaemonBinPath(const AppSpawningCtx *property, char *pathBuf, uint32_t bufSize)
{
    uint32_t len = 0;
    const char *binPath = reinterpret_cast<const char *>(GetAppSpawnMsgExtInfo(
        property->message, MSG_EXT_NAME_DAEMON_BIN_PATH, &len));
    if (binPath == nullptr || len == 0 || len >= bufSize ||
        (memcpy_s(pathBuf, bufSize, binPath, len) != EOK)) {
        APPSPAWN_LOGE("daemon_bin_path invalid: len=%{public}u", len);
        return APPSPAWN_MSG_INVALID;
    }
    pathBuf[len] = '\0';
    APPSPAWN_LOGI("daemon_bin_path: %{public}s", pathBuf);
    return 0;
}

APPSPAWN_STATIC int PrepareDaemonExec(AppSpawnMgr *content, AppSpawningCtx *property,
    char *pathBuf, uint32_t bufSize)
{
    UNUSED(content);
    int ret = GetDaemonBinPath(property, pathBuf, bufSize);
    APPSPAWN_CHECK(ret == 0, return ret, "PrepareDaemonExec: get bin path failed: 0x%{public}x", ret);

    if (strstr(pathBuf, "..") != nullptr) {
        APPSPAWN_LOGE("PrepareDaemonExec: daemon_bin_path contains '..'");
        return APPSPAWN_MSG_INVALID;
    }

    struct stat pathStat = {};
    APPSPAWN_CHECK(lstat(pathBuf, &pathStat) == 0, return APPSPAWN_MSG_INVALID,
        "PrepareDaemonExec: lstat %{public}s failed, errno=%{public}d", pathBuf, errno);
    APPSPAWN_CHECK(S_ISREG(pathStat.st_mode), return APPSPAWN_MSG_INVALID,
        "PrepareDaemonExec: %{public}s is not a regular file, mode=0x%{public}x", pathBuf, pathStat.st_mode);
    APPSPAWN_CHECK(IsDaemonBinTrustedOwner(pathStat.st_uid), return APPSPAWN_MSG_INVALID,
        "PrepareDaemonExec: untrusted owner uid=%{public}u, path=%{public}s", pathStat.st_uid, pathBuf);
    return 0;
}

int AppSpawnServiceBinRunTarget(AppSpawnMgr *content, AppSpawningCtx *property)
{
    APPSPAWN_CHECK(IsNoShareFsEnable(), return APPSPAWN_NATIVE_NOT_SUPPORT,
        "AppSpawnServiceBinRunTarget: Current platform not support daemon");
    APPSPAWN_CHECK(content != nullptr && property != nullptr, return APPSPAWN_ARG_INVALID,
        "AppSpawnServiceBinRunTarget: invalid arg");
    const SpawnPolicy *pl = ResolvePolicy(property);
    APPSPAWN_CHECK(pl->execFrom == POLICY_SRC_EXT_BIN_PATH, return APPSPAWN_MSG_INVALID,
        "AppSpawnServiceBinRunTarget: not a daemon bin request");

    char pathBuf[PATH_MAX + 1];
    int ret = GetDaemonBinPath(property, pathBuf, sizeof(pathBuf));
    APPSPAWN_CHECK(ret == 0, return ret, "AppSpawnServiceBinRunTarget: get bin path failed: 0x%{public}x", ret);
    char *const argv[] = { pathBuf, nullptr };
    APPSPAWN_LOGI("Start to launch daemon bin with execv");
    ClearServiceBinEnvAndReturnSuccess((AppSpawnContent *)&content->content, (AppSpawnClient *)&property->client);
    int execRet = execv(pathBuf, argv);
    // If it succeeds calling execv, it never returns.
    int err = errno;
    APPSPAWN_LOGE("Failed to launch daemon bin with execv: %{public}d errno %{public}s", execRet, strerror(err));
    return 0;
}

APPSPAWN_STATIC int CheckDaemonExecTarget(AppSpawnMgr *content, AppSpawningCtx *property)
{
    UNUSED(content);
    const SpawnPolicy *pl = ResolvePolicy(property);
    if (pl->execFrom != POLICY_SRC_EXT_BIN_PATH) {
        return 0;  // app: runChildProcessor handles hap entry
    }
    APPSPAWN_CHECK(IsNoShareFsEnable(), return APPSPAWN_NATIVE_NOT_SUPPORT,
        "CheckDaemonExecTarget: Current platform not support daemon");
    char pathBuf[PATH_MAX + 1];
    int ret = PrepareDaemonExec(content, property, pathBuf, sizeof(pathBuf));
    APPSPAWN_CHECK(ret == 0, return ret,
        "CheckDaemonExecTarget: reject daemon bin path, ret: 0x%{public}x", ret);

    if (access(pathBuf, X_OK) != 0) {
        APPSPAWN_LOGE("CheckDaemonExecTarget: exec denied for appspawn domain, errno=%{public}d", errno);
        return APPSPAWN_PERMISSION_NOT_SUPPORT;
    }
    return 0;
}

}  // extern "C"

MODULE_CONSTRUCTOR(void)
{
    APPSPAWN_LOGV("Load service bin adapter ...");
    AddAppSpawnHook(STAGE_CHILD_PRE_RELY, HOOK_PRIO_LOWEST, CheckDaemonExecTarget);
}
