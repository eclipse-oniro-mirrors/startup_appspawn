/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include "appspawn_adapter.h"
#include "appspawn_hook.h"
#include "appspawn_msg.h"
#include "appspawn_manager.h"
#include "appspawn_service.h"
#include "appspawn_utils.h"
#include "app_spawn_test_helper.h"
#include "appspawn_client.h"
#include "spawn_policy.h"
#include "spawn_primitives.h"

using namespace testing;
using namespace testing::ext;
using namespace OHOS;

static int g_notifyResToParentCount = 0;
void NotifyResToParent(AppSpawnContent *content, AppSpawnClient *client, int result)
{
    (void)content;
    (void)client;
    (void)result;
    g_notifyResToParentCount++;
}

namespace {
static AppSpawnTestHelper g_testHelper;
static const char *DAEMON_BIN_LIBS_DIR = "/data/storage/el1/bundle/libs";
#if defined(__aarch64__)
static const char *DAEMON_BIN_ABI = "arm64";
#else
static const char *DAEMON_BIN_ABI = "arm";
#endif
static const char *VALID_BIN_NAME = "daemon_ut_bin";

static const char *INVALID_RELATIVE_PATH = "relative/path";
static const char *NON_WHITELISTED_PATH = "/system/bin/appspawn";
static const char *TEST_EXT_NAME_OWNER = "owner";

static const char *DAEMON_REAL_BUNDLE_ROOT = "/data/app/el1/bundle/public";
static const char *TEST_BUNDLE_NAME = "com.test.daemon";

static const uint32_t TEST_DAC_ID = 20010029;   // 合法的测试 app uid/gid 占位值

static const mode_t TEST_DIR_MODE = 0750;      // 测试目录权限：属主 rwx、属组 r-x
static const mode_t TEST_BIN_MODE = 0750;      // daemon bin 权限：需含 x 位以通过白名单校验

static std::string g_libsDir;             // /data/storage/el1/bundle/libs/<abi>
static std::string g_validBinPath;        // 沙箱视图下的 daemon bin（子进程门禁校验）
static std::string g_realValidBinPath;    // 真实安装路径下的同一文件（父进程校验）
static std::string g_dotdotPath;          // 同目录下、含 ".." 的越权路径
static bool g_validBinReady = false;      // 满足白名单（root 属主 / 普通文件 / x 位）
static bool g_validBinExecable = false;   // 本进程 access(X_OK) 通过（SELinux 也需放行）

static bool MakeDirIfAbsent(const std::string &dir)
{
    struct stat dirStat = {};
    if (stat(dir.c_str(), &dirStat) == 0) {
        return S_ISDIR(dirStat.st_mode);
    }
    return mkdir(dir.c_str(), TEST_DIR_MODE) == 0 || errno == EEXIST;
}

static bool PrepareValidDaemonBin()
{
    g_libsDir = std::string(DAEMON_BIN_LIBS_DIR) + "/" + DAEMON_BIN_ABI;
    g_validBinPath = g_libsDir + "/" + VALID_BIN_NAME;
    // 前缀检查会先放行 ".../libs/<abi>/"，再由 '..' 检查拒绝，用于覆盖越权分支
    g_dotdotPath = g_libsDir + "/../evil_daemon";
    g_realValidBinPath = std::string(DAEMON_REAL_BUNDLE_ROOT) + "/" + TEST_BUNDLE_NAME +
        "/libs/" + DAEMON_BIN_ABI + "/" + VALID_BIN_NAME;

    if (!MakeDirIfAbsent(DAEMON_BIN_LIBS_DIR) || !MakeDirIfAbsent(g_libsDir)) {
        GTEST_LOG_(WARNING) << "daemon bin dir " << g_libsDir << " not ready, errno " << errno;
        return false;
    }
    int fd = open(g_validBinPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, TEST_BIN_MODE);
    if (fd < 0) {
        GTEST_LOG_(WARNING) << "create " << g_validBinPath << " failed, errno " << errno;
        return false;
    }
    const char content[] = "#!/bin/sh\nexit 0\n";
    ssize_t writeLen = write(fd, content, sizeof(content) - 1);
    (void)close(fd);
    if (writeLen != static_cast<ssize_t>(sizeof(content) - 1) || chmod(g_validBinPath.c_str(), TEST_BIN_MODE) != 0) {
        GTEST_LOG_(WARNING) << "prepare " << g_validBinPath << " failed, errno " << errno;
        return false;
    }
    // 白名单要求 root 属主 + 至少一个 x 位；非 root 环境无法满足，相关用例跳过
    struct stat fileStat = {};
    if (lstat(g_validBinPath.c_str(), &fileStat) != 0 || !S_ISREG(fileStat.st_mode) ||
        fileStat.st_uid != 0 || (fileStat.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        GTEST_LOG_(WARNING) << "daemon bin " << g_validBinPath << " not whitelist-compliant";
        return false;
    }
    g_validBinExecable = access(g_validBinPath.c_str(), X_OK) == 0;

    // 父进程校验用的真实安装路径：逐级补目录后与沙箱路径共享同一 inode
    const std::string bundleDir = std::string(DAEMON_REAL_BUNDLE_ROOT) + "/" + TEST_BUNDLE_NAME;
    const std::string realLibsDir = bundleDir + "/libs/" + DAEMON_BIN_ABI;
    if (!MakeDirIfAbsent(DAEMON_REAL_BUNDLE_ROOT) || !MakeDirIfAbsent(bundleDir) ||
        !MakeDirIfAbsent(bundleDir + "/libs") || !MakeDirIfAbsent(realLibsDir)) {
        GTEST_LOG_(WARNING) << "real daemon bin dir " << realLibsDir << " not ready, errno " << errno;
        return false;
    }
    if (link(g_validBinPath.c_str(), g_realValidBinPath.c_str()) != 0 && errno != EEXIST) {
        GTEST_LOG_(WARNING) << "link " << g_realValidBinPath << " failed, errno " << errno;
        return false;
    }
    return true;
}

static void RemoveValidDaemonBin()
{
    if (!g_validBinPath.empty()) {
        (void)unlink(g_validBinPath.c_str());
    }
    if (!g_realValidBinPath.empty()) {
        (void)unlink(g_realValidBinPath.c_str());
    }
}

static bool AddDaemonStandardTlvs(AppSpawnReqMsgHandle reqHandle, const char *bundleName,
    uint32_t gidCount = 1, bool withDaemonBinFlag = true)
{
    APPSPAWN_CHECK(reqHandle != 0 && bundleName != nullptr, return false, "Invalid param for daemon tlvs");
    AppDacInfo dacInfo = {};
    // daemon 的 DAC 值当前不被改写（仍以 appspawn 自身身份运行），这里只需填合法 app uid 占位，
    // SetAppUidGid 对 uid/gid 有 >= MIN_VALID_APP_UID 的校验，填 0 会在该步被判非法。
    dacInfo.uid = TEST_DAC_ID;
    dacInfo.gid = TEST_DAC_ID;
    // gidCount 传 0 用于构造"空补充组表"的非法报文
    dacInfo.gidCount = gidCount;
    if (gidCount > 0) {
        dacInfo.gidTable[0] = TEST_DAC_ID;
    }
    APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetBundleInfo(reqHandle, 100, bundleName) == 0, return false);
    // daemon 与 app 一致，token 类型必须是 TOKEN_HAP（已确认）：AccessTokenIDInner 位布局为
    // uniqueID[0:19] | type[27:28] | version[29:31]，0x20003039 = version1 + HAP + uniqueID 12345。
    APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppAccessToken(reqHandle, 0x20003039) == 0, return false);
    APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppDomainInfo(reqHandle, 1, "system_core") == 0, return false);
    APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppDacInfo(reqHandle, &dacInfo) == 0, return false);
    if (withDaemonBinFlag) {
        APPSPAWN_CHECK_ONLY_EXPER(AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_DAEMON_BIN) == 0,
            return false);
    }
    return true;
}
}  // namespace

