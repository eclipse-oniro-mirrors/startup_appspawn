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
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "appspawn_server.h"
#include "appspawn_service.h"
#include "appspawn_manager.h"
#include "appspawn_msg.h"
#include "json_utils.h"
#include "sandbox_def.h"
#include "sandbox_core.h"
#include "sandbox_clone_app.h"
#include "securec.h"

#include "app_spawn_stub.h"
#include "app_spawn_test_helper.h"
#include "lib_wrapper.h"

using namespace testing;
using namespace testing::ext;
using namespace OHOS::AppSpawn;

namespace OHOS {
namespace AppSpawn {
// sandbox_clone_app.cpp内部函数: UT构建(APPSPAWN_TEST)下APPSPAWN_STATIC展开为空, 可直接链接调用
int32_t ParseCloneAppGrantItem(cJSON *item, CloneAppGrantExtData *grantData);
int32_t ReadCloneAppGrantDownloadJson(const std::string &configPath, CloneAppGrantExtData *grantData);
int32_t MatchGrantUriByAppIndex(const CloneAppGrantExtData *grantData, uint32_t appIndex,
    uint32_t &matchedGrantUri);
CloneAppGrantExtData *CreateCloneAppGrantExtData(AppSpawnMgr *content);
} // namespace AppSpawn
} // namespace OHOS

