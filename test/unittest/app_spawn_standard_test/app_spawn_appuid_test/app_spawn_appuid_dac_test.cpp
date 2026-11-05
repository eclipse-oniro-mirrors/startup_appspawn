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
#include <sys/prctl.h>
#include <poll.h>
#include "cJSON.h"
#include <unistd.h>

#include <string>

#include "app_spawn_appuid_test_helper.h"
#ifdef WITH_SELINUX
#include "hap_restorecon.h"
#endif

using namespace testing::ext;
using OHOS::AppUidTest::BuildAppUidProperty;
using OHOS::AppUidTest::CleanupAppUidProperty;

/* Production functions under test (APPSPAWN_STATIC is empty under APPSPAWN_TEST). */
extern "C" {
int SetUidGid(const AppSpawnMgr *content, const AppSpawningCtx *property);
int NormalizeDacAppUid(AppSpawnMgr *content, AppSpawningCtx *property);
int SetAppUid(uint32_t appUid);
int GetAppUid(uint32_t *appUid);
int SetUserId(char *userIdStr);
int KillReasonReportHook(const AppSpawnMgr *mgr, const AppSpawnedProcessInfo *appInfo);
void SetKillReason(const AppSpawnMgr *mgr, pid_t pid, uid_t uid, int reason, int errorCause);
int GetCgroupPath(const AppSpawnedProcessInfo *appInfo, char *buffer, uint32_t buffLen);
void HandleDiedPid(pid_t pid, uid_t uid, int status);
AppSpawnedProcess *InitSpawnedProcessInfo(AppSpawningCtx *property);
void WriteSignalInfoToFd(AppSpawnedProcess *appInfo, AppSpawnContent *content, int signal);
int32_t SetEnvInfo(const AppSpawnMgr *content, const AppSpawningCtx *property);
}

#ifdef WITH_SELINUX
/* appspawn_adapter.cpp (C++ linkage) */
void SetHapDomainInfo(const AppSpawnMgr *content, const AppSpawningCtx *property,
    AppSpawnMsgDomainInfo *msgDomainInfo, HapDomainInfo *hapDomainInfo);
#endif

namespace OHOS {
namespace system {
/* event_reporter.cpp */
void ProcessMgrRemoveApp(const char *processName, int pid, int appUid, int status);
}  // namespace system
}  // namespace OHOS

namespace {

/* constants: custom sandbox uid is 10x segment, appUid keeps 200xxxxx. */
constexpr uint32_t CUSTOM_SANDBOX_UID = 100;          // userId-like 10x uid, below MIN_VALID_APP_UID
constexpr uint32_t CUSTOM_SANDBOX_APPUID = 20010052;  // 200xxxxx
constexpr uint32_t NORMAL_APP_UID = 20010029;

constexpr int SIGNAL_READ_TIMEOUT_MS = 1000;
constexpr size_t SIGNAL_JSON_BUFFER_SIZE = 512;
constexpr pid_t EVENT_TEST_PID = 9999;

void ExpectSignalAppUid(int fd, uint32_t expectedAppUid)
{
    struct pollfd descriptor = {fd, POLLIN, 0};
    ASSERT_EQ(poll(&descriptor, 1, SIGNAL_READ_TIMEOUT_MS), 1);
    ASSERT_NE(descriptor.revents & POLLIN, 0);
    char buffer[SIGNAL_JSON_BUFFER_SIZE] = {0};
    ASSERT_GT(read(fd, buffer, sizeof(buffer) - 1), 0);
    cJSON *json = cJSON_Parse(buffer);
    ASSERT_NE(json, nullptr);
    const cJSON *uid = cJSON_GetObjectItemCaseSensitive(json, "uid");
    EXPECT_TRUE(cJSON_IsNumber(uid));
    if (cJSON_IsNumber(uid)) {
        EXPECT_EQ(uid->valuedouble, static_cast<double>(expectedAppUid));
    }
    cJSON_Delete(json);
}

class AppSpawnAppUidDacTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override
    {
        savedNoShareFs_ = IsNoShareFsEnable();
        SetNoShareFsEnable(false);
        AppUidMockReset();
    }
    void TearDown() override
    {
        SetNoShareFsEnable(savedNoShareFs_);
        AppUidMockReset();
    }
private:
    bool savedNoShareFs_ = false;
};

