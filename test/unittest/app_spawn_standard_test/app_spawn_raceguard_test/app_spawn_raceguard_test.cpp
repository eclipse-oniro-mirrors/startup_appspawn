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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include <gtest/gtest.h>

#include "appspawn_modulemgr.h"
#include "appspawn_server.h"
#include "appspawn_manager.h"
#include "appspawn.h"
#include "appspawn_utils.h"
#include "parameter.h"
#include "securec.h"

#include "raceguard_test_helper.h"

#ifdef __cplusplus
extern "C" {
#endif
APPSPAWN_STATIC int BuildRaceGuardPreloadValue(char *buf, size_t bufLen);
APPSPAWN_STATIC int SetRaceGuardSpawningFlag(AppSpawnMgr *content, AppSpawningCtx *property);
APPSPAWN_STATIC int SetRaceGuardPreloadEnv(AppSpawnMgr *content, AppSpawningCtx *property);
#ifdef __cplusplus
}
#endif

// 与 raceguard_detector.c 编译期常量保持一致的镜像值（刻意为之，便于断言）
#define TEST_RACEGUARD_LD_PRELOAD "/system/asan/lib64/chipset-sdk-sp/libraceguard_rt.z.so"
#define TEST_RACEGUARD_COLD_RUN_PATH "/system/asan/bin/appspawn"
#define TEST_BUNDLE_NAME "com.example.myapplication"

using namespace testing;
using namespace testing::ext;
using namespace OHOS;

namespace OHOS {
class AppSpawnRaceGuardTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
        // 每用例前清理 LD_PRELOAD，避免用例间污染
        unsetenv("LD_PRELOAD");
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " start";
        APPSPAWN_LOGI("%{public}s.%{public}s start", info->test_suite_name(), info->name());
    }
    void TearDown()
    {
        unsetenv("LD_PRELOAD");
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " end";
        APPSPAWN_LOGI("%{public}s.%{public}s end", info->test_suite_name(), info->name());
    }

    // 检测 RaceGuard so 是否真实存在于测试环境
    static bool IsRaceGuardSoPresent()
    {
        return access(TEST_RACEGUARD_LD_PRELOAD, F_OK) == 0;
    }

    // 检测冷启动入口是否存在且可执行（与父 hook 的 F_OK + X_OK 降级检查对齐）
    static bool IsRaceGuardEntryReady()
    {
        return access(TEST_RACEGUARD_COLD_RUN_PATH, F_OK) == 0 &&
               access(TEST_RACEGUARD_COLD_RUN_PATH, X_OK) == 0;
    }

    // 释放 property->forkCtx.coldRunPath并置NULL, 防止用例间泄露
    static void FreeColdRunPath(AppSpawningCtx *property)
    {
        if (property != nullptr && property->forkCtx.coldRunPath != nullptr) {
            free(property->forkCtx.coldRunPath);
            property->forkCtx.coldRunPath = nullptr;
        }
    }

    // 设置 wrap.<bundleName>系统参数为"raceguard_wrapper"，模拟应用配置了 raceguard
    static void SetRaceGuardWrapParam(const char *bundleName)
    {
        char key[128] = {0};
        int len = sprintf_s(key, sizeof(key), "wrap.%s", bundleName);
        if (len <= 0) {
            APPSPAWN_LOGW("SetRaceGuardWrapParam format wrapKey fail, len=%{public}d", len);
            return;
        }
        SetParameter(key, "raceguard_wrapper");
    }

    // 清除 wrap.<bundleName>系统参数
    static void ClearRaceGuardWrapParam(const char *bundleName)
    {
        char key[128] = {0};
        int len = sprintf_s(key, sizeof(key), "wrap.%s", bundleName);
        if (len <= 0) {
            APPSPAWN_LOGW("ClearRaceGuardWrapParam format wrapKey fail, len=%{public}d", len);
            return;
        }
        SetParameter(key, "");
    }

    // 校验 wrap 参数是否已生效为 "raceguard_wrapper"（主机侧参数可能只读/不可写）
    static bool IsRaceGuardWrapOk()
    {
        char wrapVal[128] = {0};
        return GetParameter("wrap." TEST_BUNDLE_NAME, "", wrapVal, sizeof(wrapVal)) > 0 &&
               strcmp(wrapVal, "raceguard_wrapper") == 0;
    }
};

