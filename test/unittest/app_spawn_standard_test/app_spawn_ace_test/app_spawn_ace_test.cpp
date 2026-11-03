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

#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <gtest/gtest.h>
#include <cJSON.h>

#include "appspawn.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_msg.h"
#include "appspawn_server.h"
#include "appspawn_service.h"
#include "securec.h"
#include "appspawn_ace_mock_test.h"

using namespace testing;
using namespace testing::ext;
using namespace OHOS;

APPSPAWN_STATIC int RunChildThread(const AppSpawnMgr *content, const AppSpawningCtx *property);
APPSPAWN_STATIC int RunChildByRenderCmd(const AppSpawnMgr *content, const AppSpawningCtx *property);
APPSPAWN_STATIC int PreLoadAppSpawn(AppSpawnMgr *content);
APPSPAWN_STATIC int PreLinkAppSpawn(AppSpawnMgr *content);
APPSPAWN_STATIC int DlopenAppSpawn(AppSpawnMgr *content);
APPSPAWN_STATIC int ProcessSpawnDlopenMsg(AppSpawnMgr *content);
APPSPAWN_STATIC int ProcessSpawnDlcloseMsg(AppSpawnMgr *content);
APPSPAWN_STATIC int PrelinkerMain(int prelinkMemfd);
APPSPAWN_STATIC void PrelinkLibs();
APPSPAWN_STATIC bool IsPrelinkEnable();
APPSPAWN_STATIC void GetNames(const cJSON *root, std::set<std::string> &names, std::string key);
APPSPAWN_STATIC int BuildFdInfoMap(const AppSpawnMsgNode *message, std::map<std::string, int> &fdMap, int isColdRun);
APPSPAWN_STATIC void ClearEnvAndReturnSuccess(AppSpawnContent *content, AppSpawnClient *client);