class AppSpawnDaemonSpawnTest : public testing::Test {
public:
    static void SetUpTestCase()
    {
        previousNoShareFs_ = IsNoShareFsEnable();
        SetNoShareFsEnable(true);
        g_validBinReady = PrepareValidDaemonBin();
    };
    static void TearDownTestCase()
    {
        RemoveValidDaemonBin();
        SetNoShareFsEnable(previousNoShareFs_);
    };
    void SetUp()
    {
        g_testHelper.SetProcessName("com.test.daemon");
    }
    void TearDown() {};

private:
    static inline bool previousNoShareFs_ = false;
};

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_MissingBinPath_001
 * @tc.desc: AC-1.3 - daemon request missing daemon_bin_path ext → APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_MissingBinPath_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");
        APPSPAWN_CHECK(property->message != nullptr, break, "message is null");

        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_InvalidBinPath_001
 * @tc.desc: AC-1.4 - daemon_bin_path not absolute → APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_InvalidBinPath_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(INVALID_RELATIVE_PATH), strlen(INVALID_RELATIVE_PATH));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add ext info");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_InvalidBinPath_002
 * @tc.desc: AC-1.4 - daemon_bin_path contains '..' → 通用报文层放行（'..' 由子进程 PRE_RELY 门禁拒绝）
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_InvalidBinPath_002, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_dotdotPath.c_str()), strlen(g_dotdotPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add ext info");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, 0);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_InvalidBinPath_003
 * @tc.desc: AC-1.4 - daemon_bin_path too long (>= PATH_MAX) → 父进程 APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_InvalidBinPath_003, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        char tooLongPath[PATH_MAX + 10];
        memset(tooLongPath, '/', PATH_MAX + 5);
        tooLongPath[0] = '/';
        tooLongPath[PATH_MAX + 5] = '\0';

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(tooLongPath), strlen(tooLongPath));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add ext info");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        // 父进程做字符串级校验：绝对路径 + 白名单根/本机 ABI
        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_ValidBinPath_001
 * @tc.desc: AC-1.1 - daemon request with valid daemon_bin_path → CheckAppSpawnMsg returns 0
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_ResolvePolicy_App_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        reqHandle = g_testHelper.CreateMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        const SpawnPolicy *pl = ResolvePolicy(property);
        APPSPAWN_CHECK(pl != nullptr, break, "ResolvePolicy returned NULL");
        EXPECT_STREQ(pl->name, "app");
        EXPECT_EQ(pl->uidFrom, POLICY_SRC_TLV_DAC);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_ResolvePolicy_DaemonOwned_001
 * @tc.desc: AC-1.1 - ResolvePolicy for MSG_SPAWN_SERVICE_BIN with owner → "daemon-owned" policy
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_ResolvePolicy_DaemonOwned_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");

        const char *owner = "test_owner";
        ret = AppSpawnReqMsgAddExtInfo(reqHandle, TEST_EXT_NAME_OWNER,
            reinterpret_cast<const uint8_t *>(owner), strlen(owner));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add owner");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        const SpawnPolicy *pl = ResolvePolicy(property);
        APPSPAWN_CHECK(pl != nullptr, break, "ResolvePolicy returned NULL");
        EXPECT_STREQ(pl->name, "daemon-owned");
        EXPECT_EQ(pl->uidFrom, POLICY_SRC_FIXED);
        EXPECT_EQ(pl->uid, 0);
        EXPECT_EQ(pl->gid, 0);
        EXPECT_EQ(pl->capsFrom, POLICY_SRC_SUDO);
        EXPECT_EQ(pl->execFrom, POLICY_SRC_EXT_BIN_PATH);
        EXPECT_EQ(pl->processNameFrom, POLICY_SRC_EXT_BIN_PATH);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_ResolvePolicy_DaemonSystem_001
 * @tc.desc: AC-1.2 - ResolvePolicy for MSG_SPAWN_SERVICE_BIN without owner → daemon profile（单一 daemon 策略）
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_ResolvePolicy_DaemonSystem_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        // 无 owner ext → 仍走 daemon profile（当前 daemon 策略只有一个）
        const SpawnPolicy *pl = ResolvePolicy(property);
        APPSPAWN_CHECK(pl != nullptr, break, "ResolvePolicy returned NULL");
        EXPECT_STREQ(pl->name, "daemon-owned");
        EXPECT_EQ(pl->uidFrom, POLICY_SRC_FIXED);
        EXPECT_EQ(pl->gidFrom, POLICY_SRC_FIXED);
        EXPECT_EQ(pl->uid, 0);
        EXPECT_EQ(pl->gid, 0);
        EXPECT_EQ(pl->capsFrom, POLICY_SRC_SUDO);
        EXPECT_EQ(pl->selinuxFrom, POLICY_SRC_SELINUX_SUDO);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_ResolvePolicy_NullCtx_001
 * @tc.desc: ResolvePolicy 对 NULL ctx 回退 app 策略，永不为 NULL
 * @tc.type: FUNC
 * @tc.level: Level1
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_ResolvePolicy_NullCtx_001, TestSize.Level0)
{
    const SpawnPolicy *pl = ResolvePolicy(nullptr);
    EXPECT_NE(pl, nullptr);
    EXPECT_STREQ(pl->name, "app");
    EXPECT_EQ(pl->uidFrom, POLICY_SRC_TLV_DAC);
}

