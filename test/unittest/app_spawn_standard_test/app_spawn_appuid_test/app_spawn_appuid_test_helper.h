/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef APP_SPAWN_APPUID_TEST_HELPER_H
#define APP_SPAWN_APPUID_TEST_HELPER_H

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "appspawn_appuid_mock.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_modulemgr.h"
#include "appspawn_msg.h"
#include "appspawn_service.h"
#include "appspawn_utils.h"
#include "securec.h"
#include "sandbox_shared_mount.h"

// Production state exposed by APPSPAWN_TEST for private AppUid fixture cleanup.
extern std::map<std::string, LockBundleInfo> g_lockBundleMap;

namespace OHOS {
namespace AppUidTest {

/* Hand-crafted TLV message builder, independent of the client/server helper. */
constexpr size_t APPUID_TEST_MSG_BUFFER_SIZE = 2048;   // test buffer size
constexpr uint32_t APPUID_TEST_DAC_GID = 20010029;     // gid paired with the uid fixture
constexpr uint64_t APPUID_TEST_ACCESS_TOKEN = 12345678;  // accessTokenIdEx fixture

AppSpawningCtx *BuildAppUidProperty(const char *processName, uint32_t uid, uint32_t appUid,
    bool customSandbox, bool withUserId = false);
AppSpawningCtx *BuildAppUidPropertyWithCheckpoint(const char *processName, uint32_t uid, uint32_t appUid,
    bool customSandbox, pid_t imgPid, uint64_t checkPointId);
void CleanupAppUidProperty(AppSpawningCtx *property);

}  // namespace AppUidTest
}  // namespace OHOS

#endif  // APP_SPAWN_APPUID_TEST_HELPER_H
