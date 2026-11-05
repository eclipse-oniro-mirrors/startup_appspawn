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

#include <gtest/gtest.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <string>

#include "app_spawn_appuid_test_helper.h"

using namespace testing::ext;
using OHOS::AppUidTest::BuildAppUidPropertyWithCheckpoint;
using OHOS::AppUidTest::CleanupAppUidProperty;

namespace {

constexpr uint32_t CUSTOM_SANDBOX_UID = 100;           // userId-like 10x uid
constexpr uint32_t CUSTOM_SANDBOX_APPUID = 20010052;  // 200xxxxx -> userId 100

/* mirrors modules/ace_adapter/appspawn_checkpoint.c private define */
#define APPUID_TEST_CHECKPOINT_RESTORE_ALL _IOR(0xE0, 0x1, int)

/* appspawn_checkpoint.c (APPSPAWN_STATIC is empty under APPSPAWN_TEST) */
extern "C" {
int32_t DoCheckpointProcess(AppSpawnMgr *content, AppSpawningCtx *property, int fd,
                            unsigned long ioctlCmd, bool needForkDenied);
AppSpawnedCheckPointProcesses *GetSpawningImgInfoByName(AppSpawnMgr *content, const char *name);
}

class AppSpawnAppUidCheckpointTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override { AppUidMockReset(); }
    void TearDown() override { AppUidMockReset(); }
};

/*
 * @tc.name: AppUid_CheckPoint_ForkDeniedPath_001
 * @tc.desc: fork-denied path userId comes from appUid: the opened path is
 *           /dev/pids/100/<bundle>/app_<pid>/fork_denied, never userId 0.
 */
HWTEST_F(AppSpawnAppUidCheckpointTest, AppUid_CheckPoint_ForkDeniedPath_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidPropertyWithCheckpoint("com.test.forkdenied",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true, 7005, 1);
    ASSERT_NE(property, nullptr);

    AppUidMockReset();
    // fake fd; the ioctl stub returns 0 so DoCheckpointProcess proceeds
    int32_t ret = DoCheckpointProcess(mgr, property, 42,
        static_cast<unsigned long>(APPUID_TEST_CHECKPOINT_RESTORE_ALL), true);
    EXPECT_EQ(ret, 0);

    // the fork-denied path must use the appUid-derived userId 100
    EXPECT_EQ(AppUidMockFindOpenPath("/dev/pids/100/com.test.forkdenied/"), 1);
    EXPECT_EQ(AppUidMockFindOpenPath("/dev/pids/0/"), 0);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_CheckPoint_RecordAppUid_001
 * @tc.desc: AppSpawnedCheckPointProcesses.appUid stores dacInfo->appUid
 *           when the checkpoint entry is created for a worker process.
 */
HWTEST_F(AppSpawnAppUidCheckpointTest, AppUid_CheckPoint_RecordAppUid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidPropertyWithCheckpoint("com.test.recordappuid",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true, 7006, 2);
    ASSERT_NE(property, nullptr);

    int32_t ret = DoCheckpointProcess(mgr, property, 42,
        static_cast<unsigned long>(APPUID_TEST_CHECKPOINT_RESTORE_ALL), false);
    EXPECT_EQ(ret, 0);

    AppSpawnedCheckPointProcesses *record = GetSpawningImgInfoByName(mgr, "com.test.recordappuid");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->appUid, CUSTOM_SANDBOX_APPUID);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

}  // namespace