/* =========================================================================
 *          SetRaceGuardSpawningFlag 用例（父进程 STAGE_PARENT_PRE_FORK）
 * ========================================================================= */

/**
 * @brief 空指针入参 → 返回 0 不崩溃（参数守卫分支）
 *        覆盖: APPSPAWN_CHECK(content != NULL && property != NULL, return 0)
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentNullPointer01, TestSize.Level0)
{
    EXPECT_EQ(SetRaceGuardSpawningFlag(NULL, NULL), 0);
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    EXPECT_EQ(SetRaceGuardSpawningFlag(mgr, NULL), 0);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @brief nweb 模式不判定冷启动（模式守卫分支，先于 wrap 判定，无需设置 wrap）
 *        覆盖: !IsAppSpawnMode(content) → return 0
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentModeGuard01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // nweb 模式：不置任何冷启动标志
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief MSG_SPAWN_NATIVE_PROCESS 消息不判定冷启动（消息类型守卫分支，先于 wrap 判定）
 *        覆盖: GetAppSpawnMsgType == MSG_SPAWN_NATIVE_PROCESS → return 0
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentNativeSkip01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_SPAWN_NATIVE_PROCESS);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief daemon 孵化（MSG_SPAWN_SERVICE_BIN + APP_FLAGS_DAEMON_BIN）→ 父阶段排除, 不判定冷启动
 *        覆盖: IsDaemonBinProperty(property) → return 0（uid0/全能力的 daemon 不注入 raceguard）
 *        构造: 参 app_spawn_daemon_spawn_test AddDaemonStandardTlvs 模式——
 *              AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN) + bundleInfo/token/domain/dac
 *              最小 TLV 集 + AppSpawnReqMsgSetAppFlag(APP_FLAGS_DAEMON_BIN)
 *              (公共 API 处理 49 号标志跨 flags[0]/flags[1] 分块, 不能手工 1U<<),
 *              并 SetNoShareFsEnable(true) 打开 daemon 判定开关, 结束恢复。
 *        说明: daemon 排除位于 wrap 守卫之前, 无需 wrap/so 即可验证。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentDaemonSkip01, TestSize.Level0)
{
    bool previousNoShareFs = IsNoShareFsEnable();
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        // 打开 daemon 判定开关（IsDaemonBinProperty 前置: IsNoShareFsEnable）
        SetNoShareFsEnable(true);

        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = 0;
        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, TEST_BUNDLE_NAME, &reqHandle);
        APPSPAWN_CHECK(ret == 0, ret = 0; break, "Failed to create daemon req");
        // daemon 报文最小 TLV 集（参 daemon_spawn_test AddDaemonStandardTlvs）
        APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetBundleInfo(reqHandle, 100, TEST_BUNDLE_NAME) == 0, ret = -1);
        APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppAccessToken(reqHandle, 0x20003039) == 0, ret = -1);
        APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppDomainInfo(reqHandle, 1, "system_core") == 0, ret = -1);
        AppDacInfo dacInfo = {};
        dacInfo.uid = 20010029;  // 合法 app uid 占位（>= MIN_VALID_APP_UID）
        dacInfo.gid = 20010029;
        dacInfo.gidCount = 1;
        dacInfo.gidTable[0] = 20010029;
        APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppDacInfo(reqHandle, &dacInfo) == 0, ret = -1);
        APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_DAEMON_BIN) == 0, ret = -1);
        if (ret != 0) {
            APPSPAWN_LOGW("Failed to fill daemon msg tlvs");
            AppSpawnReqMsgFree(reqHandle);
            ret = 0;  // 构造失败不判失败, 保持用例环境自适应
            break;
        }

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // daemon 排除: 不置任何冷启动标志、不设置 coldRunPath
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        EXPECT_EQ(property->forkCtx.coldRunPath, nullptr);
        ret = 0;
    } while (0);
    SetNoShareFsEnable(previousNoShareFs);  // 恢复全局开关, 防止影响后续用例
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief wrap 参数未设置 → CheckSupportRaceGuardColdStart 返回 -1 → 不判定冷启动
 *        覆盖: CheckSupportRaceGuardColdStart != 0 → return 0
 *        说明: 主机/设备均可验证（GetParameter 取不到值即 ret<=0 走该分支）
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentWrapNotSet01, TestSize.Level0)
{
    // 防御性清理，确保本用例 wrap 处于未配置状态
    ClearRaceGuardWrapParam(TEST_BUNDLE_NAME);
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // wrap 未配置：不判定冷启动、不设置 coldRunPath
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ(property->forkCtx.coldRunPath, nullptr);
        ret = 0;
    } while (0);
    ClearRaceGuardWrapParam(TEST_BUNDLE_NAME);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 已有冷启动先行（asan 在同父阶段 HOOK_PRIO_COMMON 更早置 APP_COLD_START）→ 互斥避让
 *        覆盖: property->client.flags & APP_COLD_START → return 0（不覆盖、不加 RACEGUARD 标记）
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentAlreadyColdSkip01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        // 预置 asan 已在同父阶段触发冷启动
        property->client.flags |= APP_COLD_START;

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // 互斥避让: APP_COLD_START 保持、不加 RACEGUARD 标记、不动 coldRunPath
        EXPECT_NE((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        EXPECT_EQ(property->forkCtx.coldRunPath, nullptr);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief wrap 参数值非 "raceguard_wrapper" → strcmp 不等 → 不判定冷启动
 *        覆盖: CheckSupportRaceGuardColdStart 中 APPSPAWN_CHECK_LOGV(!strcmp(...)) 失败分支
 *        说明: 环境无法设置参数时退化为 ret<=0 分支, 两者最终都走"不判定", 断言一致。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentWrapWrongValue01, TestSize.Level0)
{
    // 写入错误值（绕过 helper，helper 仅写正确值）
    SetParameter("wrap." TEST_BUNDLE_NAME, "not_raceguard_value");
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // 值不匹配：不判定冷启动
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        ret = 0;
    } while (0);
    ClearRaceGuardWrapParam(TEST_BUNDLE_NAME);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 父阶段判定成功: wrap 配置 + so 存在 → 置 APP_COLD_START|APP_RACEGUARD_DETECTOR
 *        + coldRunPath, 且父阶段不动 LD_PRELOAD（env 仅由子阶段 hook 设置）
 *        覆盖: 父 hook 正向路径（决策: 10s 超时档 + prefork 旁路 + 子阶段标记）
 *        前置条件: wrap 可配置 + so 存在; 二者不满足则跳过。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentDecide01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        SetRaceGuardWrapParam(TEST_BUNDLE_NAME);
        if (!IsRaceGuardWrapOk() || !IsRaceGuardSoPresent() || !IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "wrap/so/entry not ready, skip parent-decide test (device verify pending)";
            ret = 0;
            break;
        }

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // 父阶段决策: 两个标志置位 + coldRunPath 为 raceguard 冷启动入口
        EXPECT_NE((property->client.flags & APP_COLD_START), 0);
        EXPECT_NE((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        ASSERT_NE(property->forkCtx.coldRunPath, nullptr);
        EXPECT_STREQ(property->forkCtx.coldRunPath, TEST_RACEGUARD_COLD_RUN_PATH);
        // 父阶段不做 setenv（env 修改只属于子阶段 hook）
        const char *ld = getenv("LD_PRELOAD");
        EXPECT_TRUE(ld == nullptr || strstr(ld, "raceguard") == nullptr);
        ret = 0;
    } while (0);
    ClearRaceGuardWrapParam(TEST_BUNDLE_NAME);
    FreeColdRunPath(property);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 防御性 free 分支: 预置非空 coldRunPath → 父阶段先 free 再 strdup 替换
 *        覆盖: SetRaceGuardSpawningFlag 中 coldRunPath != NULL 分支(free + NULL + strdup)
 *        说明: 当前仓内并无可达的"有残留路径但无 APP_COLD_START"写入点（asan 是唯一
 *              其它生产者且同时置标志, 本 hook 已提前让位）, 该分支仅为内存安全防御,
 *              防止未来新写入点只写路径不置标志时覆盖泄漏（与 asan_detector.c 同款防御）。
 *        前置条件: wrap 可配置 + so/入口可用; 不满足则跳过。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentReplaceResidualColdRun01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        SetRaceGuardWrapParam(TEST_BUNDLE_NAME);
        if (!IsRaceGuardWrapOk() || !IsRaceGuardSoPresent() || !IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "wrap/so/entry not ready, skip replace-coldrun test (device verify pending)";
            ret = 0;
            break;
        }

        // 预置非空 coldRunPath, 验证父 hook 防御性 branch (先 free 再 strdup)
        property->forkCtx.coldRunPath = strdup("/dummy/coldrun");
        if (property->forkCtx.coldRunPath == nullptr) {
            APPSPAWN_LOGE("strdup coldRunPath fail");
            ret = -1;
            break;
        }

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // 预置值被释放、新值为 RACEGUARD_COLD_RUN_PATH, 标志置位
        ASSERT_NE(property->forkCtx.coldRunPath, nullptr);
        EXPECT_STREQ(property->forkCtx.coldRunPath, TEST_RACEGUARD_COLD_RUN_PATH);
        EXPECT_NE((property->client.flags & APP_COLD_START), 0);
        EXPECT_NE((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        ret = 0;
    } while (0);
    ClearRaceGuardWrapParam(TEST_BUNDLE_NAME);
    FreeColdRunPath(property);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief bundle 名过长 → CheckSupportRaceGuardColdStart 中 sprintf_s 截断 → 返回 -1 → 不判定
 *        覆盖: CheckSupportRaceGuardColdStart 中 sprintf_s(len<=0 || len>=96) 失败分支
 *        说明: 100 字符名 + "wrap." 前缀 = 105 > RACEGUARD_WRAP_VALUE_MAX_LENGTH(96) 必然截断。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentWrapNameTooLongSkip01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        // 100 字符 bundle 名: "wrap." + 100 = 105 > 96 → sprintf_s 截断
        helper.SetProcessName(std::string(100, 'a'));
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // 名过长截断: 不判定冷启动
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 冷启动入口缺失/不可执行 → 父阶段降级, 不判定冷启动（入口 X_OK 检查分支）
 *        覆盖: access(RACEGUARD_COLD_RUN_PATH, F_OK/X_OK) != 0 → return 0
 *        场景: 入口缺失时若仍判定冷启动, 子进程 execv 必然失败且不回通知,
 *              父进程只能等 watcher 超时——因此缺失即降级为普通孵化。
 *        环境自适应: 仅当 wrap 可配置 + so 存在而入口不可用时才可验证该降级分支;
 *        完整设备上入口必然存在, 此时跳过（该分支由主机侧覆盖）。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentEntryMissingDegrade01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        SetRaceGuardWrapParam(TEST_BUNDLE_NAME);
        if (!IsRaceGuardWrapOk() || !IsRaceGuardSoPresent() || IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "entry ready or wrap/so not ready, skip entry-missing degrade test";
            ret = 0;
            break;
        }

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // 入口不可用: 不判定冷启动、不设置 coldRunPath
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        EXPECT_EQ(property->forkCtx.coldRunPath, nullptr);
        ret = 0;
    } while (0);
    ClearRaceGuardWrapParam(TEST_BUNDLE_NAME);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief raceguard so 缺失 → 父阶段降级, 不判定冷启动（so F_OK 检查分支）
 *        覆盖: access(RACEGUARD_LD_PRELOAD, F_OK) != 0 → return 0
 *        说明: so 缺失时注入无从谈起, 保持普通孵化（so 由 mmap 加载, 只查存在性）。
 *        环境自适应: 仅当 wrap 可配置而 so 缺失时才可验证; so 存在（天网/完整设备）时跳过。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardParentSoMissingDegrade01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        SetRaceGuardWrapParam(TEST_BUNDLE_NAME);
        if (!IsRaceGuardWrapOk() || IsRaceGuardSoPresent()) {
            GTEST_LOG_(INFO) << "so present or wrap not ready, skip so-missing degrade test";
            ret = 0;
            break;
        }

        ret = SetRaceGuardSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        // so 缺失: 不判定冷启动、不设置 coldRunPath
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        EXPECT_EQ(property->forkCtx.coldRunPath, nullptr);
        ret = 0;
    } while (0);
    ClearRaceGuardWrapParam(TEST_BUNDLE_NAME);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/* =========================================================================
 *        SetRaceGuardPreloadEnv 用例（子进程 STAGE_CHILD_PRE_COLDBOOT, 仅 setenv）
 * ========================================================================= */