namespace OHOS {
class AppSpawnAceTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
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

/**
 * @brief Enable prelink
 * @note 预期结果: 正常 prelink
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Prelink_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetParameter("const.startup.prelink.enable", "true");
    int ret = PreLinkAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(GetIsExecutedDlprelinkRegister(), true);
    ClearIsExecutedDlprelinkRegister();
}

/**
 * @brief run mode: MODE_FOR_NWEB_SPAWN 不支持 prelink
 * @note 预期结果: 直接返回 0
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Prelink_002, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(NWEBSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetParameter("const.startup.prelink.enable", "true");
    int ret = PreLinkAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    ClearIsExecutedDlprelinkRegister();
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(GetIsExecutedDlprelinkRegister(), false);
}

/**
 * @brief run mode: MODE_FOR_NWEB_COLD_RUN 不支持 prelink
 * @note 预期结果: 直接返回 0
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Prelink_003, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_COLD_RUN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(NWEBSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetParameter("const.startup.prelink.enable", "true");
    int ret = PreLinkAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    ClearIsExecutedDlprelinkRegister();
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(GetIsExecutedDlprelinkRegister(), false);
}

/**
 * @brief Disable prelink
 * @note 预期结果: 在 IsPrelinkEnable 判断失败，直接返回，不会进行 prelink
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Prelink_004, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetParameter("const.startup.prelink.enable", "false");
    int ret = PreLinkAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    ClearIsExecutedDlprelinkRegister();
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(GetIsExecutedDlprelinkRegister(), false);
}

/**
 * @brief const.startup.prelink.enable 参数不存在
 * @note 预期结果: 在 IsPrelinkEnable 判断失败，直接返回，不会进行 prelink
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Prelink_005, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    int ret = PreLinkAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    ClearIsExecutedDlprelinkRegister();
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(GetIsExecutedDlprelinkRegister(), false);
}

/**
 * @brief 注入 dlprelink_reserve_mem 分配失败，
 * @note 预期结果: 直接返回，不会进行 prelink
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Prelink_006, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetParameter("const.startup.prelink.enable", "true");
    SetMockDlprelinkReserveMemFailed(true);
    int ret = PreLinkAppSpawn(mgr);
    SetMockDlprelinkReserveMemFailed(false);
    DeleteAppSpawnMgr(mgr);
    ClearIsExecutedDlprelinkRegister();
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(GetIsExecutedDlprelinkRegister(), false);
}

/**
 * @brief 注入 dlprelink_record 失败，
 * @note 预期结果: 直接返回，不会进行 prelink
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Prelink_007, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetParameter("const.startup.prelink.enable", "true");
    SetMockDlprelinkRecordFailed(true);
    int ret = PreLinkAppSpawn(mgr);
    SetMockDlprelinkRecordFailed(false);
    ClearIsExecutedDlprelinkRegister();
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(GetIsExecutedDlprelinkRegister(), false);
}

/**
 * @brief nwebspawn进行预加载
 * @note 预期结果: nwebspawn不支持预加载，直接返回
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(NWEBSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    int ret = PreLoadAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief cjappspawn进行预加载
 * @note 预期结果: cjappspawn通过processName校验，执行LoadExtendCJLib预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_002, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_CJAPP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(CJAPPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    int ret = PreLoadAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief 未设置preload参数时（使用参数默认值），appspawn进行预加载
 * @note 预期结果: appspawn根据参数默认值判断是否执行preload预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_003, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    int ret = PreLoadAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief preload参数设置为true时，测试appspawn进行预加载
 * @note 预期结果: appspawn执行preload预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_004, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetBoolParamResult("persist.appspawn.preload", true);
    int ret = PreLoadAppSpawn(mgr);
    SetBoolParamResult("persist.appspawn.preload", false);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief preload参数和preloadets参数均设置为true时，测试appspawn进行预加载
 * @note 预期结果: appspawn执行preload和preloadets预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_005, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetBoolParamResult("persist.appspawn.preload", true);
    SetBoolParamResult("persist.appspawn.preloadets", true);
    int ret = PreLoadAppSpawn(mgr);
    SetBoolParamResult("persist.appspawn.preload", false);
    SetBoolParamResult("persist.appspawn.preloadets", false);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief preload、preloadets和unified参数均设置为true时，测试appspawn进行预加载
 * @note 预期结果: appspawn执行preload和preloadets预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_006, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetBoolParamResult("persist.appspawn.preload", true);
    SetBoolParamResult("persist.appspawn.preloadets", true);
    SetBoolParamResult("persist.appspawn.hybridspawn.unified", true);
    int ret = PreLoadAppSpawn(mgr);
    SetBoolParamResult("persist.appspawn.preload", false);
    SetBoolParamResult("persist.appspawn.preloadets", false);
    SetBoolParamResult("persist.appspawn.hybridspawn.unified", false);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief 未设置preload参数时（使用参数默认值），hybridspawn进行预加载
 * @note 预期结果: hybridspawn根据参数默认值判断是否执行preload预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_007, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_HYBRID_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(HYBRIDSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    int ret = PreLoadAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief preload参数设置为true时，测试hybridspawn进行预加载
 * @note 预期结果: hybridspawn执行preload预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_008, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_HYBRID_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(HYBRIDSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetBoolParamResult("persist.appspawn.preload", true);
    int ret = PreLoadAppSpawn(mgr);
    SetBoolParamResult("persist.appspawn.preload", false);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief preload参数和preloadets参数均设置为true时，测试hybridspawn进行预加载
 * @note 预期结果: hybridspawn执行preload和preloadets预加载
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Preload_009, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_HYBRID_SPAWN);
    ASSERT_NE(mgr, nullptr);
    mgr->content.longProcName = const_cast<char *>(HYBRIDSPAWN_SERVER_NAME);
    mgr->content.longProcNameLen = APP_LEN_PROC_NAME;

    SetBoolParamResult("persist.appspawn.preload", true);
    SetBoolParamResult("persist.appspawn.preloadets", true);
    int ret = PreLoadAppSpawn(mgr);
    SetBoolParamResult("persist.appspawn.preload", false);
    SetBoolParamResult("persist.appspawn.preloadets", false);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief nwebspawn进行加载system libs
 * @note 预期结果: nwebspawn不支持加载system libs，直接返回
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Dlopen_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);
    ASSERT_NE(mgr, nullptr);

    int ret = DlopenAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief appspawn进行加载system libs
 * @note 预期结果: appspawn加载system libs正常
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_Dlopen_002, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    int ret = DlopenAppSpawn(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief appspawn执行RunChildThread
 * @note 预期结果: 执行正常，分支覆盖
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_RunChild_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = CreateAppSpawningCtx();
    ASSERT_NE(property, nullptr);
    property->message = CreateAppSpawnMsg();
    ASSERT_NE(property->message, nullptr);

    int ret = RunChildThread(mgr, property);
    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief appspawn执行RunChildByRenderCmd
 * @note 预期结果: 执行正常，分支覆盖
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_RunChild_002, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = CreateAppSpawningCtx();
    ASSERT_NE(property, nullptr);
    property->message = CreateAppSpawnMsg();
    ASSERT_NE(property->message, nullptr);

    int ret = RunChildByRenderCmd(mgr, property);
    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief Appspawn dlopen arkweb so
 * @note Expected result: Normal execution
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_ProcessSpawnDlopenMsg_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    int ret = ProcessSpawnDlopenMsg(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief Appspawn dlclose arkweb so
 * @note Expected result: Normal execution
 *
 */
HWTEST_F(AppSpawnAceTest, App_Spawn_Ace_ProcessSpawnDlcloseMsg_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);