/*
 * @tc.name: AppUid_HdacWrite_SetAppUid_001
 * @tc.desc: HDAC_DEV write - SetAppUid pushes appUid to
 *           /dev/access_token_id via ACCESS_TOKENID_SET_APPUID (_IOW('A', 31)).
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_HdacWrite_SetAppUid_001, TestSize.Level1)
{
    AppUidMockReset();
    int ret = SetAppUid(CUSTOM_SANDBOX_APPUID);
    EXPECT_EQ(ret, 0);
    // HDAC_DEV must be opened for the ioctl
    EXPECT_EQ(AppUidMockFindOpenPath("/dev/access_token_id"), 1);
    // The ioctl command must be the new SET_APPUID with the appUid payload
    AppUidIoctlCall call = {};
    unsigned long expectCmd = AppUidMockEncodeIoctlCmd(1, APPUID_HM_SET_APPUID);
    EXPECT_EQ(AppUidMockFindIoctlCall(expectCmd, &call), 1);
    EXPECT_EQ(call.value, CUSTOM_SANDBOX_APPUID);
}

/*
 * @tc.name: AppUid_HdacWrite_GetAppUid_001
 * @tc.desc: HDAC_DEV read - GetAppUid reads appUid back via
 *           ACCESS_TOKENID_GET_APPUID (_IOR('A', 32)).
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_HdacWrite_GetAppUid_001, TestSize.Level1)
{
    AppUidMockReset();
    const uint32_t kernelAppUid = CUSTOM_SANDBOX_APPUID;
    AppUidMockSetGetAppUidValue(kernelAppUid);
    uint32_t appUid = 0;
    int ret = GetAppUid(&appUid);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(appUid, kernelAppUid);
    AppUidIoctlCall call = {};
    unsigned long expectCmd = AppUidMockEncodeIoctlCmd(0, APPUID_HM_GET_APPUID);
    EXPECT_EQ(AppUidMockFindIoctlCall(expectCmd, &call), 1);
    EXPECT_EQ(call.value, kernelAppUid);
}

/*
 * @tc.name: AppUid_SetUidGid_CustomSandbox_001
 * @tc.desc: core rule - custom sandbox app: setresuid keeps uid (10x
 *           segment) while the kernel gets appUid (200xxxxx) via SET_APPUID.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_CustomSandbox_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.customsandbox",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    // enable the real custom sandbox condition so the userId-like 10x uid passes the range check
    SetNoShareFsEnable(true);
    AppUidMockReset();
    int ret = SetUidGid(mgr, property);
    SetNoShareFsEnable(false);
    EXPECT_EQ(ret, 0);

    // process identity: setresuid receives the 10x uid, NOT the appUid
    ASSERT_EQ(AppUidMockSetresuidCallCount(), 1);
    AppUidSetresuidCall setresuid = {};
    EXPECT_EQ(AppUidMockGetSetresuidCall(0, &setresuid), 0);
    EXPECT_EQ(setresuid.ruid, CUSTOM_SANDBOX_UID);
    EXPECT_EQ(setresuid.euid, CUSTOM_SANDBOX_UID);
    EXPECT_EQ(setresuid.suid, CUSTOM_SANDBOX_UID);

    // kernel appUid: SET_APPUID receives the 200xxxxx appUid
    AppUidIoctlCall call = {};
    unsigned long setAppUidCmd = AppUidMockEncodeIoctlCmd(1, APPUID_HM_SET_APPUID);
    EXPECT_EQ(AppUidMockFindIoctlCall(setAppUidCmd, &call), 1);
    EXPECT_EQ(call.value, CUSTOM_SANDBOX_APPUID);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_SetUidGid_NormalApp_001
 * @tc.desc: compat - normal app keeps uid == appUid (200xxxxx): both
 *           setresuid and SET_APPUID receive the same value.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_NormalApp_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.normalapp", NORMAL_APP_UID, NORMAL_APP_UID, false);
    ASSERT_NE(property, nullptr);

    AppUidMockReset();
    int ret = SetUidGid(mgr, property);
    EXPECT_EQ(ret, 0);

    AppUidSetresuidCall setresuid = {};
    ASSERT_EQ(AppUidMockGetSetresuidCall(0, &setresuid), 0);
    EXPECT_EQ(setresuid.ruid, NORMAL_APP_UID);

    AppUidIoctlCall call = {};
    unsigned long setAppUidCmd = AppUidMockEncodeIoctlCmd(1, APPUID_HM_SET_APPUID);
    EXPECT_EQ(AppUidMockFindIoctlCall(setAppUidCmd, &call), 1);
    EXPECT_EQ(call.value, NORMAL_APP_UID);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_SetUidGid_RootUid_001
 * @tc.desc: uid range rule - uid 0 is rejected even for a real custom sandbox
 *           app, so the custom-sandbox path can never spawn a root process.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_RootUid_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.rootuid", 0, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    SetNoShareFsEnable(true);
    AppUidMockReset();
    int ret = SetUidGid(mgr, property);
    SetNoShareFsEnable(false);
    EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_SetUidGid_BothIoctlCoexist_001
 * @tc.desc: SetUserId and SetAppUid are two independent ioctls on HDAC_DEV.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_BothIoctlCoexist_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.coexist",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true, true);
    ASSERT_NE(property, nullptr);

    // enable the real custom sandbox condition so the userId-like 10x uid passes the range check
    SetNoShareFsEnable(true);
    AppUidMockReset();
    int ret = SetUidGid(mgr, property);
    SetNoShareFsEnable(false);
    EXPECT_EQ(ret, 0);
    AppUidIoctlCall call = {};
    EXPECT_EQ(AppUidMockFindIoctlCall(AppUidMockEncodeIoctlCmd(1, APPUID_HM_SET_APPUID), &call), 1);
    EXPECT_EQ(call.value, CUSTOM_SANDBOX_APPUID);
    EXPECT_EQ(AppUidMockFindIoctlCall(AppUidMockEncodeIoctlCmd(1, APPUID_HM_SET_USERID), &call), 1);
    EXPECT_EQ(call.value, CUSTOM_SANDBOX_UID);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_SetUidGid_DacCheckInvalidAppUid_001
 * @tc.desc: DAC check - validation runs on appUid: invalid appUid
 *           (below MIN_VALID_APP_UID) must be rejected even if uid is valid.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_DacCheckInvalidAppUid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    // Keep uid valid so only the appUid validation can reject this request.
    AppSpawningCtx *property = BuildAppUidProperty("com.test.baddac", NORMAL_APP_UID, 1, false);
    ASSERT_NE(property, nullptr);

    int ret = SetUidGid(mgr, property);
    EXPECT_EQ(ret, APPSPAWN_MSG_INVALID);
    // setresuid must not be reached
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_NormalizeDacAppUid_Fallback_001
 * @tc.desc: compat rule - when the caller passes appUid = 0, appUid falls back
 *           to uid (appUid = uid), keeping normal apps unaffected.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_NormalizeDacAppUid_Fallback_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    // appUid = 0 (caller not adapted yet), uid = 20010029
    AppSpawningCtx *property = BuildAppUidProperty("com.test.fallback", NORMAL_APP_UID, 0, false);
    ASSERT_NE(property, nullptr);

    int ret = NormalizeDacAppUid(mgr, property);
    EXPECT_EQ(ret, 0);
    AppDacInfo *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    EXPECT_EQ(dacInfo->appUid, dacInfo->uid);
    EXPECT_EQ(dacInfo->appUid, NORMAL_APP_UID);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_NormalizeDacAppUid_KeepValid_001
 * @tc.desc: - a valid appUid passed by the caller is kept as-is; running the
 *           hook twice (idempotency, MSG_DECODE + BOOT_IMG) changes nothing.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_NormalizeDacAppUid_KeepValid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.keepvalid",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    EXPECT_EQ(NormalizeDacAppUid(mgr, property), 0);
    EXPECT_EQ(NormalizeDacAppUid(mgr, property), 0);  // idempotent
    AppDacInfo *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    EXPECT_EQ(dacInfo->uid, CUSTOM_SANDBOX_UID);
    EXPECT_EQ(dacInfo->appUid, CUSTOM_SANDBOX_APPUID);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_KeepCapabilities_RootUid_001
 * @tc.desc: uid == 0 skips PR_SET_KEEPCAPS even when appUid is non-zero.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_KeepCapabilities_RootUid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    // Keepcaps follows the real process uid, independently of appUid.
    AppSpawningCtx *property = BuildAppUidProperty("com.test.keepcaps0", 0, NORMAL_APP_UID, false);
    ASSERT_NE(property, nullptr);
    // Keep the property hook under test without creating a real sandbox in the UT process.
    ASSERT_EQ(SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_NO_SANDBOX), 0);

    AppUidMockReset();
    // STAGE_CHILD_EXECUTE runs SpawnSetProperties -> SetKeepCapabilities
    int ret = AppSpawnHookExecute(STAGE_CHILD_EXECUTE, HOOK_STOP_WHEN_ERROR,
        GetAppSpawnContent(), reinterpret_cast<AppSpawnClient *>(property));
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);

    bool foundKeepCaps = false;
    for (int i = 0; i < AppUidMockPrctlCallCount(); i++) {
        AppUidPrctlCall call = {};
        if (AppUidMockGetPrctlCall(i, &call) == 0 && call.option == PR_SET_KEEPCAPS) {
            EXPECT_EQ(call.arg2, 1UL);
            foundKeepCaps = true;
        }
    }
    EXPECT_FALSE(foundKeepCaps);
    // hook chain may fail on unrelated child-env steps; not asserted here
    (void)ret;
}

/*
 * @tc.name: AppUid_KeepCapabilities_NonzeroUid_001
 * @tc.desc: non-zero uid enables PR_SET_KEEPCAPS even when appUid is zero
 *           in the pre-normalization message.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_KeepCapabilities_NonzeroUid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    // A raw message isolates the keepcaps decision from appUid normalization.
    AppSpawningCtx *property = BuildAppUidProperty("com.test.keepcapsappuid0",
        CUSTOM_SANDBOX_UID, 0, false);
    ASSERT_NE(property, nullptr);
    // Keep the property hook under test without creating a real sandbox in the UT process.
    ASSERT_EQ(SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_NO_SANDBOX), 0);

    AppUidMockReset();
    (void)AppSpawnHookExecute(STAGE_CHILD_EXECUTE, HOOK_STOP_WHEN_ERROR,
        GetAppSpawnContent(), reinterpret_cast<AppSpawnClient *>(property));

    bool foundKeepCaps = false;
    for (int i = 0; i < AppUidMockPrctlCallCount(); i++) {
        AppUidPrctlCall call = {};
        if (AppUidMockGetPrctlCall(i, &call) == 0 && call.option == PR_SET_KEEPCAPS) {
            EXPECT_EQ(call.arg2, 1UL);
            foundKeepCaps = true;
        }
    }
    EXPECT_TRUE(foundKeepCaps);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_KeepCapabilities_CustomSandbox_001
 * @tc.desc: - custom sandbox app (uid=10x, appUid=200xxxxx) enables
 *           PR_SET_KEEPCAPS because the real process uid is non-zero.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_KeepCapabilities_CustomSandbox_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.keepcaps10x",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);
    // Keep the property hook under test without creating a real sandbox in the UT process.
    ASSERT_EQ(SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_NO_SANDBOX), 0);

    AppUidMockReset();
    (void)AppSpawnHookExecute(STAGE_CHILD_EXECUTE, HOOK_STOP_WHEN_ERROR,
        GetAppSpawnContent(), reinterpret_cast<AppSpawnClient *>(property));

    bool foundKeepCaps = false;
    for (int i = 0; i < AppUidMockPrctlCallCount(); i++) {
        AppUidPrctlCall call = {};
        if (AppUidMockGetPrctlCall(i, &call) == 0 && call.option == PR_SET_KEEPCAPS) {
            EXPECT_EQ(call.arg2, 1UL);
            foundKeepCaps = true;
        }
    }
    EXPECT_TRUE(foundKeepCaps);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_IsShareUidApp_FlagMatrix_001
 * @tc.desc: custom sandbox detection - IsShareUidApp is true only when
 *           nosharefs (PC) is enabled AND (CUSTOM_SANDBOX or SHAREUID) flag set.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_IsShareUidApp_FlagMatrix_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.flagmatrix",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    // PC (nosharefs) enabled + CUSTOM_SANDBOX -> custom sandbox
    SetNoShareFsEnable(true);
    EXPECT_TRUE(IsShareUidApp(property));

    // phone platform (nosharefs disabled) -> never custom sandbox
    SetNoShareFsEnable(false);
    EXPECT_FALSE(IsShareUidApp(property));

    // restore PC and check SHAREUID flag alone also works
    SetNoShareFsEnable(true);
    AppSpawningCtx *shareUidProperty = BuildAppUidProperty("com.test.shareuid",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, false);
    ASSERT_NE(shareUidProperty, nullptr);
    (void)SetAppSpawnMsgFlag(shareUidProperty->message, TLV_MSG_FLAGS, APP_FLAGS_SHAREUID_ENABLED);
    EXPECT_TRUE(IsShareUidApp(shareUidProperty));

    SetNoShareFsEnable(false);
    CleanupAppUidProperty(property);
    CleanupAppUidProperty(shareUidProperty);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_ProcessMgr_InitSpawnedProcessInfo_001
 * @tc.desc: process management keeps the real uid and application appUid separately.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_ProcessMgr_InitSpawnedProcessInfo_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    SetNoShareFsEnable(true);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.procinfo",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);
    property->pid = 4321;

    AppSpawnedProcess *appInfo = InitSpawnedProcessInfo(property);
    ASSERT_NE(appInfo, nullptr);
    EXPECT_EQ(appInfo->uid, CUSTOM_SANDBOX_UID);
    EXPECT_EQ(appInfo->appUid, CUSTOM_SANDBOX_APPUID);
    EXPECT_EQ(appInfo->pid, 4321);

    SetNoShareFsEnable(false);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_DiedPid_CustomSandbox_KernelUid_001
 * @tc.desc: custom sandbox app dies with kernel uid=10x but appUid=200xxxxx:
 *           still matched, removed, and the signal JSON carries appUid.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_DiedPid_CustomSandbox_KernelUid_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawnContent *content = GetAppSpawnContent();
    ASSERT_NE(content, nullptr);

    int pipefd[2] = {-1, -1};
    ASSERT_EQ(pipe(pipefd), 0);
    content->signalFd = pipefd[1];

    AppSpawnedProcess *appInfo = AddSpawnedProcess(4322, "com.test.diedpid", 0, false, 12345678);
    ASSERT_NE(appInfo, nullptr);
    appInfo->uid = CUSTOM_SANDBOX_UID;
    appInfo->appUid = CUSTOM_SANDBOX_APPUID;

    // kernel reports the real process uid (10x) on death
    HandleDiedPid(4322, CUSTOM_SANDBOX_UID, 0);

    // process must be removed from the spawned queue
    EXPECT_EQ(GetSpawnedProcess(4322), nullptr);

    ExpectSignalAppUid(pipefd[0], CUSTOM_SANDBOX_APPUID);

    close(pipefd[0]);
    close(pipefd[1]);
    content->signalFd = -1;
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_CgroupPath_UsesAppUid_001
 * @tc.desc: cgroup management - cgroup path userId is derived from
 *           appUid (20010052 / 200000 = 100), not the 10x uid.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_CgroupPath_UsesAppUid_001, TestSize.Level1)
{
    const char *name = "com.example.cgroupapp";
    size_t nameLen = strlen(name) + 1;
    auto *appInfo = reinterpret_cast<AppSpawnedProcessInfo *>(calloc(1, sizeof(AppSpawnedProcessInfo) + nameLen));
    ASSERT_NE(appInfo, nullptr);
    (void)strcpy_s(appInfo->name, nameLen, name);
    appInfo->pid = 33;
    appInfo->appUid = CUSTOM_SANDBOX_APPUID;

    char path[PATH_MAX] = {0};
    int ret = GetCgroupPath(appInfo, path, sizeof(path));
    EXPECT_EQ(ret, 0);
    // userId must come from appUid: /dev/pids/testpids/100/... (APPSPAWN_TEST branch)
    EXPECT_NE(nullptr, strstr(path, "/dev/pids/testpids/100/"));
    EXPECT_EQ(nullptr, strstr(path, "/dev/pids/testpids/0/"));
    free(appInfo);
}

/*
 * @tc.name: AppUid_KillReason_SetKillReason_AppUid_001
 * @tc.desc: kill reason - SetKillReason is invoked with appUid for
 *           reporting through the cleanup hook and the sysload ioctl payload.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_KillReason_SetKillReason_AppUid_001, TestSize.Level2)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppUidMockReset();
    AppSpawnedProcess appInfo = {};
    appInfo.pid = 5678;
    appInfo.appUid = CUSTOM_SANDBOX_APPUID;
    appInfo.killReason.reason = 1;
    appInfo.killReason.errorCause = 9;
    EXPECT_EQ(KillReasonReportHook(mgr, &appInfo), 0);
    EXPECT_EQ(AppUidMockKillCallCount(), 1);
    EXPECT_EQ(AppUidMockGetKillUid(), static_cast<int>(CUSTOM_SANDBOX_APPUID));
    // /dev/sysload is opened for the kill-info ioctl
    EXPECT_EQ(AppUidMockFindOpenPath("/dev/sysload"), 1);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_EventReporter_ProcessMgrRemoveApp_001
 * @tc.desc: event reporting - ProcessMgrRemoveApp takes appUid as the UID
 *           param through the process hook; normal exit does not emit an event.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_EventReporter_ProcessMgrRemoveApp_001, TestSize.Level2)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawnedProcess *appInfo = AddSpawnedProcess(EVENT_TEST_PID, "com.test.eventreport", 0, false, 0);
    ASSERT_NE(appInfo, nullptr);
    appInfo->appUid = CUSTOM_SANDBOX_APPUID;
    appInfo->exitStatus = 1;
    ProcessMgrHookExecute(STAGE_SERVER_APP_DIED, GetAppSpawnContent(), appInfo);
    AppUidEventCall event = AppUidMockGetEvent();
    EXPECT_EQ(event.count, 1);
    EXPECT_EQ(event.pid, appInfo->pid);
    EXPECT_EQ(event.appUid, static_cast<int>(CUSTOM_SANDBOX_APPUID));
    EXPECT_EQ(event.status, appInfo->exitStatus);
    AppUidMockReset();
    appInfo->exitStatus = 0;
    ProcessMgrHookExecute(STAGE_SERVER_APP_DIED, GetAppSpawnContent(), appInfo);
    EXPECT_EQ(AppUidMockGetEvent().count, 0);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_HdacWrite_SetAppUid_OpenFail_001
 * @tc.desc: HDAC_DEV write abnormal branch - open("/dev/access_token_id")
 *           failure makes SetAppUid return -1.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_HdacWrite_SetAppUid_OpenFail_001, TestSize.Level2)
{
    AppUidMockReset();
    AppUidMockSetOpenRet(-1);
    int ret = SetAppUid(CUSTOM_SANDBOX_APPUID);
    EXPECT_EQ(ret, -1);
    AppUidMockReset();
}

/*
 * @tc.name: AppUid_HdacWrite_SetAppUid_IoctlFail_001
 * @tc.desc: HDAC_DEV write/read abnormal branch - ioctl failure makes both
 *           SetAppUid and GetAppUid return -1.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_HdacWrite_SetAppUid_IoctlFail_001, TestSize.Level2)
{
    AppUidMockReset();
    AppUidMockSetIoctlRet(-1);
    EXPECT_EQ(SetAppUid(CUSTOM_SANDBOX_APPUID), -1);
    uint32_t appUid = 0;
    EXPECT_EQ(GetAppUid(&appUid), -1);
    AppUidMockSetIoctlRet(0);  // restore default
}

/*
 * @tc.name: AppUid_NormalizeDacAppUid_BootImgStage_001
 * @tc.desc: the image-process path only runs BOOT_IMG hooks; the hook
 *           registered there still normalizes appUid=0 to uid.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_NormalizeDacAppUid_BootImgStage_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.bootimg", NORMAL_APP_UID, 0, false);
    ASSERT_NE(property, nullptr);

    int ret = AppSpawnHookExecute(STAGE_PARENT_BOOT_IMG, 0,
        GetAppSpawnContent(), reinterpret_cast<AppSpawnClient *>(property));
    EXPECT_EQ(ret, 0);
    AppDacInfo *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    EXPECT_EQ(dacInfo->appUid, dacInfo->uid);
    EXPECT_EQ(dacInfo->appUid, NORMAL_APP_UID);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

#ifdef WITH_SELINUX
/*
 * @tc.name: AppUid_DomainInfo_SetHapDomainInfo_001
 * @tc.desc: domain info reporting - HapDomainInfo.uid is filled with
 *           dacInfo->appUid (200xxxxx), not the 10x process uid.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_DomainInfo_SetHapDomainInfo_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.domaininfo",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    AppSpawnMsgDomainInfo *msgDomainInfo = reinterpret_cast<AppSpawnMsgDomainInfo *>(
        GetAppProperty(property, TLV_DOMAIN_INFO));
    ASSERT_NE(msgDomainInfo, nullptr);
    HapDomainInfo hapDomainInfo = {};
    SetHapDomainInfo(mgr, property, msgDomainInfo, &hapDomainInfo);
    EXPECT_EQ(hapDomainInfo.uid, CUSTOM_SANDBOX_APPUID);
    EXPECT_EQ(hapDomainInfo.uid != CUSTOM_SANDBOX_UID, true);
    EXPECT_EQ(hapDomainInfo.packageName, std::string("com.test.domaininfo"));

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}
#endif

/*
 * @tc.name: AppUid_EnvInfo_UserEnvUsesAppUid_001
 * @tc.desc: common env - SetEnvInfo exports USER derived from appUid/UID_BASE
 *           (20010052 -> "100"), never the 10x uid (which would give "0").
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_EnvInfo_UserEnvUsesAppUid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.userenv",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    int32_t ret = SetEnvInfo(mgr, property);
    EXPECT_EQ(ret, 0);
    const char *user = getenv("USER");
    ASSERT_NE(user, nullptr);
    EXPECT_STREQ(user, "100");
    unsetenv("USER");

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_DiedPid_NormalApp_001
 * @tc.desc: died-pid matching - a normal app (uid == appUid) is matched and
 *           removed when the kernel reports the same 200xxxxx uid.
 */
