/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
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
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <map>

#include "appspawn_server.h"
#include "appspawn_service.h"
#include "json_utils.h"
#include "parameter.h"
#include "sandbox_def.h"
#include "sandbox_core.h"
#include "sandbox_common.h"
#include "securec.h"
#include "app_spawn_stub.h"
#include "app_spawn_test_helper.h"
#include "sandbox_dec.h"
#include "parameters.h"
#include "appspawn_hook.h"
#include "sandbox_lock_bundle_test.h"

using namespace testing;
using namespace testing::ext;
using namespace OHOS::AppSpawn;

APPSPAWN_STATIC int AddDataGroupItemToQueue(AppSpawnMgr *content, const std::string &srcPath,
    const std::string &destPath, const std::string &dataGroupUuid);
APPSPAWN_STATIC void DumpDataGroupCtxQueue(const ListNode *front);
APPSPAWN_STATIC int DataGroupCtxNodeCompare(ListNode *node, void *data);
APPSPAWN_STATIC int AppCleanupHook(const AppSpawnMgr *content, const AppSpawnedProcessInfo *appInfo);
APPSPAWN_STATIC int SpawnFailedHook(AppSpawnMgr *content, AppSpawningCtx *property);
int UpdateDataGroupDirs(AppSpawnMgr *content);

APPSPAWN_STATIC bool CheckPath(const std::string& name);

namespace OHOS {

AppSpawnTestHelper g_testHelperLock;

class AppSpawnLockBundleTest : public testing::Test {
public:
    static void SetUpTestCase();
    static void TearDownTestCase();
    void SetUp();
    void TearDown();