/**
 * @tc.name: App_Spawn_Daemon_SetProcessName_FromMsg_001
 * @tc.desc: daemon 进程名直接取报文头 processName（服务端不再按 daemon_bin_path 派生）
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_SetProcessName_FromMsg_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    char longName[128] = {0};
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");
        mgr->content.mode = MODE_FOR_APP_SPAWN;  // 隔离其它用例遗留的全局 mgr 模式
        mgr->content.longProcName = longName;
        mgr->content.longProcNameLen = sizeof(longName);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        // 调用方在创建报文时给定进程名，服务端不再覆盖
        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, "test_daemon", &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        SetPrctlResult(0);  // prctl stub 默认返回 -1，会让 SetProcessName 直接返回 errno
        ret = SetProcessName(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_STREQ(longName, "test_daemon");

        ret = 0;
    } while (0);
    SetPrctlResult(-1);  // 恢复 stub 默认值，避免影响其它用例
    mgr->content.longProcName = nullptr;
    mgr->content.longProcNameLen = 0;
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_NotInWhitelist_001
 * @tc.desc: F-1 - daemon_bin_path outside whitelist → 父进程 APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_NotInWhitelist_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(NON_WHITELISTED_PATH), strlen(NON_WHITELISTED_PATH));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add ext info");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        // 父进程做字符串级校验：绝对路径 + 白名单根/本机 ABI
        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_MsgFlagsTlv_001
 * @tc.desc: daemon 与 app 报文同构：同样携带 TLV_MSG_FLAGS（默认全 0），差异只在 ext TLV
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_MsgFlagsTlv_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        EXPECT_NE(property->message->tlvOffset[TLV_MSG_FLAGS], INVALID_OFFSET);
        AppSpawnMsgFlags *msgFlags = (AppSpawnMsgFlags *)GetAppSpawnMsgInfo(property->message, TLV_MSG_FLAGS);
        ASSERT_NE(msgFlags, nullptr);
        const uint32_t daemonFlagIndex = APP_FLAGS_DAEMON_BIN / 32;
        const uint32_t daemonFlagMask = 1u << (APP_FLAGS_DAEMON_BIN % 32);
        for (uint32_t index = 0; index < msgFlags->count; index++) {
            uint32_t expected = (index == daemonFlagIndex) ? daemonFlagMask : 0u;
            EXPECT_EQ(msgFlags->flags[index], expected);
        }
        EXPECT_NE(CheckAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_DAEMON_BIN), 0);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_ValidBinPath_001
 * @tc.desc: AC-1.1 - whitelisted root-owned bin without app flags → CheckAppSpawnMsg returns 0
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_ValidBinPath_001, TestSize.Level0)
{
    if (!g_validBinReady) {
        GTEST_SKIP() << g_validBinPath << " not prepared, skip whitelist path check";
    }
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");
        mgr->content.mode = MODE_FOR_APP_SPAWN;

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add ext info");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        int checkRet = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(checkRet, 0);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_EmptyGidTable_001
 * @tc.desc: daemon 报文 gidCount == 0（空补充组表）→ APPSPAWN_MSG_INVALID：
 *           子进程 setgroups 用报文里的表，空表等价于清空 appspawn 的补充组，必须在入口拒绝
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_EmptyGidTable_001, TestSize.Level0)
{
    if (!g_validBinReady) {
        GTEST_SKIP() << g_validBinPath << " not prepared, skip empty gid table check";
    }
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");
        mgr->content.mode = MODE_FOR_APP_SPAWN;

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName(), 0),
            break, "Failed to add standard tlvs with empty gid table");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add ext info");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        int checkRet = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(checkRet, APPSPAWN_MSG_INVALID);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_SetAppFlagTlv_001
 * @tc.desc: daemon 报文与 app 同构、同样带 TLV_MSG_FLAGS，AppSpawnReqMsgSetAppFlag 对 daemon 报文可正常置位
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_SetAppFlagTlv_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_DEBUGGABLE);
        EXPECT_EQ(ret, 0);

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");
        EXPECT_NE(CheckAppMsgFlagsSet(property, APP_FLAGS_DEBUGGABLE), 0);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CapMaskTest_001
 * @tc.desc: P0-3 - 64 位 capability 掩码位判断不得回绕（CAP_TO_MASK 只取低 5 位）
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CapMaskTest_001, TestSize.Level0)
{
    const uint64_t sudoMask = 0x3fffffffffULL;
    // 0~37 位置位
    EXPECT_TRUE(SpawnCapMaskTest(sudoMask, 0));
    EXPECT_TRUE(SpawnCapMaskTest(sudoMask, 31));
    EXPECT_TRUE(SpawnCapMaskTest(sudoMask, 32));
    EXPECT_TRUE(SpawnCapMaskTest(sudoMask, 37));
    // 38 位及以上未置位（旧实现在这里会因低 5 位回绕误判为置位）
    EXPECT_FALSE(SpawnCapMaskTest(sudoMask, 38));
    EXPECT_FALSE(SpawnCapMaskTest(sudoMask, 40));
    EXPECT_FALSE(SpawnCapMaskTest(sudoMask, 41));
    EXPECT_FALSE(SpawnCapMaskTest(sudoMask, 63));
    // 边界
    EXPECT_TRUE(SpawnCapMaskTest(0x8000000000000000ULL, 63));
    EXPECT_FALSE(SpawnCapMaskTest(0x8000000000000000ULL, 31));
    EXPECT_FALSE(SpawnCapMaskTest(sudoMask, 64));
    EXPECT_FALSE(SpawnCapMaskTest(0, 0));
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_DacTlvAllowed_001
 * @tc.desc: daemon 报文携带 DAC TLV 入口不拒绝、入口也不改写（覆盖由 MSG_DECODE 的服务端 hook 负责）
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_DacTlvAllowed_001, TestSize.Level0)
{
    if (!g_validBinReady) {
        GTEST_SKIP() << g_validBinPath << " not prepared, skip daemon msg check";
    }
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");

        // 混入 app 语义的 DAC TLV：入口不拦截
        APPSPAWN_CHECK(g_testHelper.AddDacInfo(reqHandle) == 0, break, "Failed to add dac info");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, 0);

        // 入口不改写 DAC；当前实现也没有 MSG_DECODE 阶段的 DAC 覆盖，客户端传入的 uid/gid 原样保留
        AppSpawnMsgDacInfo *dacInfo = reinterpret_cast<AppSpawnMsgDacInfo *>(
            GetAppProperty(property, TLV_DAC_INFO));
        APPSPAWN_CHECK(dacInfo != nullptr, break, "no dac info");
        EXPECT_NE(dacInfo->uid, 0u);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_ExtAllowed_001
 * @tc.desc: AC-1.7 - daemon 报文携带未登记 ext（AppEnv）不再入口拒绝
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_ExtAllowed_001, TestSize.Level0)
{
    if (!g_validBinReady) {
        GTEST_SKIP() << g_validBinPath << " not prepared, skip daemon msg check";
    }
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");

        const char *anyExt = "{}";
        ret = AppSpawnReqMsgAddExtInfo(reqHandle, "AppEnv",
            reinterpret_cast<const uint8_t *>(anyExt), strlen(anyExt));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add arbitrary ext");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, 0);  // daemon 入口只校验通道与 bin path，任意 ext 不在入口拦截

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_EmptyProcessName_Rejected_001
 * @tc.desc: 服务端不再派生 daemon 进程名，processName 为空直接拒绝
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_EmptyProcessName_Rejected_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        property->message->msgHeader.processName[0] = '\0';
        ret = CheckAppSpawnMsg(property->message);
        EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);  // 进程名由调用方提供，空名不受理

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_PrepareDaemonExec_RevalidateOnly_001
 * @tc.desc: P0-1 - CheckDaemonExecTarget 与 PrepareDaemonExec 只做校验、都不写结果通道；
 *           成功回写由 AppSpawnServiceBinRunTarget 在 execv 前调用 ClearEnvAndReturnSuccess 完成
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_PrepareDaemonExec_RevalidateOnly_001, TestSize.Level0)
{
    if (!g_validBinReady || !g_validBinExecable) {
        GTEST_SKIP() << g_validBinPath << " not executable in this environment, skip notify check";
    }
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    AppSpawningCtx *property = nullptr;
    int fds[2] = {-1, -1};
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");
        mgr->content.mode = MODE_FOR_APP_SPAWN;  // 隔离其它用例遗留的全局 mgr 模式
        mgr->content.notifyResToParent = NotifyResToParent;

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");
        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");

        property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        APPSPAWN_CHECK(pipe(fds) == 0, break, "Failed to create pipe");
        property->forkCtx.fd[0] = fds[0];
        property->forkCtx.fd[1] = fds[1];

        // PRE_RELY 门禁：路径校验通过，本阶段不写结果通道
        ret = CheckDaemonExecTarget(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_EQ(property->forkCtx.fd[1], fds[1]);

        // exec 前复校验：只校验路径、不写结果通道；成功回写由 AppSpawnServiceBinRunTarget
        // 在 execv 之前调用 ClearEnvAndReturnSuccess 完成（见 ace_adapter.cpp）
        char pathBuf[PATH_MAX + 1] = {0};
        ret = PrepareDaemonExec(mgr, property, pathBuf, sizeof(pathBuf));
        EXPECT_EQ(ret, 0);
        EXPECT_STREQ(pathBuf, g_validBinPath.c_str());
        EXPECT_EQ(property->forkCtx.fd[1], fds[1]);

        ret = 0;
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_PrepareDaemonExec_InvalidPath_001
 * @tc.desc: daemon 报文缺 daemon_bin_path 时 PRE_RELY 门禁与 exec 前复校验都返回 MSG_INVALID，
 *           且 hook 自身不写结果通道（由框架统一回写，避免误报成功）
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_PrepareDaemonExec_InvalidPath_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    AppSpawningCtx *property = nullptr;
    int fds[2] = {-1, -1};
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");
        mgr->content.mode = MODE_FOR_APP_SPAWN;
        mgr->content.notifyResToParent = NotifyResToParent;

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");

        property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        APPSPAWN_CHECK(pipe(fds) == 0, break, "Failed to create pipe");
        property->forkCtx.fd[0] = fds[0];
        property->forkCtx.fd[1] = fds[1];

        // PRE_RELY 门禁：缺 daemon_bin_path → 返回 MSG_INVALID；本 hook 不写结果通道，
        // 由 AppSpawnChild 的统一失败路径 NotifyResToParent(ret) 回写（否则会"先回写再失败"）。
        ret = CheckDaemonExecTarget(mgr, property);
        EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);
        EXPECT_EQ(property->forkCtx.fd[1], fds[1]);

        // exec 前复校验（PrepareDaemonExec）：同样拒绝且不写结果通道
        char pathBuf[PATH_MAX + 1] = {0};
        ret = PrepareDaemonExec(mgr, property, pathBuf, sizeof(pathBuf));
        EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);
        EXPECT_EQ(pathBuf[0], '\0');
        EXPECT_EQ(property->forkCtx.fd[1], fds[1]);

        ret = 0;
    } while (0);
    if (property != nullptr) {
        property->forkCtx.fd[0] = -1;
        property->forkCtx.fd[1] = -1;
    }
    if (fds[0] >= 0) {
        (void)close(fds[0]);
    }
    if (fds[1] >= 0) {
        (void)close(fds[1]);
    }
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_CheckAppSpawnMsg_NonAppspawnMode_001
 * @tc.desc: AC-1.5 - MSG_SPAWN_SERVICE_BIN 仅在 appspawn 通道受理，其它 spawner 返回 APPSPAWN_MSG_INVALID
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_CheckAppSpawnMsg_NonAppspawnMode_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    AppSpawnMgr *mgr = nullptr;
    int ret = -1;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);
        APPSPAWN_CHECK(mgr != nullptr, break, "Failed to create mgr");
        mgr->content.mode = MODE_FOR_NWEB_SPAWN;

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName()),
            break, "Failed to add standard tlvs");
        ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
            reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
        APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        // 即使是合法路径，非 appspawn 通道也必须拒绝
        EXPECT_EQ(CheckAppSpawnMsg(property->message), APPSPAWN_MSG_INVALID);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_MsgTypeOnly_TreatedAsApp_001
 * @tc.desc: 双闸门 - MSG_SPAWN_SERVICE_BIN 未携带 APP_FLAGS_DAEMON_BIN：不构成 daemon 报文，
 *           入口只执行通用（app 语义）检查、不做 daemon 专属检查，且不命中 daemon 策略
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_MsgTypeOnly_TreatedAsApp_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        ret = AppSpawnReqMsgCreate(MSG_SPAWN_SERVICE_BIN, g_testHelper.GetDefaultTestAppBundleName(), &reqHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create daemon msg");
        // 刻意不置 APP_FLAGS_DAEMON_BIN：只满足 msgType 一个闸门
        APPSPAWN_CHECK(AddDaemonStandardTlvs(reqHandle, g_testHelper.GetDefaultTestAppBundleName(), 1, false),
            break, "Failed to add standard tlvs");
        // 携带 daemon_bin_path 也不影响结论：非 daemon 报文时该 ext 是惰性的（消费方均经
        // IsDaemonBinProperty 门禁），能准备就带上以固化这一点
        if (!g_validBinPath.empty()) {
            ret = AppSpawnReqMsgAddExtInfo(reqHandle, MSG_EXT_NAME_DAEMON_BIN_PATH,
                reinterpret_cast<const uint8_t *>(g_validBinPath.c_str()), strlen(g_validBinPath.c_str()));
            APPSPAWN_CHECK(ret == 0, break, "Failed to add bin path");
        }

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        // 缺 flag：双闸门不成立，不是 daemon 报文
        EXPECT_FALSE(IsDaemonBinMsg(property->message));
        // 入口按 app 语义处理：通用检查（标准 TLV/processName/bundleName）通过即可，
        // 不做 daemon 专属检查（模式门禁/gidCount/路径白名单均不适用）
        EXPECT_EQ(CheckAppSpawnMsg(property->message), 0);
        const SpawnPolicy *pl = ResolvePolicy(property);
        APPSPAWN_CHECK(pl != nullptr, break, "ResolvePolicy returned NULL");
        EXPECT_STREQ(pl->name, "app");
        EXPECT_EQ(pl->execFrom, POLICY_SRC_TLV_HAP_ENTRY);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}

/**
 * @tc.name: App_Spawn_Daemon_FlagOnAppMsgType_001
 * @tc.desc: 双闸门 - MSG_APP_SPAWN 携带 APP_FLAGS_DAEMON_BIN 仍按 app 处理（flag 不能单独提权）
 * @tc.type: FUNC
 * @tc.level: Level0
 */
HWTEST_F(AppSpawnDaemonSpawnTest, App_Spawn_Daemon_FlagOnAppMsgType_001, TestSize.Level0)
{
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawnReqMsgHandle reqHandle = 0;
    int ret = -1;
    do {
        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to init client");

        reqHandle = g_testHelper.CreateMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");
        // 只有 flag、没有 MSG_SPAWN_SERVICE_BIN：属于普通 app 报文
        ret = AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_DAEMON_BIN);
        APPSPAWN_CHECK(ret == 0, break, "Failed to set daemon bin flag");

        AppSpawningCtx *property = g_testHelper.GetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get property");

        EXPECT_FALSE(IsDaemonBinMsg(property->message));
        const SpawnPolicy *pl = ResolvePolicy(property);
        APPSPAWN_CHECK(pl != nullptr, break, "ResolvePolicy returned NULL");
        EXPECT_STREQ(pl->name, "app");
        EXPECT_EQ(pl->uidFrom, POLICY_SRC_TLV_DAC);

        ret = 0;
    } while (0);
    AppSpawnClientDestroy(clientHandle);
    ASSERT_EQ(ret, 0);
}