HWTEST_F(AppSpawnAppUidDacTest, AppUid_DiedPid_NormalApp_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawnContent *content = GetAppSpawnContent();
    ASSERT_NE(content, nullptr);

    int pipefd[2] = {-1, -1};
    ASSERT_EQ(pipe(pipefd), 0);
    content->signalFd = pipefd[1];

    AppSpawnedProcess *appInfo = AddSpawnedProcess(4323, "com.test.diedpid.normal", 0, false, 12345679);
    ASSERT_NE(appInfo, nullptr);
    appInfo->uid = NORMAL_APP_UID;
    appInfo->appUid = NORMAL_APP_UID;

    // kernel reports the process uid, which equals appUid for a normal app
    HandleDiedPid(4323, NORMAL_APP_UID, 0);
    EXPECT_EQ(GetSpawnedProcess(4323), nullptr);

    ExpectSignalAppUid(pipefd[0], NORMAL_APP_UID);

    close(pipefd[0]);
    close(pipefd[1]);
    content->signalFd = -1;
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_ShareUid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.uidboundary",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, false);
    ASSERT_NE(property, nullptr);
    SetNoShareFsEnable(true);
    EXPECT_EQ(SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_SHAREUID_ENABLED), 0);
    EXPECT_EQ(SetUidGid(mgr, property), 0);
    AppUidSetresuidCall call = {};
    EXPECT_EQ(AppUidMockGetSetresuidCall(0, &call), 0);
    EXPECT_EQ(call.ruid, CUSTOM_SANDBOX_UID);
    AppUidIoctlCall ioctlCall = {};
    EXPECT_EQ(AppUidMockFindIoctlCall(AppUidMockEncodeIoctlCmd(1, APPUID_HM_SET_APPUID), &ioctlCall), 1);
    EXPECT_EQ(ioctlCall.value, CUSTOM_SANDBOX_APPUID);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_NonPcCustomFlag_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.uidboundary", NORMAL_APP_UID, NORMAL_APP_UID, true);
    ASSERT_NE(property, nullptr);
    SetNoShareFsEnable(false);
    EXPECT_EQ(SetUidGid(mgr, property), 0);
    AppUidSetresuidCall call = {};
    EXPECT_EQ(AppUidMockGetSetresuidCall(0, &call), 0);
    EXPECT_EQ(call.ruid, NORMAL_APP_UID);
    AppUidIoctlCall ioctlCall = {};
    EXPECT_EQ(AppUidMockFindIoctlCall(AppUidMockEncodeIoctlCmd(1, APPUID_HM_SET_APPUID), &ioctlCall), 1);
    EXPECT_EQ(ioctlCall.value, NORMAL_APP_UID);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_UidUpperBoundary_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.uidboundary", UINT32_MAX, NORMAL_APP_UID, false);
    ASSERT_NE(property, nullptr);
    SetNoShareFsEnable(false);
    EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
    EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_AppUidUpperBoundary_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.uidboundary", NORMAL_APP_UID, UINT32_MAX, false);
    ASSERT_NE(property, nullptr);
    SetNoShareFsEnable(false);
    EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
    EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_NormalizeNonzeroInvalid_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.invalidappuid", NORMAL_APP_UID, 1, false);
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(AppSpawnHookExecute(STAGE_PARENT_MSG_DECODE, HOOK_STOP_WHEN_ERROR,
        GetAppSpawnContent(), reinterpret_cast<AppSpawnClient *>(property)), 0);
    auto *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    EXPECT_EQ(dacInfo->appUid, 1u);
    EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_NormalizeDecodeStage_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.decodefallback", NORMAL_APP_UID, 0, false);
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(AppSpawnHookExecute(STAGE_PARENT_MSG_DECODE, HOOK_STOP_WHEN_ERROR,
        GetAppSpawnContent(), reinterpret_cast<AppSpawnClient *>(property)), 0);
    auto *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    EXPECT_EQ(dacInfo->appUid, NORMAL_APP_UID);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}