/**
 * @brief 空指针入参 → 返回 0 不崩溃（参数守卫分支）
 *        覆盖: APPSPAWN_CHECK(content != NULL && property != NULL, return 0)
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildNullPointer01, TestSize.Level0)
{
    EXPECT_EQ(SetRaceGuardPreloadEnv(NULL, NULL), 0);
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    EXPECT_EQ(SetRaceGuardPreloadEnv(mgr, NULL), 0);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @brief 非标准模式（nweb）defense-in-depth → 不注入（模式守卫分支）
 *        覆盖: !IsAppSpawnMode(content) → return 0
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildModeGuard01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        const char *ld = getenv("LD_PRELOAD");
        EXPECT_TRUE(ld == nullptr || strstr(ld, "raceguard") == nullptr);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 无 APP_RACEGUARD_DETECTOR 标记（父阶段未判定本请求）→ 不注入
 *        覆盖: !(client.flags & APP_RACEGUARD_DETECTOR) → return 0（默认路径）
 *        说明: 子 hook 不再依赖 wrap/APP_COLD_START 判定, 仅按父阶段标记注入;
 *              预置 LD_PRELOAD 验证不被改动。主机/设备均可验证。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildNoMarkerSkip01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        // 未置 APP_RACEGUARD_DETECTOR, 预置 LD_PRELOAD 验证不被改动
        setenv("LD_PRELOAD", "/system/lib64/libfoo.z.so", 1);

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        const char *ld = getenv("LD_PRELOAD");
        EXPECT_NE(ld, nullptr);
        EXPECT_STREQ(ld, "/system/lib64/libfoo.z.so");
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 子阶段标准注入: 父阶段标记已置 + so 存在 + 空 LD_PRELOAD
 *        覆盖: Build==0(空值分支) + setenv 成功; 子 hook 不改 flags/coldRunPath
 *        前置条件: so 存在; 不满足则跳过。（子 hook 不依赖 wrap, 标记由用例直接预置）
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildInject01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        if (!IsRaceGuardSoPresent() || !IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "so/entry not ready, skip child-inject test (device verify pending)";
            ret = 0;
            break;
        }

        // 模拟父阶段已判定: 标记 + APP_COLD_START 已由 fork 继承
        property->client.flags |= (APP_COLD_START | APP_RACEGUARD_DETECTOR);

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        // 注入成功: LD_PRELOAD 含 raceguard 路径; 子 hook 不改 flags、不动 coldRunPath
        const char *ld = getenv("LD_PRELOAD");
        ASSERT_NE(ld, nullptr);
        EXPECT_STREQ(ld, TEST_RACEGUARD_LD_PRELOAD);
        EXPECT_NE((property->client.flags & APP_COLD_START), 0);
        EXPECT_NE((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        EXPECT_EQ(property->forkCtx.coldRunPath, nullptr);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 子阶段已有其他 so → 冒号分隔追加
 *        覆盖: Build==0(已有值追加分支), LD_PRELOAD 原值在前、raceguard 在后
 *        前置条件: so 存在
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildAppend01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        if (!IsRaceGuardSoPresent() || !IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "so/entry not ready, skip child-append test (device verify pending)";
            ret = 0;
            break;
        }

        property->client.flags |= APP_RACEGUARD_DETECTOR;
        // 预置已有其他 so
        setenv("LD_PRELOAD", "/system/lib64/libfoo.z.so", 1);

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        const char *ld = getenv("LD_PRELOAD");
        ASSERT_NE(ld, nullptr);
        // 冒号分隔, 原值在前、raceguard 在后
        EXPECT_TRUE(strstr(ld, "/system/lib64/libfoo.z.so:") != nullptr);
        EXPECT_TRUE(strstr(ld, TEST_RACEGUARD_LD_PRELOAD) != nullptr);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 子阶段 LD_PRELOAD 已含 raceguard so → 幂等不重复追加
 *        覆盖: Build 中 strstr 命中 → 返回 -1 → hook 不 setenv
 *        前置条件: so 存在
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildIdempotent01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        if (!IsRaceGuardSoPresent() || !IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "so/entry not ready, skip child-idempotent test (device verify pending)";
            ret = 0;
            break;
        }

        property->client.flags |= APP_RACEGUARD_DETECTOR;
        // 预置 LD_PRELOAD 已含完整 raceguard so 路径
        setenv("LD_PRELOAD", TEST_RACEGUARD_LD_PRELOAD, 1);

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        // 幂等命中: LD_PRELOAD 保持原值
        const char *ld = getenv("LD_PRELOAD");
        ASSERT_NE(ld, nullptr);
        EXPECT_STREQ(ld, TEST_RACEGUARD_LD_PRELOAD);
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 子阶段已有值过长 → 追加截断 → Build 返回 -1 → 不注入
 *        覆盖: hook 层 Build 截断失败路径 (len<=0 || len>=bufLen → return -1 → return 0)
 *        前置条件: so 存在
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildTruncate01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        if (!IsRaceGuardSoPresent() || !IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "so/entry not ready, skip child-truncate test (device verify pending)";
            ret = 0;
            break;
        }

        property->client.flags |= APP_RACEGUARD_DETECTOR;
        // 构造 > 512B 且不含 raceguard 的 LD_PRELOAD, 触发追加截断
        char longValue[600] = {0};
        (void)memset_s(longValue, sizeof(longValue), '/', 590);
        longValue[0] = '/';
        longValue[589] = '\0';
        setenv("LD_PRELOAD", longValue, 1);

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        // 截断拒绝: LD_PRELOAD 保持原值
        const char *ld = getenv("LD_PRELOAD");
        ASSERT_NE(ld, nullptr);
        EXPECT_EQ(strlen(ld), 589);  // 589 = 原值长度, 未被改写
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 子阶段冷启动入口消失（父阶段判定后、execv 前入口被动过）→ 降级回普通孵化
 *        覆盖: access(RACEGUARD_COLD_RUN_PATH, F_OK/X_OK) != 0 → 清 APP_COLD_START/
 *              APP_RACEGUARD_DETECTOR + free coldRunPath + 不注入
 *        场景: 入口缺失时 execv 必然失败且不回通知, 清标志使 AppSpawnChild 落回
 *              普通孵化路径, 结果仍能回报父进程, 避免挂死到 watcher 超时。
 *        环境自适应: 仅当 so 存在而入口不可用时才可验证; 完整设备上入口必然存在, 跳过。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildEntryMissingDegrade01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        if (!IsRaceGuardSoPresent() || IsRaceGuardEntryReady()) {
            GTEST_LOG_(INFO) << "entry ready or so not ready, skip child entry-missing degrade test";
            ret = 0;
            break;
        }

        // 模拟父阶段已判定 + fork 继承的完整状态: 双标志 + coldRunPath
        property->client.flags |= (APP_COLD_START | APP_RACEGUARD_DETECTOR);
        property->forkCtx.coldRunPath = strdup(TEST_RACEGUARD_COLD_RUN_PATH);
        APPSPAWN_CHECK(property->forkCtx.coldRunPath != nullptr, ret = -1; break, "strdup coldRunPath fail");
        // 预置 LD_PRELOAD 验证降级时不被改动
        setenv("LD_PRELOAD", "/system/lib64/libfoo.z.so", 1);

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        // 入口消失降级: 清双标志、释放 coldRunPath、不注入
        EXPECT_EQ((property->client.flags & APP_COLD_START), 0);
        EXPECT_EQ((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        EXPECT_EQ(property->forkCtx.coldRunPath, nullptr);
        const char *ld = getenv("LD_PRELOAD");
        ASSERT_NE(ld, nullptr);
        EXPECT_STREQ(ld, "/system/lib64/libfoo.z.so");
        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @brief 子阶段 so 消失（父阶段判定后 so 被动过）→ 跳过注入, 冷启动继续
 *        覆盖: access(RACEGUARD_LD_PRELOAD, F_OK) != 0 → return 0（不清标志, 不动 coldRunPath）
 *        说明: 与入口缺失不同, so 缺失仅意味着"不带 raceguard 运行时冷启动",
 *              APP_COLD_START/APP_RACEGUARD_DETECTOR/coldRunPath 全部保持, 冷启动照常进行。
 *        环境自适应: 仅当 so 缺失时（主机）可验证; so 存在（天网/完整设备）时跳过。
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardChildSoMissingSkip01, TestSize.Level0)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client %{public}s", APPSPAWN_SERVER_NAME);

        RaceGuardTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.RaceGuardTestCreateMsg(clientHandle, MSG_APP_SPAWN);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, ret = -1; break, "Failed to create msg");

        property = helper.RaceGuardTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, ret = -1; break, "Failed to get app property");

        if (IsRaceGuardSoPresent()) {
            GTEST_LOG_(INFO) << "so present, skip child so-missing test (device verify pending)";
            ret = 0;
            break;
        }

        // 模拟父阶段已判定 + fork 继承的完整状态
        property->client.flags |= (APP_COLD_START | APP_RACEGUARD_DETECTOR);
        property->forkCtx.coldRunPath = strdup(TEST_RACEGUARD_COLD_RUN_PATH);
        APPSPAWN_CHECK(property->forkCtx.coldRunPath != nullptr, ret = -1; break, "strdup coldRunPath fail");
        // 预置 LD_PRELOAD 验证跳过注入时不被改动
        setenv("LD_PRELOAD", "/system/lib64/libfoo.z.so", 1);

        ret = SetRaceGuardPreloadEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        // so 缺失仅跳过注入: 标志/coldRunPath 全部保持, LD_PRELOAD 不动
        EXPECT_NE((property->client.flags & APP_COLD_START), 0);
        EXPECT_NE((property->client.flags & APP_RACEGUARD_DETECTOR), 0);
        ASSERT_NE(property->forkCtx.coldRunPath, nullptr);
        EXPECT_STREQ(property->forkCtx.coldRunPath, TEST_RACEGUARD_COLD_RUN_PATH);
        const char *ld = getenv("LD_PRELOAD");
        ASSERT_NE(ld, nullptr);
        EXPECT_STREQ(ld, "/system/lib64/libfoo.z.so");
        ret = 0;
    } while (0);
    FreeColdRunPath(property);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/* =========================================================================
 *                  RaceGuardPreloadValue 用例（直接调用, 不依赖 so）
 * ========================================================================= */