    int ret = ProcessSpawnDlcloseMsg(mgr);
    DeleteAppSpawnMgr(mgr);
    EXPECT_EQ(ret, 0);
}

// ==================== GetNames tests ====================

/**
 * @brief GetNames with valid JSON array
 *        Covers: line 72-86, names inserted into set
 */
HWTEST_F(AppSpawnAceTest, GetNames_Valid_001, TestSize.Level0)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("module1"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("module2"));
    cJSON_AddItemToObject(root, "napi", arr);

    std::set<std::string> names;
    GetNames(root, names, "napi");
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names.count("module1"), 1u);
    EXPECT_EQ(names.count("module2"), 1u);

    cJSON_Delete(root);
}

/**
 * @brief GetNames with key not found
 *        Covers: namesJson == nullptr -> return
 */
HWTEST_F(AppSpawnAceTest, GetNames_KeyNotFound_001, TestSize.Level0)
{
    cJSON *root = cJSON_CreateObject();
    std::set<std::string> names;
    GetNames(root, names, "nonexistent");
    EXPECT_EQ(names.size(), 0u);
    cJSON_Delete(root);
}

/**
 * @brief GetNames with null entries in array
 *        Covers: name == nullptr -> continue
 */
HWTEST_F(AppSpawnAceTest, GetNames_NullEntry_001, TestSize.Level0)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNull());
    cJSON_AddItemToArray(arr, cJSON_CreateString("valid"));
    cJSON_AddItemToObject(root, "napi", arr);

    std::set<std::string> names;
    GetNames(root, names, "napi");
    EXPECT_EQ(names.size(), 1u);

    cJSON_Delete(root);
}

// ==================== IsPrelinkEnable tests ====================

/**
 * @brief IsPrelinkEnable with param set to "true"
 *        Covers: strcmp(buffer, "true") == 0 -> return true
 */
HWTEST_F(AppSpawnAceTest, IsPrelinkEnable_True_001, TestSize.Level0)
{
    SetParameter("const.startup.prelink.enable", "true");
    bool ret = IsPrelinkEnable();
    EXPECT_TRUE(ret);
}

/**
 * @brief IsPrelinkEnable with param set to "false"
 *        Covers: strcmp != 0 -> return false
 */
HWTEST_F(AppSpawnAceTest, IsPrelinkEnable_False_001, TestSize.Level0)
{
    SetParameter("const.startup.prelink.enable", "false");
    bool ret = IsPrelinkEnable();
    EXPECT_FALSE(ret);
}

// ==================== PrelinkerMain tests ====================

/**
 * @brief PrelinkerMain with invalid fd (fcntl fails)
 *        Covers: fcntl fails -> return -1
 */
HWTEST_F(AppSpawnAceTest, PrelinkerMain_InvalidFd_001, TestSize.Level0)
{
    int ret = PrelinkerMain(-1);
    EXPECT_EQ(ret, -1);
}

// ==================== BuildFdInfoMap tests ====================

/**
 * @brief BuildFdInfoMap with null message
 *        Covers: message == nullptr -> return -1
 */
