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

#include "app_spawn_appenv_test_helper.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "app_spawn_test_helper.h"

namespace OHOS {

namespace {
/* Test-scope AppSpawnTestHelper fixture for message building. */
AppSpawnTestHelper &GetTestHelper()
{
    static AppSpawnTestHelper helper;
    return helper;
}

/* Recursively mkdir each path component. EEXIST is tolerated. Returns 0 on
 * success (including when the final path already exists). */
int MkdirAll(const std::string &path, mode_t mode)
{
    if (path.empty()) {
        return -1;
    }
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) {
            if (mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) {
                return -1;
            }
        }
    }
    if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}
}  // namespace

AppSpawningCtx *AppenvTestHelper::BuildProperty(const char *processName, const char *userId, uint32_t appUid,
    bool customSandbox, bool withUserId, bool withDac)
{
    (void)withDac;  // DacInfo is always added by CreateMsg via SetTestUid; kept for API symmetry.
    AppSpawnClientHandle clientHandle = nullptr;
    int ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
    if (ret != 0) {
        return nullptr;
    }
    AppSpawnReqMsgHandle reqHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        /* SetTestUid drives AddDacInfo (called inside CreateMsg) so that
         * dacInfo.uid == dacInfo.appUid == appUid. InitAppCommonEnv derives
         * userId = appInfo->appUid / UID_BASE from this. */
        GetTestHelper().SetProcessName(processName);
        GetTestHelper().SetTestUid(appUid);
        GetTestHelper().SetTestGid(appUid);
        reqHandle = GetTestHelper().CreateMsg(clientHandle, MSG_APP_SPAWN, 0);
        if (reqHandle == nullptr) {
            break;
        }
        if (withUserId && userId != nullptr) {
            ret = AppSpawnReqMsgAddStringInfo(reqHandle, MSG_EXT_NAME_USERID, userId);
            if (ret != 0) {
                break;
            }
        }
        if (customSandbox) {
            (void)AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_CUSTOM_SANDBOX);
        }
        property = GetTestHelper().GetAppProperty(clientHandle, reqHandle);
        reqHandle = nullptr;  // ownership consumed by GetAppProperty
    } while (0);
    if (reqHandle != nullptr) {
        AppSpawnReqMsgFree(reqHandle);
    }
    AppSpawnClientDestroy(clientHandle);
    return property;
}

int AppenvTestHelper::WriteAppenvConf(const char *userId, const std::string &content)
{
    if (userId == nullptr || userId[0] == '\0') {
        return -1;
    }
    std::string dir = std::string(APPENV_DIR_PREFIX) + userId + APPENV_DIR_SUFFIX;
    if (MkdirAll(dir, 0755) != 0) {  // 0755 dir mode
        return -1;
    }
    std::string path = std::string(APPENV_DIR_PREFIX) + userId + APPENV_CONF_SUFFIX;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);  // 0644 file mode
    if (fd < 0) {
        return -1;
    }
    ssize_t written = write(fd, content.data(), content.size());
    close(fd);
    return (written == static_cast<ssize_t>(content.size())) ? 0 : -1;
}

void AppenvTestHelper::CleanupAppenv(const char *userId)
{
    if (userId == nullptr || userId[0] == '\0') {
        return;
    }
    std::string confPath = std::string(APPENV_DIR_PREFIX) + userId + APPENV_CONF_SUFFIX;
    std::string appenvDir = std::string(APPENV_DIR_PREFIX) + userId + APPENV_DIR_SUFFIX;
    std::string userDir = std::string(APPENV_DIR_PREFIX) + userId;
    (void)unlink(confPath.c_str());
    (void)rmdir(appenvDir.c_str());
    (void)rmdir(userDir.c_str());  // only succeeds if empty
}

bool AppenvTestHelper::DirExists(const char *path)
{
    struct stat st = {};
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

bool AppenvTestHelper::FileExists(const char *path)
{
    struct stat st = {};
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

bool AppenvTestHelper::GetStat(const char *path, struct stat &st)
{
    return stat(path, &st) == 0;
}

}  // namespace OHOS