namespace OHOS {
AppSpawnTestHelper g_testHelperCloneApp;

// ==================== 测试辅助: 操作分身授权扩展数据 ====================

static int TestCompareExtDataId(ListNode *node, void *data)
{
    AppSpawnExtData *extData = ListEntry(node, AppSpawnExtData, node);
    return extData->dataId - *(uint32_t *)data;
}

static void FreeTestCloneAppGrantUri(ListNode *node)
{
    CloneAppGrantUri *entry = ListEntry(node, CloneAppGrantUri, node);
    free(entry);
}

// 移除mgr上已注册的分身授权扩展数据(含规则链表), 保证用例间互不影响
static void RemoveTestCloneAppGrantExtData(AppSpawnMgr *mgr)
{
    if (mgr == nullptr) {
        return;
    }
    uint32_t dataId = EXT_DATA_CLONE_APP_GRANT;
    ListNode *node = OH_ListFind(&mgr->extData, &dataId, TestCompareExtDataId);
    if (node == nullptr) {
        return;
    }
    AppSpawnExtData *extData = ListEntry(node, AppSpawnExtData, node);
    CloneAppGrantExtData *grantData = reinterpret_cast<CloneAppGrantExtData *>(extData);
    OH_ListRemoveAll(&grantData->grantQueue, FreeTestCloneAppGrantUri);
    OH_ListRemove(&grantData->extData.node);
    free(grantData);
}

// 在mgr上注册一个分身授权扩展数据节点, configLoaded由用例指定
static CloneAppGrantExtData *PrepareTestCloneAppGrantData(AppSpawnMgr *mgr, bool configLoaded)
{
    RemoveTestCloneAppGrantExtData(mgr);
    CloneAppGrantExtData *grantData =
        static_cast<CloneAppGrantExtData *>(calloc(1, sizeof(CloneAppGrantExtData)));
    APPSPAWN_CHECK(grantData != nullptr, return nullptr, "Failed to alloc clone app grant ext data");
    OH_ListInit(&grantData->extData.node);
    OH_ListInit(&grantData->grantQueue);
    grantData->configLoaded = configLoaded;
    grantData->grantCount = 0;
    grantData->extData.dataId = EXT_DATA_CLONE_APP_GRANT;
    grantData->extData.freeNode = nullptr;
    grantData->extData.dumpNode = nullptr;
    OH_ListAddTail(&mgr->extData, &grantData->extData.node);
    return grantData;
}

// 按配置顺序追加一条授权规则
static void AddTestCloneAppGrantRule(CloneAppGrantExtData *grantData, uint32_t grantUri,
    uint32_t indexMin, uint32_t indexMax)
{
    CloneAppGrantUri *entry = static_cast<CloneAppGrantUri *>(calloc(1, sizeof(CloneAppGrantUri)));
    APPSPAWN_CHECK(entry != nullptr, return, "Failed to alloc clone app grant uri");
    OH_ListInit(&entry->node);
    entry->grantUri = grantUri;
    entry->indexMin = indexMin;
    entry->indexMax = indexMax;
    OH_ListAddTail(&grantData->grantQueue, &entry->node);
    grantData->grantCount++;
}

// 清空规则链表(复用扩展数据节点时使用)
static void ClearTestCloneAppGrantRules(CloneAppGrantExtData *grantData)
{
    OH_ListRemoveAll(&grantData->grantQueue, FreeTestCloneAppGrantUri);
    OH_ListInit(&grantData->grantQueue);
    grantData->grantCount = 0;
}

// 统计extData链表上分身授权节点的个数
static int CountTestCloneAppGrantNodes(AppSpawnMgr *mgr)
{
    int count = 0;
    ListNode *node = nullptr;
    ForEachListEntry(&mgr->extData, node) {
        AppSpawnExtData *extData = ListEntry(node, AppSpawnExtData, node);
        if (extData->dataId == EXT_DATA_CLONE_APP_GRANT) {
            count++;
        }
    }
    return count;
}

// 构造带柔性数组成员的bundle信息
static AppSpawnMsgBundleInfo *MakeTestBundleInfo(uint32_t bundleIndex, const char *bundleName)
{
    size_t nameLen = strlen(bundleName) + 1;
    AppSpawnMsgBundleInfo *bundleInfo =
        static_cast<AppSpawnMsgBundleInfo *>(calloc(1, sizeof(AppSpawnMsgBundleInfo) + nameLen));
    APPSPAWN_CHECK(bundleInfo != nullptr, return nullptr, "Failed to alloc bundle info");
    bundleInfo->bundleIndex = bundleIndex;
    APPSPAWN_CHECK(strcpy_s(bundleInfo->bundleName, nameLen, bundleName) == 0,
        free(bundleInfo); return nullptr, "Failed to copy bundle name");
    return bundleInfo;
}

// 初始化栈上的授权扩展数据(不挂到mgr, 用于直接测试内部函数)
static void InitTestGrantData(CloneAppGrantExtData &grantData)
{
    OH_ListInit(&grantData.extData.node);
    OH_ListInit(&grantData.grantQueue);
    grantData.configLoaded = false;
    grantData.grantCount = 0;
}

// 清理栈上授权扩展数据的规则链表
static void ClearTestGrantData(CloneAppGrantExtData &grantData)
{
    OH_ListRemoveAll(&grantData.grantQueue, FreeTestCloneAppGrantUri);
    OH_ListInit(&grantData.grantQueue);
}

// 确保测试配置目录存在
static bool EnsureTestCfgDir()
{
    mkdir(APPSPAWN_BASE_DIR, 0755);  // ignore result, may already exist
    return mkdir(APPSPAWN_BASE_DIR "/clone_app_test", 0755) == 0 || errno == EEXIST;
}

// 写入测试用JSON配置文件
static bool WriteTestConfigFile(const char *fileName, const char *content, std::string &path)
{
    if (!EnsureTestCfgDir()) {
        return false;
    }
    path = APPSPAWN_BASE_DIR "/clone_app_test/";
    path += fileName;
    FILE *fp = fopen(path.c_str(), "w");
    if (fp == nullptr) {
        return false;
    }
    size_t len = strlen(content);
    bool ok = (fwrite(content, 1, len, fp) == len);
    fclose(fp);
    return ok;
}

class AppSpawnSandboxCloneAppTest : public testing::Test {
public:
    static void SetUpTestCase();
    static void TearDownTestCase();
    void SetUp();
    void TearDown();
};

void AppSpawnSandboxCloneAppTest::SetUpTestCase() {}

void AppSpawnSandboxCloneAppTest::TearDownTestCase() {}

void AppSpawnSandboxCloneAppTest::SetUp()
{
    const TestInfo *info = UnitTest::GetInstance()->current_test_info();
    GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " start";
    APPSPAWN_LOGI("%{public}s.%{public}s start", info->test_suite_name(), info->name());
}

void AppSpawnSandboxCloneAppTest::TearDown()
{
    // 清理挂到全局mgr上的分身授权扩展数据, 避免影响其他用例
    AppSpawnMgr *mgr = GetAppSpawnMgr();
    if (mgr != nullptr) {
        RemoveTestCloneAppGrantExtData(mgr);
    }

    const TestInfo *info = UnitTest::GetInstance()->current_test_info();
    GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " end";
    APPSPAWN_LOGI("%{public}s.%{public}s end", info->test_suite_name(), info->name());
}

static AppSpawningCtx *GetTestAppPropertyCloneApp()
{
    AppSpawnClientHandle clientHandle = nullptr;
    int ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
    APPSPAWN_CHECK(ret == 0, return nullptr, "Failed to create reqMgr");
    AppSpawnReqMsgHandle reqHandle = g_testHelperCloneApp.CreateMsg(clientHandle, MSG_APP_SPAWN, 0);
    APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, return nullptr, "Failed to create req");
    return g_testHelperCloneApp.GetAppProperty(clientHandle, reqHandle);
}