// These fixtures exercise the caller-visible boundary through SetUidGid.
constexpr uint32_t MIN_APP_UID = 10000;
constexpr uint32_t MIN_APP_GID = 10000;

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_NormalLowUidRejected_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    const uint32_t invalidUids[] = {1, CUSTOM_SANDBOX_UID, MIN_APP_UID - 1};
    for (bool isPc : {false, true}) {
        SetNoShareFsEnable(isPc);
        for (uint32_t uid : invalidUids) {
            AppSpawningCtx *property = BuildAppUidProperty("com.test.lowuid", uid, NORMAL_APP_UID, false);
            ASSERT_NE(property, nullptr);
            AppUidMockReset();
            EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
            EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
            EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
            CleanupAppUidProperty(property);
        }
    }
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_NonPcLowUidFlagsRejected_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    SetNoShareFsEnable(false);
    const uint32_t flags[] = {APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SHAREUID_ENABLED};
    for (uint32_t flag : flags) {
        AppSpawningCtx *property = BuildAppUidProperty("com.test.nonpclowuid",
            CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, false);
        ASSERT_NE(property, nullptr);
        ASSERT_EQ(SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, flag), 0);
        AppUidMockReset();
        EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
        EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
        EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
        CleanupAppUidProperty(property);
    }
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_NormalLowerBoundary_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.minuid", MIN_APP_UID, MIN_APP_UID, false);
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(SetUidGid(mgr, property), 0);
    AppUidSetresuidCall call = {};
    ASSERT_EQ(AppUidMockGetSetresuidCall(0, &call), 0);
    EXPECT_EQ(call.ruid, MIN_APP_UID);
    AppDacInfo *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    dacInfo->appUid = MIN_APP_UID - 1;
    AppUidMockReset();
    EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
    EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_HdacRead_InvalidInputAndOpenFail_001, TestSize.Level1)
{
    EXPECT_EQ(GetAppUid(nullptr), -1);
    EXPECT_EQ(AppUidMockFindOpenPath("/dev/access_token_id"), 0);
    uint32_t appUid = NORMAL_APP_UID;
    AppUidMockSetOpenRet(-1);
    EXPECT_EQ(GetAppUid(&appUid), -1);
    EXPECT_EQ(appUid, NORMAL_APP_UID);
    EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_SharedLowerBoundary_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    SetNoShareFsEnable(true);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.sharedminuid",
        CUSTOM_SANDBOX_UID - 1, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
    EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
    AppDacInfo *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    dacInfo->uid = CUSTOM_SANDBOX_UID;
    EXPECT_EQ(SetUidGid(mgr, property), 0);
    AppUidSetresuidCall call = {};
    ASSERT_EQ(AppUidMockGetSetresuidCall(0, &call), 0);
    EXPECT_EQ(call.ruid, CUSTOM_SANDBOX_UID);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

HWTEST_F(AppSpawnAppUidDacTest, AppUid_SetUidGid_InvalidGroups_001, TestSize.Level1)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.invalidgroups",
        NORMAL_APP_UID, NORMAL_APP_UID, false);
    ASSERT_NE(property, nullptr);
    AppDacInfo *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    const uint32_t invalidGids[] = {0, MIN_APP_GID - 1, UINT32_MAX};
    for (uint32_t gid : invalidGids) {
        dacInfo->gid = gid;
        EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
    }
    dacInfo->gid = MIN_APP_GID;
    dacInfo->gidCount = APP_MAX_GIDS + 1;
    EXPECT_EQ(SetUidGid(mgr, property), APPSPAWN_MSG_INVALID);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 0);
    EXPECT_EQ(AppUidMockIoctlCallCount(), 0);
    dacInfo->gidCount = APP_MAX_GIDS;
    EXPECT_EQ(SetUidGid(mgr, property), 0);
    EXPECT_EQ(AppUidMockSetresuidCallCount(), 1);
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

}  // namespace