/**
 * @brief 入参非法 → 返回 RACEGUARD_ERR_FAILED（参数守卫分支）
 *        覆盖: buf==NULL 或 bufLen==0 → return RACEGUARD_ERR_FAILED(-1)
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardPreloadValueNullBuf01, TestSize.Level0)
{
    EXPECT_EQ(BuildRaceGuardPreloadValue(NULL, 512), -1);
    char buf[8] = {0};
    EXPECT_EQ(BuildRaceGuardPreloadValue(buf, 0), -1);
}

/**
 * @brief 空 LD_PRELOAD → 输出为常量路径（空值分支）
 *        覆盖: getenv==NULL||'\0' → sprintf 常量, return 0
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardPreloadValueEmpty01, TestSize.Level0)
{
    char buf[512] = {0};
    unsetenv("LD_PRELOAD");
    int ret = BuildRaceGuardPreloadValue(buf, sizeof(buf));
    EXPECT_EQ(ret, 0);
    EXPECT_STREQ(buf, TEST_RACEGUARD_LD_PRELOAD);
    // 单值, 无冒号分隔符
    EXPECT_TRUE(strchr(buf, ':') == nullptr);
}

/**
 * @brief 已有值 → 冒号追加（追加成功分支）
 *        覆盖: getenv 非空且不含 raceguard → sprintf "%s:%s", return 0
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardPreloadValueAppend01, TestSize.Level0)
{
    char buf[512] = {0};
    setenv("LD_PRELOAD", "/system/lib64/libfoo.z.so", 1);
    int ret = BuildRaceGuardPreloadValue(buf, sizeof(buf));
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(strstr(buf, "/system/lib64/libfoo.z.so:") != nullptr);
    EXPECT_TRUE(strstr(buf, TEST_RACEGUARD_LD_PRELOAD) != nullptr);
}

/**
 * @brief LD_PRELOAD 已含 raceguard so → 幂等返回 RACEGUARD_ERR_FAILED（幂等分支）
 *        覆盖: strstr(cur, RACEGUARD_LD_PRELOAD) != NULL → return RACEGUARD_ERR_FAILED(-1)
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardPreloadValueIdempotent01, TestSize.Level0)
{
    char buf[512] = {0};
    setenv("LD_PRELOAD", TEST_RACEGUARD_LD_PRELOAD, 1);
    int ret = BuildRaceGuardPreloadValue(buf, sizeof(buf));
    EXPECT_EQ(ret, -1);
}

/**
 * @brief 已有值过长 → 追加截断 → 返回 RACEGUARD_ERR_FAILED（截断分支）
 *        覆盖: sprintf_s 截断返回 <=0 或 >= bufLen → return RACEGUARD_ERR_FAILED(-1)
 */
HWTEST_F(AppSpawnRaceGuardTest, RaceGuardPreloadValueTruncate01, TestSize.Level0)
{
    // 构造 > 512B 且不含 raceguard 的 LD_PRELOAD, 触发追加截断
    char longValue[600] = {0};
    (void)memset_s(longValue, sizeof(longValue), '/', 590);
    longValue[0] = '/';
    longValue[589] = '\0';
    setenv("LD_PRELOAD", longValue, 1);

    char buf[512] = {0};
    int ret = BuildRaceGuardPreloadValue(buf, sizeof(buf));
    EXPECT_EQ(ret, -1);
}

}  // namespace OHOS