// ==================== 分身应用下载目录授权相关测试 ====================

/**
 * @tc.name: BuildClonePackageName_001
 * @tc.desc: Test BuildClonePackageName without clone app grant ext data
 *           Branch: grantData == nullptr → default GRANT_URI_CLONE_NAME → +clone-index+bundleName
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    RemoveTestCloneAppGrantExtData(mgr);

    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");  // 100 test index
    ASSERT_NE(bundleInfo, nullptr);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "+clone-100+com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_002
 * @tc.desc: Test BuildClonePackageName with ext data present but config not loaded
 *           Branch: configLoaded == false → default GRANT_URI_CLONE_NAME → +clone-index+bundleName
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_002, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, false);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 1, 200);

    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");  // 100 test index
    ASSERT_NE(bundleInfo, nullptr);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    // configLoaded == false, rules not applied
    EXPECT_STREQ(clonePackageName.str().c_str(), "+clone-100+com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_003
 * @tc.desc: Test BuildClonePackageName with matched rule grantUri = 0 (original name) and floorIndex = 0
 *           Branch: matchedGrantUri == GRANT_URI_ORIGINAL_NAME, floorIndex == 0 → plain bundleName
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_003, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 1, 200);

    // bundleIndex 100: matchIndex = 100 % 10000 = 100 in [1, 200], floorIndex = 100 / 10000 = 0
    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_004
 * @tc.desc: Test BuildClonePackageName with matched rule grantUri = 0 and bundleIndex >= 10000
 *           Branch: matchedGrantUri == GRANT_URI_ORIGINAL_NAME, floorIndex > 0
 *                 → +clone-(floorIndex * 10000)+bundleName
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_004, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 1, 200);

    // bundleIndex 20100: matchIndex = 20100 % 10000 = 100 in [1, 200], floorIndex = 20100 / 10000 = 2
    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(20100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "+clone-20000+com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_005
 * @tc.desc: Test BuildClonePackageName with matched rule grantUri non-zero (clone name)
 *           Branch: matchedGrantUri != GRANT_URI_ORIGINAL_NAME → +clone-bundleIndex+bundleName
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_005, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);

    // grantUri = 1 (GRANT_URI_CLONE_NAME)
    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_CLONE_NAME, 1, 200);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "+clone-100+com.example.myapplication");

    // grantUri = 2 (any non-zero value also means clone name)
    ClearTestCloneAppGrantRules(grantData);
    AddTestCloneAppGrantRule(grantData, 2, 1, 200);  // 2 test grant uri

    std::ostringstream clonePackageName2;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName2);
    EXPECT_STREQ(clonePackageName2.str().c_str(), "+clone-100+com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_006
 * @tc.desc: Test BuildClonePackageName when loaded rules have no match for the app index
 *           Branch: MatchGrantUriByAppIndex returns -1 → fallback GRANT_URI_CLONE_NAME
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_006, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 1, 50);

    // bundleIndex 100: matchIndex = 100 not in [1, 50] → no match → fallback clone name
    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "+clone-100+com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_007
 * @tc.desc: Test BuildClonePackageName with overlapping rules, first matched rule wins
 *           Branch: ForEachListEntry order → first rule in config order is applied
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_007, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    // two overlapping rules, the first one (original name) must win
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 100, 200);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_CLONE_NAME, 100, 200);

    // bundleIndex 20100: matchIndex = 100 matched by the first rule → original name, floorIndex = 2
    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(20100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "+clone-20000+com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_008
 * @tc.desc: Test BuildClonePackageName index boundary: matchIndex == indexMin / == indexMax
 *           are inclusive, indexMax + 1 does not match
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_008, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 100, 200);

    // matchIndex == indexMin (100): original name, floorIndex = 0
    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);
    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "com.example.myapplication");
    free(bundleInfo);

    // matchIndex == indexMax (200): original name, floorIndex = 1
    bundleInfo = MakeTestBundleInfo(10200, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);
    std::ostringstream clonePackageName2;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName2);
    EXPECT_STREQ(clonePackageName2.str().c_str(), "+clone-10000+com.example.myapplication");
    free(bundleInfo);

    // matchIndex == indexMax + 1 (201): no match → fallback clone name with raw bundleIndex
    bundleInfo = MakeTestBundleInfo(10201, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);
    std::ostringstream clonePackageName3;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName3);
    EXPECT_STREQ(clonePackageName3.str().c_str(), "+clone-10201+com.example.myapplication");
    free(bundleInfo);
}

/**
 * @tc.name: BuildClonePackageName_009
 * @tc.desc: Test BuildClonePackageName with loaded config but no valid rule
 *           Branch: configLoaded == true && grantCount == 0 → skip matching
 *                 → default GRANT_URI_CLONE_NAME clone name
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, BuildClonePackageName_009, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    // configLoaded == true but grantCount == 0, no rule appended

    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);

    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "+clone-100+com.example.myapplication");

    free(bundleInfo);
}

/**
 * @tc.name: LoadCloneAppGrantDownloadConfig_001
 * @tc.desc: Test LoadCloneAppGrantDownloadConfig with null content
 *           Branch: content == nullptr → return 0
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, LoadCloneAppGrantDownloadConfig_001, TestSize.Level0)
{
    int ret = SandboxCore::LoadCloneAppGrantDownloadConfig(nullptr);
    EXPECT_EQ(ret, 0);
}

/**
 * @tc.name: LoadCloneAppGrantDownloadConfig_002
 * @tc.desc: Test LoadCloneAppGrantDownloadConfig creates the clone app grant ext data on content
 *           Branch: grantData == nullptr → CreateCloneAppGrantExtData → node added into extData list
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, LoadCloneAppGrantDownloadConfig_002, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    RemoveTestCloneAppGrantExtData(mgr);

    int ret = SandboxCore::LoadCloneAppGrantDownloadConfig(mgr);
    EXPECT_EQ(ret, 0);

    uint32_t dataId = EXT_DATA_CLONE_APP_GRANT;
    ListNode *node = OH_ListFind(&mgr->extData, &dataId, TestCompareExtDataId);
    EXPECT_NE(node, nullptr);
}

/**
 * @tc.name: LoadCloneAppGrantDownloadConfig_003
 * @tc.desc: Test LoadCloneAppGrantDownloadConfig skips reload when config already loaded,
 *           pre-registered rules are preserved
 *           Branch: configLoaded == true → APPSPAWN_CHECK_LOGV early return 0
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, LoadCloneAppGrantDownloadConfig_003, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 1, 200);

    int ret = SandboxCore::LoadCloneAppGrantDownloadConfig(mgr);
    EXPECT_EQ(ret, 0);

    // early return: rules are not reloaded or cleared
    EXPECT_EQ(grantData->grantCount, 1);

    // rule still takes effect on package name build
    AppSpawnMsgBundleInfo *bundleInfo = MakeTestBundleInfo(100, "com.example.myapplication");
    ASSERT_NE(bundleInfo, nullptr);
    std::ostringstream clonePackageName;
    SandboxCore::BuildClonePackageName(bundleInfo, clonePackageName);
    EXPECT_STREQ(clonePackageName.str().c_str(), "com.example.myapplication");
    free(bundleInfo);
}

/**
 * @tc.name: LoadCloneAppGrantDownloadConfig_004
 * @tc.desc: Test LoadCloneAppGrantDownloadConfig reuses the existing ext data node on repeated calls,
 *           no duplicate node is added into the extData list
 *           Branch: grantData != nullptr → reuse
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, LoadCloneAppGrantDownloadConfig_004, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    RemoveTestCloneAppGrantExtData(mgr);

    ASSERT_EQ(SandboxCore::LoadCloneAppGrantDownloadConfig(mgr), 0);
    ASSERT_EQ(SandboxCore::LoadCloneAppGrantDownloadConfig(mgr), 0);

    EXPECT_EQ(CountTestCloneAppGrantNodes(mgr), 1);
}

/**
 * @tc.name: LoadCloneAppGrantDownloadConfig_005
 * @tc.desc: Test LoadCloneAppGrantDownloadConfig when GetCfgFiles returns nullptr
 *           Branch: files == nullptr → log warning and return 0, no rule loaded
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, LoadCloneAppGrantDownloadConfig_005, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    // 预置未加载状态的扩展数据节点, 绕开CreateCloneAppGrantExtData
    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, false);
    ASSERT_NE(grantData, nullptr);

    // 注入GetCfgFiles返回nullptr, 覆盖files == nullptr分支
    SetGetCfgFilesNull(true);
    int ret = SandboxCore::LoadCloneAppGrantDownloadConfig(mgr);
    SetGetCfgFilesNull(false);

    EXPECT_EQ(ret, 0);
    // 分支内提前return 0, 未加载任何规则, configLoaded保持false
    EXPECT_EQ(grantData->grantCount, 0u);
    EXPECT_FALSE(grantData->configLoaded);
}

/**
 * @tc.name: CreateCloneAppGrantExtData_001
 * @tc.desc: Test CreateCloneAppGrantExtData when calloc fails
 *           Branch: grantData == nullptr → return nullptr, no node added into extData list
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, CreateCloneAppGrantExtData_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    RemoveTestCloneAppGrantExtData(mgr);

    // 注入calloc失败
    CallocFunc callocFail = [](size_t m, size_t n) -> void * {
        return nullptr;
    };
    UpdateCallocFunc(callocFail);
    CloneAppGrantExtData *grantData = CreateCloneAppGrantExtData(mgr);
    UpdateCallocFunc(nullptr);

    EXPECT_EQ(grantData, nullptr);
    uint32_t dataId = EXT_DATA_CLONE_APP_GRANT;
    EXPECT_EQ(OH_ListFind(&mgr->extData, &dataId, TestCompareExtDataId), nullptr);

    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: FreeCloneAppGrantExtData_001
 * @tc.desc: Test clone app grant ext data is released through the registered freeNode
 *           callback when the mgr is destroyed
 *           Branch: DeleteAppSpawnMgr → ExtDataDestroy → FreeCloneAppGrantExtData
 *                 → DestroyCloneAppGrantUri for each queued rule
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, FreeCloneAppGrantExtData_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    RemoveTestCloneAppGrantExtData(mgr);

    // 通过被测函数创建扩展数据节点(内部已注册freeNode回调)
    int ret = SandboxCore::LoadCloneAppGrantDownloadConfig(mgr);
    ASSERT_EQ(ret, 0);
    uint32_t dataId = EXT_DATA_CLONE_APP_GRANT;
    ListNode *node = OH_ListFind(&mgr->extData, &dataId, TestCompareExtDataId);
    ASSERT_NE(node, nullptr);
    AppSpawnExtData *extData = ListEntry(node, AppSpawnExtData, node);
    CloneAppGrantExtData *grantData = reinterpret_cast<CloneAppGrantExtData *>(extData);
    // 设备etc/clone_app目录下可能存在真实配置, 上面的Load已加载其规则,
    // 先清空已加载规则, 保证断言不受设备环境影响
    ClearTestCloneAppGrantRules(grantData);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 1, 200);
    ASSERT_EQ(grantData->grantCount, 1u);

    // 删除mgr触发freeNode回调链, 释放规则链表与扩展数据节点
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(GetAppSpawnMgr(), nullptr);
}

/**
 * @tc.name: SetDecWithDir_CloneApp_001
 * @tc.desc: Test SetDecWithDir with APP_FLAGS_CLONE_ENABLE and bundleIndex > 0,
 *           clone package name is built through BuildClonePackageName
 *           Branch: flags == 0x1 → SandboxCore::BuildClonePackageName
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, SetDecWithDir_CloneApp_001, TestSize.Level0)
{
    g_testHelperCloneApp.SetProcessName("com.example.myapplication");
    g_testHelperCloneApp.SetTestApl("normal");

    AppSpawningCtx *appProperty = GetTestAppPropertyCloneApp();
    ASSERT_NE(appProperty, nullptr);
    // bundle index in msg is 100 (> 0), set clone enable flag
    int ret = SetAppSpawnMsgFlag(appProperty->message, TLV_MSG_FLAGS, APP_FLAGS_CLONE_ENABLE);
    ASSERT_EQ(ret, 0);

    uint32_t userId = 100;  // 100 test user id
    ret = SandboxCore::SetDecWithDir(appProperty, userId);
    EXPECT_EQ(ret, 0);

    DeleteAppSpawningCtx(appProperty);
}

/**
 * @tc.name: SetDecWithDir_CloneApp_002
 * @tc.desc: Test SetDecWithDir with clone enable flag and loaded grant rules,
 *           SetDecWithDir still succeeds with rule applied
 *           Branch: flags == 0x1 → BuildClonePackageName with matched grantUri rule
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, SetDecWithDir_CloneApp_002, TestSize.Level0)
{
    g_testHelperCloneApp.SetProcessName("com.example.myapplication");
    g_testHelperCloneApp.SetTestApl("normal");

    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    CloneAppGrantExtData *grantData = PrepareTestCloneAppGrantData(mgr, true);
    ASSERT_NE(grantData, nullptr);
    AddTestCloneAppGrantRule(grantData, SandboxCommonDef::GRANT_URI_ORIGINAL_NAME, 1, 200);

    AppSpawningCtx *appProperty = GetTestAppPropertyCloneApp();
    ASSERT_NE(appProperty, nullptr);
    int ret = SetAppSpawnMsgFlag(appProperty->message, TLV_MSG_FLAGS, APP_FLAGS_CLONE_ENABLE);
    ASSERT_EQ(ret, 0);

    uint32_t userId = 100;  // 100 test user id
    ret = SandboxCore::SetDecWithDir(appProperty, userId);
    EXPECT_EQ(ret, 0);

    DeleteAppSpawningCtx(appProperty);
}

// ==================== 内部函数直接测试: ParseCloneAppGrantItem ====================

/**
 * @tc.name: ParseCloneAppGrantItem_001
 * @tc.desc: Test ParseCloneAppGrantItem with a valid item
 *           Branch: all fields valid → entry appended, grantCount incremented, return 0
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ParseCloneAppGrantItem_001, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    cJSON *item = cJSON_Parse(R"({"grantUri":0,"indexMin":1,"indexMax":5})");
    ASSERT_NE(item, nullptr);
    int32_t ret = ParseCloneAppGrantItem(item, &grantData);
    EXPECT_EQ(ret, 0);
    cJSON_Delete(item);

    ASSERT_EQ(grantData.grantCount, 1u);
    CloneAppGrantUri *entry = ListEntry(grantData.grantQueue.next, CloneAppGrantUri, node);
    EXPECT_EQ(entry->grantUri, 0u);
    EXPECT_EQ(entry->indexMin, 1u);
    EXPECT_EQ(entry->indexMax, 5u);

    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ParseCloneAppGrantItem_002
 * @tc.desc: Test ParseCloneAppGrantItem with null item and non-object item
 *           Branch: item == nullptr / !cJSON_IsObject → return -1
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ParseCloneAppGrantItem_002, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    EXPECT_EQ(ParseCloneAppGrantItem(nullptr, &grantData), -1);

    cJSON *item = cJSON_Parse("123");  // 123 test: number node, not an object
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    EXPECT_EQ(grantData.grantCount, 0u);
    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ParseCloneAppGrantItem_003
 * @tc.desc: Test ParseCloneAppGrantItem with items missing required fields
 *           Branch: grantUriNode/indexMinNode/indexMaxNode == nullptr → return -1
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ParseCloneAppGrantItem_003, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    // missing grantUri
    cJSON *item = cJSON_Parse(R"({"indexMin":1,"indexMax":5})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    // missing indexMin
    item = cJSON_Parse(R"({"grantUri":0,"indexMax":5})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    // missing indexMax
    item = cJSON_Parse(R"({"grantUri":0,"indexMin":1})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    EXPECT_EQ(grantData.grantCount, 0u);
    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ParseCloneAppGrantItem_004
 * @tc.desc: Test ParseCloneAppGrantItem with non-number field values
 *           Branch: !cJSON_IsNumber → return -1
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ParseCloneAppGrantItem_004, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    cJSON *item = cJSON_Parse(R"({"grantUri":"0","indexMin":1,"indexMax":5})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    item = cJSON_Parse(R"({"grantUri":0,"indexMin":"1","indexMax":5})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    EXPECT_EQ(grantData.grantCount, 0u);
    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ParseCloneAppGrantItem_005
 * @tc.desc: Test ParseCloneAppGrantItem with negative field values
 *           Branch: valueint < 0 → return -1
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ParseCloneAppGrantItem_005, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    cJSON *item = cJSON_Parse(R"({"grantUri":-1,"indexMin":1,"indexMax":5})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    item = cJSON_Parse(R"({"grantUri":0,"indexMin":-1,"indexMax":5})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    EXPECT_EQ(grantData.grantCount, 0u);
    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ParseCloneAppGrantItem_006
 * @tc.desc: Test ParseCloneAppGrantItem with indexMin greater than indexMax
 *           Branch: indexMin > indexMax → return -1
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ParseCloneAppGrantItem_006, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    cJSON *item = cJSON_Parse(R"({"grantUri":0,"indexMin":6,"indexMax":5})");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ParseCloneAppGrantItem(item, &grantData), -1);
    cJSON_Delete(item);

    EXPECT_EQ(grantData.grantCount, 0u);
    ClearTestGrantData(grantData);
}

// ==================== 内部函数直接测试: ReadCloneAppGrantDownloadJson ====================

/**
 * @tc.name: ReadCloneAppGrantDownloadJson_001
 * @tc.desc: Test ReadCloneAppGrantDownloadJson with empty config path and null grantData
 *           Branch: configPath empty / grantData == nullptr → return APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ReadCloneAppGrantDownloadJson_001, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    EXPECT_EQ(ReadCloneAppGrantDownloadJson(std::string(""), &grantData), APPSPAWN_MSG_INVALID);
    EXPECT_EQ(ReadCloneAppGrantDownloadJson(std::string("/data/clone_app_ut.json"), nullptr),
        APPSPAWN_MSG_INVALID);

    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ReadCloneAppGrantDownloadJson_002
 * @tc.desc: Test ReadCloneAppGrantDownloadJson with a nonexistent config file
 *           Branch: GetJsonObjFromFile fails → return APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ReadCloneAppGrantDownloadJson_002, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    std::string path = APPSPAWN_BASE_DIR "/clone_app_test/not_exist_clone_app.json";
    EXPECT_EQ(ReadCloneAppGrantDownloadJson(path, &grantData), APPSPAWN_MSG_INVALID);

    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ReadCloneAppGrantDownloadJson_003
 * @tc.desc: Test ReadCloneAppGrantDownloadJson with root node not an object
 *           Branch: !cJSON_IsObject(root) → return APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ReadCloneAppGrantDownloadJson_003, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    std::string path;
    bool ok = WriteTestConfigFile("root_not_object.json", "[1, 2]", path);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ReadCloneAppGrantDownloadJson(path, &grantData), APPSPAWN_MSG_INVALID);

    unlink(path.c_str());
    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ReadCloneAppGrantDownloadJson_004
 * @tc.desc: Test ReadCloneAppGrantDownloadJson with cloneAppGrantDownloadUri missing or not an array
 *           Branch: grantUriArray == nullptr / !cJSON_IsArray → return APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ReadCloneAppGrantDownloadJson_004, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    std::string path;
    bool ok = WriteTestConfigFile("key_not_exist.json", R"({"other": []})", path);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ReadCloneAppGrantDownloadJson(path, &grantData), APPSPAWN_MSG_INVALID);
    unlink(path.c_str());

    ok = WriteTestConfigFile("key_not_array.json", R"({"cloneAppGrantDownloadUri": {"grantUri": 0}})", path);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ReadCloneAppGrantDownloadJson(path, &grantData), APPSPAWN_MSG_INVALID);
    unlink(path.c_str());

    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ReadCloneAppGrantDownloadJson_005
 * @tc.desc: Test ReadCloneAppGrantDownloadJson with a valid config file containing valid and
 *           invalid items, invalid items are skipped and overall return 0
 *           Branch: ParseCloneAppGrantItem returns -1 → continue; valid items appended
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ReadCloneAppGrantDownloadJson_005, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    std::string path;
    bool ok = WriteTestConfigFile("valid_clone_app.json",
        R"({"cloneAppGrantDownloadUri":[)"
        R"({"grantUri":0,"indexMin":1,"indexMax":5},)"
        R"({"grantUri":2,"indexMin":6,"indexMax":2},)"
        R"({"grantUri":1,"indexMin":6,"indexMax":9}]})",
        path);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ReadCloneAppGrantDownloadJson(path, &grantData), 0);
    // item 1 (indexMin > indexMax) is skipped, 2 valid rules remain
    ASSERT_EQ(grantData.grantCount, 2u);

    CloneAppGrantUri *first = ListEntry(grantData.grantQueue.next, CloneAppGrantUri, node);
    EXPECT_EQ(first->grantUri, 0u);
    EXPECT_EQ(first->indexMin, 1u);
    EXPECT_EQ(first->indexMax, 5u);

    unlink(path.c_str());
    ClearTestGrantData(grantData);
}

/**
 * @tc.name: ReadCloneAppGrantDownloadJson_006
 * @tc.desc: Test ReadCloneAppGrantDownloadJson with an empty rule array, return 0 and no rules
 *           Branch: cJSON_GetArraySize == 0 → loop not entered → return 0
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, ReadCloneAppGrantDownloadJson_006, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);

    std::string path;
    bool ok = WriteTestConfigFile("empty_clone_app.json", R"({"cloneAppGrantDownloadUri": []})", path);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ReadCloneAppGrantDownloadJson(path, &grantData), 0);
    EXPECT_EQ(grantData.grantCount, 0u);

    unlink(path.c_str());
    ClearTestGrantData(grantData);
}

// ==================== 内部函数直接测试: MatchGrantUriByAppIndex ====================

/**
 * @tc.name: MatchGrantUriByAppIndex_001
 * @tc.desc: Test MatchGrantUriByAppIndex with app index inside the rule range
 *           Branch: appIndex >= indexMin && appIndex <= indexMax → matchedGrantUri set, return 0
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, MatchGrantUriByAppIndex_001, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);
    AddTestCloneAppGrantRule(&grantData, 3, 10, 20);  // 3 test grant uri

    uint32_t matchedGrantUri = 0;
    EXPECT_EQ(MatchGrantUriByAppIndex(&grantData, 15, matchedGrantUri), 0);
    EXPECT_EQ(matchedGrantUri, 3u);

    ClearTestGrantData(grantData);
}

/**
 * @tc.name: MatchGrantUriByAppIndex_002
 * @tc.desc: Test MatchGrantUriByAppIndex with app index outside the rule range,
 *           matchedGrantUri keeps its original value
 *           Branch: no rule matches → return -1
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, MatchGrantUriByAppIndex_002, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);
    AddTestCloneAppGrantRule(&grantData, 3, 10, 20);  // 3 test grant uri

    uint32_t matchedGrantUri = 7;  // 7 test sentinel
    EXPECT_EQ(MatchGrantUriByAppIndex(&grantData, 9, matchedGrantUri), -1);   // below indexMin
    EXPECT_EQ(matchedGrantUri, 7u);
    EXPECT_EQ(MatchGrantUriByAppIndex(&grantData, 21, matchedGrantUri), -1);  // above indexMax
    EXPECT_EQ(matchedGrantUri, 7u);

    ClearTestGrantData(grantData);
}

/**
 * @tc.name: MatchGrantUriByAppIndex_003
 * @tc.desc: Test MatchGrantUriByAppIndex with overlapping rules, first matched rule wins
 *           Branch: ForEachListEntry stops at the first matching entry
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, MatchGrantUriByAppIndex_003, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);
    AddTestCloneAppGrantRule(&grantData, 3, 10, 20);  // 3 test grant uri
    AddTestCloneAppGrantRule(&grantData, 4, 10, 20);  // 4 test grant uri

    uint32_t matchedGrantUri = 0;
    EXPECT_EQ(MatchGrantUriByAppIndex(&grantData, 15, matchedGrantUri), 0);
    EXPECT_EQ(matchedGrantUri, 3u);  // first rule wins

    ClearTestGrantData(grantData);
}

/**
 * @tc.name: MatchGrantUriByAppIndex_004
 * @tc.desc: Test MatchGrantUriByAppIndex boundary, indexMin and indexMax are inclusive
 *           Branch: appIndex == indexMin / appIndex == indexMax → match
 * @tc.type: FUNC
 * @tc.require: issueI5NTX6
 */
HWTEST_F(AppSpawnSandboxCloneAppTest, MatchGrantUriByAppIndex_004, TestSize.Level0)
{
    CloneAppGrantExtData grantData;
    InitTestGrantData(grantData);
    AddTestCloneAppGrantRule(&grantData, 3, 10, 20);  // 3 test grant uri

    uint32_t matchedGrantUri = 0;
    EXPECT_EQ(MatchGrantUriByAppIndex(&grantData, 10, matchedGrantUri), 0);  // == indexMin
    EXPECT_EQ(matchedGrantUri, 3u);
    EXPECT_EQ(MatchGrantUriByAppIndex(&grantData, 20, matchedGrantUri), 0);  // == indexMax
    EXPECT_EQ(matchedGrantUri, 3u);

    ClearTestGrantData(grantData);
}
} // namespace OHOS
