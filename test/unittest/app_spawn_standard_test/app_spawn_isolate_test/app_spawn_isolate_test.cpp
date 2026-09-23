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

#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <cstdbool>
#include <cstdlib>

#include "appspawn_utils.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "json_utils.h"
#include "lib_wrapper.h"
#include "cJSON.h"
#include "app_spawn_isolate_test.h"

using namespace testing;
using namespace testing::ext;
using namespace OHOS;

namespace OHOS {
static int g_ioctlCallCount = 0;

static AppSpawnMgr *CreateMockAppSpawnMgr(void)
{
    AppSpawnMgr *mgr = (AppSpawnMgr *)calloc(1, sizeof(AppSpawnMgr));
    if (mgr == nullptr) {
        return nullptr;
    }
    OH_ListInit(&mgr->appQueue);
    OH_ListInit(&mgr->diedQueue);
    OH_ListInit(&mgr->appSpawnQueue);
    OH_ListInit(&mgr->extData);
    OH_ListInit(&mgr->dataGroupCtxQueue);
    OH_ListInit(&mgr->checkPointIdQueue);
    OH_ListInit(&mgr->spawningFdsQueue);
    return mgr;
}

static void FreeSandboxPathMarkConfigFields(SandboxPathMarkConfig *config)
{
    if (config == nullptr) {
        return;
    }
    if (config->mounts != nullptr) {
        for (uint32_t i = 0; i < config->mountCount; i++) {
            free(config->mounts[i].path);
        }
        free(config->mounts);
        config->mounts = nullptr;
        config->mountCount = 0;
    }
    if (config->paths != nullptr) {
        for (uint32_t i = 0; i < config->pathCount; i++) {
            free(config->paths[i].path);
        }
        free(config->paths);
        config->paths = nullptr;
        config->pathCount = 0;
    }
}

class AppSpawnIsolateTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
        g_ioctlCallCount = 0;
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " start";
        APPSPAWN_LOGI("%{public}s.%{public}s start", info->test_suite_name(), info->name());
    }
    void TearDown()
    {
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " end";
        APPSPAWN_LOGI("%{public}s.%{public}s end", info->test_suite_name(), info->name());
    }
};

/*
 * ParsePathConfig with both "mounts" (2 entries, flags isolate+ugc) and
 * "paths" (1 entry, flags ugc). Verifies config is populated with 2 marks
 * and 1 dec path.
 */
HWTEST_F(AppSpawnIsolateTest, ParsePathConfig_BothSections, TestSize.Level0)
{
    const char *jsonStr =
        "{"
        "\"mounts\":["
        "{\"path\":\"/data/isolate1\",\"flags\":[\"isolate\",\"ugc\"]},"
        "{\"path\":\"/data/isolate2\",\"flags\":[\"isolate\",\"ugc\"]}"
        "],"
        "\"paths\":["
        "{\"path\":\"/data/dec1\",\"flags\":[\"ugc\"]}"
        "]"
        "}";
    cJSON *root = cJSON_Parse(jsonStr);
    ASSERT_NE(root, nullptr);

    SandboxPathMarkConfig config = {};
    ParseJsonContext ctx;
    ctx.config = &config;

    int ret = ParsePathConfig(root, &ctx);
    EXPECT_EQ(ret, APPSPAWN_OK);
    EXPECT_EQ(config.mountCount, 2);
    EXPECT_EQ(config.pathCount, 1);

    FreeSandboxPathMarkConfigFields(&config);
    cJSON_Delete(root);
}

/*
 * ParsePathConfig with a mounts entry whose flags array contains only an
 * unknown flag. The entry must be skipped, leaving mountCount at 0.
 */
HWTEST_F(AppSpawnIsolateTest, ParsePathConfig_UnknownFlag, TestSize.Level0)
{
    const char *jsonStr =
        "{"
        "\"mounts\":["
        "{\"path\":\"/data/unknown\",\"flags\":[\"unknown\"]}"
        "]"
        "}";
    cJSON *root = cJSON_Parse(jsonStr);
    ASSERT_NE(root, nullptr);

    SandboxPathMarkConfig config = {};
    ParseJsonContext ctx;
    ctx.config = &config;

    int ret = ParsePathConfig(root, &ctx);
    EXPECT_EQ(ret, APPSPAWN_OK);
    EXPECT_EQ(config.mountCount, 0);

    FreeSandboxPathMarkConfigFields(&config);
    cJSON_Delete(root);
}

