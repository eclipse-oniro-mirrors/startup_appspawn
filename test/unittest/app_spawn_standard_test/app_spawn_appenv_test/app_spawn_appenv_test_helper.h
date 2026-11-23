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

#ifndef APP_SPAWN_APPENV_TEST_HELPER_H
#define APP_SPAWN_APPENV_TEST_HELPER_H

#include <cstdint>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

#include "appspawn.h"
#include "appspawn_client.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_server.h"
#include "appspawn_service.h"
#include "appspawn_utils.h"

namespace OHOS {

/*
 * AppenvTestHelper builds AppSpawningCtx with the MSG_EXT_NAME_USERID TLV and
 * manages /data/service/el1/<userId>/appenv/appenv.conf fixtures on device.
 *
 * Path layout (matches production constants in appspawn_adapter.cpp /
 * appspawn_common.c):
 *   APPENV_DIR_PREFIX  = "/data/service/el1/"
 *   APPENV_DIR_SUFFIX  = "/appenv"
 *   APPENV_CONF_SUFFIX = "/appenv/appenv.conf"
 */
class AppenvTestHelper {
public:
    static constexpr const char *APPENV_DIR_PREFIX = "/data/service/el1/";
    static constexpr const char *APPENV_DIR_SUFFIX = "/appenv";
    static constexpr const char *APPENV_CONF_SUFFIX = "/appenv/appenv.conf";

    AppenvTestHelper() = default;
    ~AppenvTestHelper() = default;

    /* Build an AppSpawningCtx with userId TLV + DacInfo (appUid) + optional
     * APP_FLAGS_CUSTOM_SANDBOX flag. withUserId=false omits the userId TLV
     * (for the "userId is null" branch). withDac=false omits TLV_DAC_INFO. */
    AppSpawningCtx *BuildProperty(const char *processName, const char *userId, uint32_t appUid,
        bool customSandbox, bool withUserId = true, bool withDac = true);

    /* Write content to /data/service/el1/<userId>/appenv/appenv.conf, creating
     * intermediate dirs. Returns 0 on success. */
    int WriteAppenvConf(const char *userId, const std::string &content);

    /* Remove appenv.conf and the appenv dir for the given userId. Also removes
     * the userId dir if it is empty after cleanup. Safe to call when absent. */
    void CleanupAppenv(const char *userId);

    /* Stat helpers. */
    static bool DirExists(const char *path);
    static bool FileExists(const char *path);
    static bool GetStat(const char *path, struct stat &st);
};

}  // namespace OHOS

#endif  // APP_SPAWN_APPENV_TEST_HELPER_H