HWTEST_F(AppSpawnAceTest, BuildFdInfoMap_NullMessage_001, TestSize.Level0)
{
    std::map<std::string, int> fdMap;
    int ret = BuildFdInfoMap(nullptr, fdMap, 0);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief BuildFdInfoMap with null buffer
 *        Covers: message->buffer == nullptr -> return -1
 */
HWTEST_F(AppSpawnAceTest, BuildFdInfoMap_NullBuffer_001, TestSize.Level0)
{
    AppSpawnMsgNode msgNode = {};
    msgNode.buffer = nullptr;
    msgNode.tlvOffset = nullptr;
    std::map<std::string, int> fdMap;
    int ret = BuildFdInfoMap(&msgNode, fdMap, 0);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief BuildFdInfoMap with null tlvOffset
 *        Covers: message->tlvOffset == nullptr -> return -1
 */
HWTEST_F(AppSpawnAceTest, BuildFdInfoMap_NullTlvOffset_001, TestSize.Level0)
{
    uint8_t buffer[64] = {0};
    AppSpawnMsgNode msgNode = {};
    msgNode.buffer = buffer;
    msgNode.tlvOffset = nullptr;
    std::map<std::string, int> fdMap;
    int ret = BuildFdInfoMap(&msgNode, fdMap, 0);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief BuildFdInfoMap non-coldrun with null connection
 *        Covers: message->connection == nullptr -> return -1
 */
HWTEST_F(AppSpawnAceTest, BuildFdInfoMap_NullConnection_001, TestSize.Level0)
{
    uint8_t buffer[64] = {0};
    uint32_t offsets[TLV_MAX + 2];
    for (uint32_t i = 0; i < TLV_MAX + 2; i++) {
        offsets[i] = INVALID_OFFSET;
    }
    AppSpawnMsgNode msgNode = {};
    msgNode.buffer = buffer;
    msgNode.tlvOffset = offsets;
    msgNode.tlvCount = 0;
    msgNode.connection = nullptr;
    std::map<std::string, int> fdMap;
    int ret = BuildFdInfoMap(&msgNode, fdMap, 0);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief BuildFdInfoMap non-coldrun with zero fdCount
 *        Covers: recvCtx.fdCount <= 0 -> return 0
 */
HWTEST_F(AppSpawnAceTest, BuildFdInfoMap_ZeroFdCount_001, TestSize.Level0)
{
    uint8_t buffer[64] = {0};
    uint32_t offsets[TLV_MAX + 2];
    for (uint32_t i = 0; i < TLV_MAX + 2; i++) {
        offsets[i] = INVALID_OFFSET;
    }
    AppSpawnConnection conn = {};
    conn.receiverCtx.fdCount = 0;
    AppSpawnMsgNode msgNode = {};
    msgNode.buffer = buffer;
    msgNode.tlvOffset = offsets;
    msgNode.tlvCount = 0;
    msgNode.connection = &conn;
    std::map<std::string, int> fdMap;
    int ret = BuildFdInfoMap(&msgNode, fdMap, 0);
    EXPECT_EQ(ret, 0);
}

/**
 * @brief BuildFdInfoMap with invalid tlv offset
 *        Covers: tlvOffset[index] == INVALID_OFFSET -> return -1
 */
HWTEST_F(AppSpawnAceTest, BuildFdInfoMap_InvalidOffset_001, TestSize.Level0)
{
    uint8_t buffer[64] = {0};
    uint32_t offsets[TLV_MAX + 2];
    for (uint32_t i = 0; i < TLV_MAX + 2; i++) {
        offsets[i] = INVALID_OFFSET;
    }
    AppSpawnConnection conn = {};
    conn.receiverCtx.fdCount = 3;
    AppSpawnMsgNode msgNode = {};
    msgNode.buffer = buffer;
    msgNode.tlvOffset = offsets;
    msgNode.tlvCount = 1;
    msgNode.connection = &conn;
    std::map<std::string, int> fdMap;
    int ret = BuildFdInfoMap(&msgNode, fdMap, 0);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief BuildFdInfoMap coldrun with env fd
 *        Covers: isColdRun path, getenv -> fdMap populated
 */
HWTEST_F(AppSpawnAceTest, BuildFdInfoMap_ColdRun_001, TestSize.Level0)
{
    uint8_t buffer[256] = {0};
    uint32_t offsets[TLV_MAX + 2];
    for (uint32_t i = 0; i < TLV_MAX + 2; i++) {
        offsets[i] = INVALID_OFFSET;
    }

    AppSpawnTlvExt tlv = {};
    tlv.tlvType = TLV_MAX;
    sprintf_s(tlv.tlvName, sizeof(tlv.tlvName), "%s", MSG_EXT_NAME_APP_FD);
    const char *key = "ipc-fd";
    tlv.dataLen = strlen(key) + 1;
    tlv.tlvLen = sizeof(AppSpawnTlvExt) + APPSPAWN_ALIGN(tlv.dataLen);
    memcpy_s(buffer, sizeof(buffer), &tlv, sizeof(tlv));
    memcpy_s(buffer + sizeof(AppSpawnTlvExt), sizeof(buffer) - sizeof(AppSpawnTlvExt), key, tlv.dataLen);
    offsets[TLV_MAX] = 0;

    AppSpawnMsgNode msgNode = {};
    msgNode.buffer = buffer;
    msgNode.tlvOffset = offsets;
    msgNode.tlvCount = 1;

    setenv("APPSPAWN_FD_ipc-fd", "10", 1);
    std::map<std::string, int> fdMap;
    int ret = BuildFdInfoMap(&msgNode, fdMap, 1);
    EXPECT_EQ(ret, 0);
    unsetenv("APPSPAWN_FD_ipc-fd");
}

// ==================== ClearEnvAndReturnSuccess tests ====================

/**
 * @brief ClearEnvAndReturnSuccess with null params
 *        Covers: content == nullptr -> return
 */
HWTEST_F(AppSpawnAceTest, ClearEnvAndReturnSuccess_NullParam_001, TestSize.Level0)
{
    ClearEnvAndReturnSuccess(nullptr, nullptr);
    // No crash expected
}

}   // namespace OHOS