    static constexpr const char *testBundle1 = "com.example.test1";
    static constexpr const char *testBundle2 = "com.example.test2";
    static constexpr const char *testLockPath1 = "/tmp/test_locked_1";
    static constexpr const char *testLockPath2 = "/tmp/test_locked_2";
    static constexpr const char *testLockPathClone = "/tmp/test_locked_clone";
    static constexpr const char *testLockPathIsolated = "/tmp/test_locked_isolated";
    static constexpr mode_t dirMode = 0755;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
};

void AppSpawnLockBundleTest::SetUpTestCase()
{
    // Create test directories
    mkdir(testLockPath1, dirMode);
    mkdir(testLockPath2, dirMode);
    mkdir(testLockPathClone, dirMode);
    mkdir(testLockPathIsolated, dirMode);
}

void AppSpawnLockBundleTest::TearDownTestCase()
{
    // Clean up test directories
    rmdir(testLockPath1);
    rmdir(testLockPath2);
    rmdir(testLockPathClone);
    rmdir(testLockPathIsolated);
}

void AppSpawnLockBundleTest::SetUp()
{
    const TestInfo *info = UnitTest::GetInstance()->current_test_info();
    GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " start";
    // Clear the map before each test
    g_lockBundleMap.clear();
}

void AppSpawnLockBundleTest::TearDown()
{
    const TestInfo *info = UnitTest::GetInstance()->current_test_info();
    GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " end";
    // Clear the map after each test
    g_lockBundleMap.clear();
}

// ==================== AddLockBundleRef Tests ====================

/**
 * @tc.name: AddLockBundleRef_001
 * @tc.desc: Test AddLockBundleRef creates new entry for new lockPath
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, AddLockBundleRef_001, TestSize.Level1)
{
    int ret = AddLockBundleRef(1000, testBundle1, testLockPath1);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(g_lockBundleMap.size(), 1u);

    auto it = g_lockBundleMap.find(testLockPath1);
    EXPECT_NE(it, g_lockBundleMap.end());
    EXPECT_EQ(it->second.refCount, 1u);
    EXPECT_EQ(it->second.appUid, 1000u);
    EXPECT_EQ(it->second.bundleName, testBundle1);
    EXPECT_EQ(it->second.lockPath, testLockPath1);
}

/**
 * @tc.name: AddLockBundleRef_002
 * @tc.desc: Test AddLockBundleRef increments ref count for existing lockPath
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, AddLockBundleRef_002, TestSize.Level1)
{
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    int ret = AddLockBundleRef(1000, testBundle1, testLockPath1);

    EXPECT_EQ(ret, 0);
    EXPECT_EQ(g_lockBundleMap.size(), 1u);
    EXPECT_EQ(g_lockBundleMap[testLockPath1].refCount, 2u);
}

/**
 * @tc.name: AddLockBundleRef_003
 * @tc.desc: Test AddLockBundleRef creates separate entries for different lockPaths
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, AddLockBundleRef_003, TestSize.Level1)
{
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    AddLockBundleRef(1000, testBundle1, testLockPathClone);

    // Different lockPaths should have separate entries
    EXPECT_EQ(g_lockBundleMap.size(), 2u);
    EXPECT_EQ(g_lockBundleMap[testLockPath1].refCount, 1u);
    EXPECT_EQ(g_lockBundleMap[testLockPathClone].refCount, 1u);
}

/**
 * @tc.name: AddLockBundleRef_004
 * @tc.desc: Test AddLockBundleRef with empty lockPath
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, AddLockBundleRef_004, TestSize.Level1)
{
    int ret = AddLockBundleRef(0, "", "");
    // Should return error for empty lockPath
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

// ==================== ReleaseLockBundleRef Tests ====================

/**
 * @tc.name: ReleaseLockBundleRef_001
 * @tc.desc: Test ReleaseLockBundleRef decrements ref count
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, ReleaseLockBundleRef_001, TestSize.Level1)
{
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    AddLockBundleRef(1000, testBundle1, testLockPath1);

    ReleaseLockBundleRef(testLockPath1);

    // Ref count should be 1, entry still exists
    auto it = g_lockBundleMap.find(testLockPath1);
    EXPECT_NE(it, g_lockBundleMap.end());
    EXPECT_EQ(it->second.refCount, 1u);
    EXPECT_EQ(it->second.appUid, 1000u);
    EXPECT_EQ(it->second.bundleName, testBundle1);
}

/**
 * @tc.name: ReleaseLockBundleRef_002
 * @tc.desc: Test ReleaseLockBundleRef removes entry when ref count reaches 0
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, ReleaseLockBundleRef_002, TestSize.Level1)
{
    AddLockBundleRef(1000, testBundle1, testLockPath1);

    ReleaseLockBundleRef(testLockPath1);

    // Entry should be removed when ref count reaches 0
    EXPECT_EQ(g_lockBundleMap.find(testLockPath1), g_lockBundleMap.end());
    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

/**
 * @tc.name: ReleaseLockBundleRef_003
 * @tc.desc: Test ReleaseLockBundleRef for non-existent lockPath
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, ReleaseLockBundleRef_003, TestSize.Level1)
{
    // Should not crash for non-existent lockPath
    ReleaseLockBundleRef(testLockPath1);
    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

/**
 * @tc.name: ReleaseLockBundleRef_004
 * @tc.desc: Test ReleaseLockBundleRef with empty lockPath
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, ReleaseLockBundleRef_004, TestSize.Level1)
{
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    // Should handle gracefully
    ReleaseLockBundleRef("");
    // Map should be unchanged
    EXPECT_EQ(g_lockBundleMap.size(), 1u);
}

// ==================== Fine-grained Management Tests ====================

/**
 * @tc.name: FineGrainedManagement_001
 * @tc.desc: Test clone and non-clone have separate entries
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, FineGrainedManagement_001, TestSize.Level1)
{
    // Non-clone process starts
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    // Clone process starts (different lockPath)
    AddLockBundleRef(1000, testBundle1, testLockPathClone);

    // Should have two separate entries
    EXPECT_EQ(g_lockBundleMap.size(), 2u);
    EXPECT_EQ(g_lockBundleMap[testLockPath1].refCount, 1u);
    EXPECT_EQ(g_lockBundleMap[testLockPathClone].refCount, 1u);

    // Clone process exits
    ReleaseLockBundleRef(testLockPathClone);
    // Non-clone entry should still exist
    EXPECT_EQ(g_lockBundleMap.size(), 1u);
    EXPECT_NE(g_lockBundleMap.find(testLockPath1), g_lockBundleMap.end());
    // Clone entry should be removed
    EXPECT_EQ(g_lockBundleMap.find(testLockPathClone), g_lockBundleMap.end());

    // Non-clone process exits
    ReleaseLockBundleRef(testLockPath1);
    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

/**
 * @tc.name: FineGrainedManagement_002
 * @tc.desc: Test isolated and non-isolated have separate entries
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, FineGrainedManagement_002, TestSize.Level1)
{
    // Non-isolated process starts
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    // Isolated process starts (different lockPath)
    AddLockBundleRef(1000, testBundle1, testLockPathIsolated);

    // Should have two separate entries
    EXPECT_EQ(g_lockBundleMap.size(), 2u);

    // Both exit
    ReleaseLockBundleRef(testLockPath1);
    ReleaseLockBundleRef(testLockPathIsolated);

    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

/**
 * @tc.name: FineGrainedManagement_003
 * @tc.desc: Test multiple instances of same lockPath
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, FineGrainedManagement_003, TestSize.Level1)
{
    // Same lockPath with multiple references
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    AddLockBundleRef(1000, testBundle1, testLockPath1);

    EXPECT_EQ(g_lockBundleMap.size(), 1u);
    EXPECT_EQ(g_lockBundleMap[testLockPath1].refCount, 3u);

    // Release one
    ReleaseLockBundleRef(testLockPath1);
    EXPECT_EQ(g_lockBundleMap[testLockPath1].refCount, 2u);

    // Release all
    ReleaseLockBundleRef(testLockPath1);
    ReleaseLockBundleRef(testLockPath1);
    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

// ==================== Multiple Bundle Tests ====================

/**
 * @tc.name: MixedBundlesScenario_001
 * @tc.desc: Test mixed bundles scenario
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, MixedBundlesScenario_001, TestSize.Level1)
{
    // Bundle 1 with 2 processes (same lockPath)
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    AddLockBundleRef(1000, testBundle1, testLockPath1);

    // Bundle 2 with 1 process
    AddLockBundleRef(2000, testBundle2, testLockPath2);

    EXPECT_EQ(g_lockBundleMap.size(), 2u);

    // One process of bundle 1 exits
    ReleaseLockBundleRef(testLockPath1);
    EXPECT_EQ(g_lockBundleMap.size(), 2u);
    EXPECT_EQ(g_lockBundleMap[testLockPath1].refCount, 1u);

    // Bundle 2 exits completely
    ReleaseLockBundleRef(testLockPath2);
    EXPECT_EQ(g_lockBundleMap.size(), 1u);
    EXPECT_EQ(g_lockBundleMap.find(testLockPath2), g_lockBundleMap.end());

    // Last process of bundle 1 exits
    ReleaseLockBundleRef(testLockPath1);
    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

// ==================== Key Format Tests ====================

/**
 * @tc.name: LockBundleKeyFormat_001
 * @tc.desc: Test that key is the complete lockPath
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, LockBundleKeyFormat_001, TestSize.Level1)
{
    // Different lockPaths have separate entries
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    AddLockBundleRef(1000, testBundle1, testLockPathClone);
    AddLockBundleRef(1000, testBundle1, testLockPathIsolated);

    // Should have three entries with lockPath as key
    EXPECT_EQ(g_lockBundleMap.size(), 3u);
    EXPECT_NE(g_lockBundleMap.find(testLockPath1), g_lockBundleMap.end());
    EXPECT_NE(g_lockBundleMap.find(testLockPathClone), g_lockBundleMap.end());
    EXPECT_NE(g_lockBundleMap.find(testLockPathIsolated), g_lockBundleMap.end());

    // The lockPath stored should match the key
    EXPECT_EQ(g_lockBundleMap[testLockPath1].lockPath, testLockPath1);
    EXPECT_EQ(g_lockBundleMap[testLockPathClone].lockPath, testLockPathClone);
    EXPECT_EQ(g_lockBundleMap[testLockPathIsolated].lockPath, testLockPathIsolated);
}

// ==================== Edge Case Tests ====================

/**
 * @tc.name: EdgeCase_001
 * @tc.desc: Test empty lock path
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, EdgeCase_001, TestSize.Level1)
{
    int ret = AddLockBundleRef(0, "", "");
    // Should return error for empty lockPath
    EXPECT_EQ(ret, -1);
}

/**
 * @tc.name: EdgeCase_002
 * @tc.desc: Test release more times than add
 * @tc.type: FUNC
 * @tc.require: issueI:1876
 */
HWTEST_F(AppSpawnLockBundleTest, EdgeCase_002, TestSize.Level1)
{
    AddLockBundleRef(1000, testBundle1, testLockPath1);
    ReleaseLockBundleRef(testLockPath1);
    // Should not crash when releasing non-existent entry
    ReleaseLockBundleRef(testLockPath1);
    EXPECT_EQ(g_lockBundleMap.size(), 0u);
}

// ==================== DoSharedMount tests ====================

/**
 * @brief DoSharedMount with nullptr arg
 *        Covers: line 240-242 (arg == nullptr -> return APPSPAWN_ARG_INVALID)
 */
HWTEST_F(AppSpawnLockBundleTest, DoSharedMount_NullArg_001, TestSize.Level1)
{
    int ret = DoSharedMount(nullptr);
    EXPECT_EQ(ret, APPSPAWN_ARG_INVALID);
}

/**
 * @brief DoSharedMount with nullptr srcPath
 *        Covers: line 240-242 (arg->srcPath == nullptr -> return APPSPAWN_ARG_INVALID)
 */
HWTEST_F(AppSpawnLockBundleTest, DoSharedMount_NullSrcPath_001, TestSize.Level1)
{
    SharedMountArgs arg = {};
    arg.srcPath = nullptr;
    arg.destPath = "/tmp/test";
    int ret = DoSharedMount(&arg);
    EXPECT_EQ(ret, APPSPAWN_ARG_INVALID);
}

/**
 * @brief DoSharedMount with nullptr destPath
 *        Covers: line 240-242 (arg->destPath == nullptr -> return APPSPAWN_ARG_INVALID)
 */
HWTEST_F(AppSpawnLockBundleTest, DoSharedMount_NullDestPath_001, TestSize.Level1)
{
    SharedMountArgs arg = {};
    arg.srcPath = "/tmp/test";
    arg.destPath = nullptr;
    int ret = DoSharedMount(&arg);
    EXPECT_EQ(ret, APPSPAWN_ARG_INVALID);
}

// ==================== CheckPath tests ====================

/**
 * @brief CheckPath with various inputs
 *        Covers: line 330-333
 */
HWTEST_F(AppSpawnLockBundleTest, CheckPath_001, TestSize.Level1)
{
    EXPECT_FALSE(CheckPath(""));
    EXPECT_FALSE(CheckPath("."));
    EXPECT_FALSE(CheckPath(".."));
    EXPECT_FALSE(CheckPath("a/b"));
    EXPECT_TRUE(CheckPath("validname"));
}

// ==================== UpdateDataGroupDirs tests ====================

/**
 * @brief UpdateDataGroupDirs with null content
 *        Covers: line 468-469 (content == nullptr -> return APPSPAWN_ARG_INVALID)
 */
HWTEST_F(AppSpawnLockBundleTest, UpdateDataGroupDirs_NullContent_001, TestSize.Level1)
{
    int ret = UpdateDataGroupDirs(nullptr);
    EXPECT_EQ(ret, APPSPAWN_ARG_INVALID);
}

/**
 * @brief UpdateDataGroupDirs with empty queue
 *        Covers: line 472-498 (queue empty -> no iteration -> return 0)
 */
HWTEST_F(AppSpawnLockBundleTest, UpdateDataGroupDirs_EmptyQueue_001, TestSize.Level1)
{
    AppSpawnMgr mgr = {};
    OH_ListInit(&mgr.dataGroupCtxQueue);
    int ret = UpdateDataGroupDirs(&mgr);
    EXPECT_EQ(ret, 0);
}

// ==================== AddDataGroupItemToQueue tests ====================

/**
 * @brief AddDataGroupItemToQueue adds a new item
 *        Covers: line 351-374 (allocate, copy, add to queue)
 */
HWTEST_F(AppSpawnLockBundleTest, AddDataGroupItemToQueue_001, TestSize.Level1)
{
    AppSpawnMgr mgr = {};
    OH_ListInit(&mgr.dataGroupCtxQueue);
    int ret = AddDataGroupItemToQueue(&mgr, "/data/test/src", "/mnt/sandbox/test/dest", "uuid-1234");
    EXPECT_EQ(ret, 0);
    // Cleanup
    OH_ListRemoveAll(&mgr.dataGroupCtxQueue, nullptr);
}

/**
 * @brief AddDataGroupItemToQueue with duplicate entry (already exists)
 *        Covers: line 365-370 (node != nullptr -> free and return 0)
 */
HWTEST_F(AppSpawnLockBundleTest, AddDataGroupItemToQueue_Duplicate_001, TestSize.Level1)
{
    AppSpawnMgr mgr = {};
    OH_ListInit(&mgr.dataGroupCtxQueue);
    AddDataGroupItemToQueue(&mgr, "/data/test/src", "/mnt/sandbox/test/dest", "uuid-1234");
    int ret = AddDataGroupItemToQueue(&mgr, "/data/test/src", "/mnt/sandbox/test/dest", "uuid-1234");
    EXPECT_EQ(ret, 0);
    OH_ListRemoveAll(&mgr.dataGroupCtxQueue, nullptr);
}

// ==================== DumpDataGroupCtxQueue tests ====================

/**
 * @brief DumpDataGroupCtxQueue with null front
 *        Covers: line 379-380 (front == nullptr -> return)
 */
HWTEST_F(AppSpawnLockBundleTest, DumpDataGroupCtxQueue_Null_001, TestSize.Level1)
{
    DumpDataGroupCtxQueue(nullptr);
    // No crash
}

/**
 * @brief DumpDataGroupCtxQueue with populated queue
 *        Covers: line 383-394 (iterate and log)
 */
HWTEST_F(AppSpawnLockBundleTest, DumpDataGroupCtxQueue_Populated_001, TestSize.Level1)
{
    AppSpawnMgr mgr = {};
    OH_ListInit(&mgr.dataGroupCtxQueue);
    AddDataGroupItemToQueue(&mgr, "/data/test/src", "/mnt/sandbox/test/dest", "uuid-1234");
    DumpDataGroupCtxQueue(&mgr.dataGroupCtxQueue);
    OH_ListRemoveAll(&mgr.dataGroupCtxQueue, nullptr);
}

// ==================== DataGroupCtxNodeCompare tests ====================

/**
 * @brief DataGroupCtxNodeCompare with matching nodes
 *        Covers: line 335-348
 */
HWTEST_F(AppSpawnLockBundleTest, DataGroupCtxNodeCompare_001, TestSize.Level1)
{
    DataGroupCtx existing = {};
    DataGroupCtx newNode = {};
    strcpy_s(existing.srcPath.path, PATH_MAX_LEN - 1, "/data/src");
    strcpy_s(existing.destPath.path, PATH_MAX_LEN - 1, "/mnt/dest");
    strcpy_s(newNode.srcPath.path, PATH_MAX_LEN - 1, "/data/src");
    strcpy_s(newNode.destPath.path, PATH_MAX_LEN - 1, "/mnt/dest");
    int ret = DataGroupCtxNodeCompare(&existing.node, &newNode);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief DataGroupCtxNodeCompare with non-matching nodes
 */
HWTEST_F(AppSpawnLockBundleTest, DataGroupCtxNodeCompare_NoMatch_001, TestSize.Level1)
{
    DataGroupCtx existing = {};
    DataGroupCtx newNode = {};
    strcpy_s(existing.srcPath.path, PATH_MAX_LEN - 1, "/data/src1");
    strcpy_s(existing.destPath.path, PATH_MAX_LEN - 1, "/mnt/dest1");
    strcpy_s(newNode.srcPath.path, PATH_MAX_LEN - 1, "/data/src2");
    strcpy_s(newNode.destPath.path, PATH_MAX_LEN - 1, "/mnt/dest2");
    int ret = DataGroupCtxNodeCompare(&existing.node, &newNode);
    EXPECT_EQ(ret, 1);
}

// ==================== MountToShared tests ====================

/**
 * @brief MountToShared with null property
 *        Covers: line 539-544 (calls MountDirToShared which checks null)
 */
HWTEST_F(AppSpawnLockBundleTest, MountToShared_NullProperty_001, TestSize.Level1)
{
    AppSpawnMgr mgr = {};
    int ret = MountToShared(&mgr, nullptr);
    EXPECT_EQ(ret, 0);
}

// ==================== AppCleanupHook tests ====================

/**
 * @brief AppCleanupHook with null appInfo
 *        Covers: line 548-549 (appInfo == nullptr -> return 0)
 */
HWTEST_F(AppSpawnLockBundleTest, AppCleanupHook_NullAppInfo_001, TestSize.Level1)
{
    AppSpawnMgr mgr = {};
    mgr.content.mode = MODE_FOR_APP_SPAWN;
    int ret = AppCleanupHook(&mgr, nullptr);
    EXPECT_EQ(ret, 0);
}

// ==================== SpawnFailedHook tests ====================

/**
 * @brief SpawnFailedHook with null property
 *        Covers: line 564-565 (property == nullptr -> return 0)
 */
HWTEST_F(AppSpawnLockBundleTest, SpawnFailedHook_NullProperty_001, TestSize.Level1)
{
    AppSpawnMgr mgr = {};
    mgr.content.mode = MODE_FOR_APP_SPAWN;
    int ret = SpawnFailedHook(&mgr, nullptr);
    EXPECT_EQ(ret, 0);
}

}  // namespace OHOS