/*
 * SetConstraintPaths batches paths in groups of KERNEL_BATCH_SIZE (8).
 * With 10 entries, ioctl should be called twice (8 + 2).
 */
HWTEST_F(AppSpawnIsolateTest, SetConstraintPaths_Batching, TestSize.Level0)
{
    IoctlFunc ioctlFunc = [](int fd, int req, va_list args) -> int {
        g_ioctlCallCount++;
        return 0;
    };
    OpenFunc openFunc = [](const char *pathname, int flags, mode_t mode) -> int {
        if (pathname != nullptr && strcmp(pathname, "/dev/dec") == 0) {
            return 3;  // fake valid fd
        }
        return __real_open(pathname, flags, mode);
    };
    UpdateIoctlFunc(ioctlFunc);
    UpdateOpenFunc(openFunc);

    PathInfo paths[10] = {};
    for (int i = 0; i < 10; i++) {
        paths[i].path = const_cast<char *>("/data/test/path");
        paths[i].pathLen = static_cast<uint32_t>(strlen(paths[i].path));
        paths[i].mode = 1;
        paths[i].flag = false;
    }

    SetConstraintPaths(paths, 10);
    EXPECT_EQ(g_ioctlCallCount, 2);  // 8 + 2 = 2 batches

    UpdateIoctlFunc(nullptr);
    UpdateOpenFunc(nullptr);
}

/*
 * SpawnLoadPathConfig when config file is not found. ParseJsonConfig fails
 * internally, but SpawnLoadPathConfig degrades gracefully and returns
 * APPSPAWN_OK. No ioctl should be issued.
 */
HWTEST_F(AppSpawnIsolateTest, SpawnLoadPathConfig_ConfigNotFound, TestSize.Level0)
{
    IoctlFunc ioctlFunc = [](int fd, int req, va_list args) -> int {
        g_ioctlCallCount++;
        return 0;
    };
    OpenFunc openFunc = [](const char *pathname, int flags, mode_t mode) -> int {
        if (pathname != nullptr && strcmp(pathname, "/dev/dec") == 0) {
            return -1;  // simulate device open failure
        }
        return __real_open(pathname, flags, mode);
    };
    UpdateIoctlFunc(ioctlFunc);
    UpdateOpenFunc(openFunc);

    AppSpawnMgr *mgr = CreateMockAppSpawnMgr();
    ASSERT_NE(mgr, nullptr);

    int ret = SpawnLoadPathConfig(mgr);
    EXPECT_EQ(ret, APPSPAWN_OK);
    EXPECT_EQ(g_ioctlCallCount, 0);

    DeleteAppSpawnMgr(mgr);
    UpdateIoctlFunc(nullptr);
    UpdateOpenFunc(nullptr);
}

/*
 * SpawnLoadPathConfig short-circuits on the second call via extData lookup.
 * After the first call an SandboxPathMarkConfig is stored in extData, so the
 * second call must not re-enter the config-loading path.
 */
HWTEST_F(AppSpawnIsolateTest, SpawnLoadPathConfig_LoadOnce, TestSize.Level0)
{
    OpenFunc openFunc = [](const char *pathname, int flags, mode_t mode) -> int {
        if (pathname != nullptr && strcmp(pathname, "/dev/dec") == 0) {
            return 3;  // fake valid fd
        }
        return __real_open(pathname, flags, mode);
    };
    IoctlFunc ioctlFunc = [](int fd, int req, va_list args) -> int {
        g_ioctlCallCount++;
        return 0;
    };
    UpdateOpenFunc(openFunc);
    UpdateIoctlFunc(ioctlFunc);

    AppSpawnMgr *mgr = CreateMockAppSpawnMgr();
    ASSERT_NE(mgr, nullptr);

    EXPECT_EQ(GetSandboxPathMarkConfig(mgr), nullptr);

    int ret1 = SpawnLoadPathConfig(mgr);
    EXPECT_EQ(ret1, APPSPAWN_OK);
    EXPECT_NE(GetSandboxPathMarkConfig(mgr), nullptr);

    int ioctlAfterFirst = g_ioctlCallCount;

    int ret2 = SpawnLoadPathConfig(mgr);
    EXPECT_EQ(ret2, APPSPAWN_OK);
    EXPECT_NE(GetSandboxPathMarkConfig(mgr), nullptr);
    EXPECT_EQ(g_ioctlCallCount, ioctlAfterFirst);  // no additional ioctl on second call

    DeleteAppSpawnMgr(mgr);
    UpdateOpenFunc(nullptr);
    UpdateIoctlFunc(nullptr);
}
}  // namespace OHOS
