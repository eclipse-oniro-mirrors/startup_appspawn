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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "appspawn_adapter.h"
#include "appspawn_fd_manager.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_modulemgr.h"
#include "appspawn_utils.h"
#include "lib_wrapper.h"
#include "securec.h"

// Opaque forward declaration: struct KillInfo is defined in appspawn_kill_reason.c.
// Only used to pass nullptr into InitKillInfo for its invalid-pointer branch.
struct KillInfo;

#ifdef __cplusplus
extern "C" {
#endif

APPSPAWN_STATIC void InitKillInfo(struct KillInfo *info, pid_t pid, uid_t uid, int reason, int errorCause);
APPSPAWN_STATIC int KillReasonReportHook(const AppSpawnMgr *mgr, const AppSpawnedProcessInfo *appInfo);

#ifdef __cplusplus
}
#endif

using namespace testing;
using namespace testing::ext;

namespace OHOS {
namespace {
// Keep in sync with struct KillInfo/KillEventInfo in modules/common/appspawn_kill_reason.c
#define CALLING_PROCESS_NAME_SIZE 16

struct TestKillEventInfo {
    int id;
    int adj;
    bool processed;
    bool foreground;
    pid_t pid;
    int uid;
    pid_t callingPid;
    char callingProcessName[CALLING_PROCESS_NAME_SIZE];
    int64_t timestamp;
    int64_t eventParamFirst;
    int64_t eventParamSecond;
    int64_t eventParamThird;
    int64_t eventParamFourth;
    int64_t eventParamFifth;
    int64_t eventParamSixth;
    int64_t eventParamSeventh;
};

struct TestKillInfo {
    unsigned int magic;
    pid_t pid;
    struct TestKillEventInfo data;
    unsigned int structSize;
};

constexpr unsigned int TEST_SET_KILL_INFO_MAGIC = 0xE5AC03;
constexpr int TEST_KILL_LOG_BASE = 'S';
constexpr unsigned int TEST_SET_KILL_INFO = static_cast<unsigned int>(_IOWR(TEST_KILL_LOG_BASE, 0x07, int32_t));

constexpr pid_t TEST_APP_PID = 3311;
constexpr uid_t TEST_APP_UID = 20010021;
constexpr int TEST_CUSTOM_REASON = 1234;

// ===== Mock state =====
int g_ioctlCallCount = 0;
int g_ioctlLastReq = 0;
int g_ioctlLastFd = -1;
int g_ioctlRet = 0;
TestKillInfo g_ioctlLastInfo = {};

int g_openSysloadCount = 0;
bool g_openSysloadFail = false;

bool g_memsetSFail = false;

void ResetMockState(void)
{
    // Cleared first: ResetMockState itself calls memset_s below and must not be mocked
    g_memsetSFail = false;
    g_ioctlCallCount = 0;
    g_ioctlLastReq = 0;
    g_ioctlLastFd = -1;
    g_ioctlRet = 0;
    (void)memset_s(&g_ioctlLastInfo, sizeof(g_ioctlLastInfo), 0, sizeof(g_ioctlLastInfo));
    g_openSysloadCount = 0;
    g_openSysloadFail = false;
}

// Fails only memset_s calls that target a KillInfo-sized buffer, matching InitKillInfo
int TestMemsetSFunc(void *dest, size_t destMax, int c, size_t count)
{
    if (g_memsetSFail && destMax == sizeof(TestKillInfo)) {
        return EINVAL;
    }
    return __real_memset_s(dest, destMax, c, count);
}

// Redirect /dev/sysload to /dev/null so that a real fd is returned and can be closed safely
int TestOpenFunc(const char *pathname, int flags, mode_t mode)
{
    if (pathname != nullptr && strcmp(pathname, DEV_SYSLOAD) == 0) {
        g_openSysloadCount++;
        if (g_openSysloadFail) {
            errno = ENOENT;
            return -1;
        }
        return __real_open("/dev/null", O_RDWR, mode);
    }
    return __real_open(pathname, flags, mode);
}

int TestIoctlFunc(int fd, int req, va_list args)
{
    void *arg = va_arg(args, void *);
    // Only intercept the kill reason request, forward everything else to the real ioctl
    if (static_cast<unsigned int>(req) != TEST_SET_KILL_INFO) {
        return __real_ioctl(fd, req, arg);
    }
    g_ioctlCallCount++;
    g_ioctlLastFd = fd;
    g_ioctlLastReq = req;
    if (arg != nullptr) {
        (void)memcpy_s(&g_ioctlLastInfo, sizeof(g_ioctlLastInfo), arg, sizeof(g_ioctlLastInfo));
    }
    if (g_ioctlRet != 0) {
        errno = EINVAL;
    }
    return g_ioctlRet;
}

AppSpawnedProcess *CreateTestAppInfo(const char *name, pid_t pid, uid_t uid, int killReason)
{
    size_t len = strlen(name) + 1;
    AppSpawnedProcess *appInfo = static_cast<AppSpawnedProcess *>(calloc(1, sizeof(AppSpawnedProcess) + len));
    if (appInfo == nullptr) {
        return nullptr;
    }
    appInfo->pid = pid;
    appInfo->uid = uid;
    appInfo->max = 0;
    appInfo->exitStatus = 0;
    appInfo->killReason.reason = killReason;
    appInfo->killReason.errorCause = 0;
    if (memcpy_s(appInfo->name, len, name, len) != 0) {
        free(appInfo);
        return nullptr;
    }
    OH_ListInit(&appInfo->node);
    return appInfo;
}
}  // namespace

class AppSpawnKillReasonTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " start";
        APPSPAWN_LOGI("%{public}s.%{public}s start", info->test_suite_name(), info->name());
        ResetMockState();
        UpdateOpenFunc(TestOpenFunc);
        UpdateIoctlFunc(TestIoctlFunc);
        UpdateMemsetSFunc(TestMemsetSFunc);
        mgr_ = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr_, nullptr);
    }
    void TearDown()
    {
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " end";
        APPSPAWN_LOGI("%{public}s.%{public}s end", info->test_suite_name(), info->name());
        if (mgr_ != nullptr) {
            DeleteAppSpawnMgr(mgr_);
            mgr_ = nullptr;
        }
        UpdateIoctlFunc(nullptr);
        UpdateOpenFunc(nullptr);
        UpdateMemsetSFunc(nullptr);
        ResetMockState();
    }

protected:
    AppSpawnMgr *mgr_ = nullptr;
};

/**
 * @tc.name: App_Spawn_KillReason_Set_001
 * @tc.desc: SetKillReason正常流程：首次上报时打开/dev/sysload并下发ioctl，
 *           校验KillInfo中magic/pid/uid/reason/structSize等字段填充正确
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_001, TestSize.Level0)
{
    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);

    EXPECT_EQ(g_openSysloadCount, 1);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastReq, static_cast<int>(TEST_SET_KILL_INFO));
    EXPECT_GE(g_ioctlLastFd, 0);

    EXPECT_EQ(g_ioctlLastInfo.magic, TEST_SET_KILL_INFO_MAGIC);
    EXPECT_EQ(g_ioctlLastInfo.pid, TEST_APP_PID);
    EXPECT_EQ(g_ioctlLastInfo.structSize, static_cast<unsigned int>(sizeof(TestKillInfo)));
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_APPSPAWN_STOP);
    EXPECT_EQ(g_ioctlLastInfo.data.pid, TEST_APP_PID);
    EXPECT_EQ(g_ioctlLastInfo.data.uid, static_cast<int>(TEST_APP_UID));
    EXPECT_FALSE(g_ioctlLastInfo.data.processed);
    EXPECT_FALSE(g_ioctlLastInfo.data.foreground);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_002
 * @tc.desc: SetKillReason成功后fd以TYPE_KILL_REASON_FD、pid=getpid()注册到spawningFdsQueue，
 *           节点count=1且fd有效
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_002, TestSize.Level0)
{
    EXPECT_EQ(FindSpawningFdsByPid(mgr_, getpid(), TYPE_KILL_REASON_FD), nullptr);

    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_KILL_CGROUP, 0);

    AppSpawnFds *fds = FindSpawningFdsByPid(mgr_, getpid(), TYPE_KILL_REASON_FD);
    ASSERT_NE(fds, nullptr);
    EXPECT_EQ(fds->type, TYPE_KILL_REASON_FD);
    EXPECT_EQ(fds->count, static_cast<uint32_t>(1));
    EXPECT_EQ(fds->pid, getpid());
    EXPECT_GE(fds->fds[0], 0);
    EXPECT_EQ(g_ioctlLastFd, fds->fds[0]);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_003
 * @tc.desc: 多次SetKillReason复用已注册的fd：/dev/sysload只打开一次，ioctl下发3次，
 *           队列中仍只有一个TYPE_KILL_REASON_FD节点
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_003, TestSize.Level0)
{
    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);
    SetKillReason(mgr_, TEST_APP_PID + 1, TEST_APP_UID, REASON_KILL_CGROUP, 0);
    SetKillReason(mgr_, TEST_APP_PID + 2, TEST_APP_UID, TEST_CUSTOM_REASON, 0);

    EXPECT_EQ(g_openSysloadCount, 1);
    EXPECT_EQ(g_ioctlCallCount, 3);
    // last call info
    EXPECT_EQ(g_ioctlLastInfo.pid, TEST_APP_PID + 2);
    EXPECT_EQ(g_ioctlLastInfo.data.id, TEST_CUSTOM_REASON);

    uint32_t total = 0;
    uint32_t childParentCount = 0;
    uint32_t parentChildCount = 0;
    GetSpawningFdsStats(mgr_, &total, &childParentCount, &parentChildCount);
    EXPECT_EQ(total, static_cast<uint32_t>(1));
    EXPECT_EQ(childParentCount, static_cast<uint32_t>(0));
    EXPECT_EQ(parentChildCount, static_cast<uint32_t>(0));
}

/**
 * @tc.name: App_Spawn_KillReason_Set_004
 * @tc.desc: 打开/dev/sysload失败时SetKillReason直接返回：不下发ioctl，不注册fd节点
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_004, TestSize.Level0)
{
    g_openSysloadFail = true;

    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);

    EXPECT_EQ(g_openSysloadCount, 1);
    EXPECT_EQ(g_ioctlCallCount, 0);
    EXPECT_EQ(FindSpawningFdsByPid(mgr_, getpid(), TYPE_KILL_REASON_FD), nullptr);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_005
 * @tc.desc: ioctl失败时不崩溃，fd仍保留在队列中，下次上报复用该fd（open只调用一次）
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_005, TestSize.Level0)
{
    g_ioctlRet = -1;

    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_NE(FindSpawningFdsByPid(mgr_, getpid(), TYPE_KILL_REASON_FD), nullptr);

    g_ioctlRet = 0;
    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);
    EXPECT_EQ(g_ioctlCallCount, 2);
    EXPECT_EQ(g_openSysloadCount, 1);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_006
 * @tc.desc: mgr为NULL时无法注册fd，SetKillReason打开fd后回收并返回，不下发ioctl
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_006, TestSize.Level0)
{
    SetKillReason(nullptr, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);

    EXPECT_EQ(g_openSysloadCount, 1);
    EXPECT_EQ(g_ioctlCallCount, 0);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_007
 * @tc.desc: SetKillReason边界入参：pid=0、uid=0、reason=0以及负值reason均按原值填充下发
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_007, TestSize.Level0)
{
    SetKillReason(mgr_, 0, 0, 0, 0);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.magic, TEST_SET_KILL_INFO_MAGIC);
    EXPECT_EQ(g_ioctlLastInfo.pid, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.id, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.uid, 0);

    SetKillReason(mgr_, -1, TEST_APP_UID, -1, 0);
    EXPECT_EQ(g_ioctlCallCount, 2);
    EXPECT_EQ(g_ioctlLastInfo.pid, -1);
    EXPECT_EQ(g_ioctlLastInfo.data.pid, -1);
    EXPECT_EQ(g_ioctlLastInfo.data.id, -1);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_001
 * @tc.desc: STAGE_SERVER_APP_CLEANUP阶段killReason=0时不上报：返回0且不下发ioctl
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_001, TestSize.Level0)
{
    AppSpawnedProcess *appInfo = CreateTestAppInfo("kill-reason-test-001", TEST_APP_PID, TEST_APP_UID, 0);
    ASSERT_NE(appInfo, nullptr);

    int ret = ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, reinterpret_cast<AppSpawnContent *>(mgr_), appInfo);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(g_ioctlCallCount, 0);
    EXPECT_EQ(g_openSysloadCount, 0);

    free(appInfo);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_002
 * @tc.desc: STAGE_SERVER_APP_CLEANUP阶段killReason=REASON_APPSPAWN_STOP时上报，
 *           下发的KillInfo与appInfo的pid/uid/killReason一致
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_002, TestSize.Level0)
{
    AppSpawnedProcess *appInfo =
        CreateTestAppInfo("kill-reason-test-002", TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP);
    ASSERT_NE(appInfo, nullptr);

    int ret = ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, reinterpret_cast<AppSpawnContent *>(mgr_), appInfo);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.pid, TEST_APP_PID);
    EXPECT_EQ(g_ioctlLastInfo.data.uid, static_cast<int>(TEST_APP_UID));
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_APPSPAWN_STOP);

    free(appInfo);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_003
 * @tc.desc: 多个应用连续走STAGE_SERVER_APP_CLEANUP：仅打开一次/dev/sysload，
 *           每个应用各上报一次，REASON_APPSPAWN_STOP与REASON_KILL_CGROUP互不影响
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_003, TestSize.Level0)
{
    AppSpawnedProcess *appInfo1 =
        CreateTestAppInfo("kill-reason-test-003-1", TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP);
    ASSERT_NE(appInfo1, nullptr);
    AppSpawnedProcess *appInfo2 =
        CreateTestAppInfo("kill-reason-test-003-2", TEST_APP_PID + 1, TEST_APP_UID + 1, REASON_KILL_CGROUP);
    if (appInfo2 == nullptr) {
        free(appInfo1);
        FAIL() << "Failed to create appInfo2";
    }

    AppSpawnContent *content = reinterpret_cast<AppSpawnContent *>(mgr_);
    EXPECT_EQ(ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, content, appInfo1), 0);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_APPSPAWN_STOP);
    EXPECT_EQ(ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, content, appInfo2), 0);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_KILL_CGROUP);
    EXPECT_EQ(g_ioctlLastInfo.pid, TEST_APP_PID + 1);

    EXPECT_EQ(g_ioctlCallCount, 2);
    EXPECT_EQ(g_openSysloadCount, 1);

    free(appInfo1);
    free(appInfo2);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_004
 * @tc.desc: STAGE_SERVER_APP_CLEANUP阶段异常入参被ProcessMgrHookExecute拦截：
 *           appInfo为NULL、content为NULL时返回非0，KillReasonReportHook不被执行也不上报
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_004, TestSize.Level0)
{
    AppSpawnedProcess *appInfo =
        CreateTestAppInfo("kill-reason-test-004", TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP);
    ASSERT_NE(appInfo, nullptr);

    EXPECT_NE(ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, reinterpret_cast<AppSpawnContent *>(mgr_), nullptr), 0);
    EXPECT_NE(ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, nullptr, appInfo), 0);
    EXPECT_EQ(g_ioctlCallCount, 0);
    EXPECT_EQ(g_openSysloadCount, 0);

    free(appInfo);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_005
 * @tc.desc: KillReasonReportHook只注册在STAGE_SERVER_APP_CLEANUP：
 *           STAGE_SERVER_APP_ADD/STAGE_SERVER_APP_DIED阶段不触发上报
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_005, TestSize.Level0)
{
    AppSpawnedProcess *appInfo =
        CreateTestAppInfo("kill-reason-test-005", TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP);
    ASSERT_NE(appInfo, nullptr);

    AppSpawnContent *content = reinterpret_cast<AppSpawnContent *>(mgr_);
    EXPECT_EQ(ProcessMgrHookExecute(STAGE_SERVER_APP_ADD, content, appInfo), 0);
    EXPECT_EQ(ProcessMgrHookExecute(STAGE_SERVER_APP_DIED, content, appInfo), 0);
    EXPECT_EQ(g_ioctlCallCount, 0);
    EXPECT_EQ(g_openSysloadCount, 0);

    free(appInfo);
}

/**
 * @tc.name: App_Spawn_KillReason_Define_001
 * @tc.desc: 校验kill reason上报相关常量：REASON_APPSPAWN_STOP=3053、REASON_KILL_CGROUP=8、
 *           DEV_SYSLOAD="/dev/sysload"
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Define_001, TestSize.Level0)
{
    EXPECT_EQ(REASON_APPSPAWN_STOP, 3053);
    EXPECT_EQ(REASON_KILL_CGROUP, 8);
    EXPECT_EQ(REASON_SIGNAL_KILL, 1008);
    EXPECT_EQ(REASON_SIGNAL_EXIT, 1009);
    EXPECT_STREQ(DEV_SYSLOAD, "/dev/sysload");
}
/**
 * @tc.name: App_Spawn_KillReason_Set_008
 * @tc.desc: 连续两次上报之间KillInfo被完整重置：magic/structSize每次重新填充，
 *           未使用的adj/timestamp/eventParam均为0，不残留上一次的值
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_008, TestSize.Level0)
{
    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, TEST_CUSTOM_REASON, 0);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.data.id, TEST_CUSTOM_REASON);

    SetKillReason(mgr_, TEST_APP_PID + 1, TEST_APP_UID + 1, REASON_KILL_CGROUP, 0);
    EXPECT_EQ(g_ioctlCallCount, 2);
    EXPECT_EQ(g_ioctlLastInfo.magic, TEST_SET_KILL_INFO_MAGIC);
    EXPECT_EQ(g_ioctlLastInfo.structSize, static_cast<unsigned int>(sizeof(TestKillInfo)));
    EXPECT_EQ(g_ioctlLastInfo.pid, TEST_APP_PID + 1);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_KILL_CGROUP);
    EXPECT_EQ(g_ioctlLastInfo.data.uid, static_cast<int>(TEST_APP_UID + 1));
    // Fields not set by InitKillInfo must stay zeroed, not carry over from the previous call
    EXPECT_EQ(g_ioctlLastInfo.data.adj, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.timestamp, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.callingPid, 0);
    EXPECT_EQ(sizeof(g_ioctlLastInfo.data.callingProcessName), CALLING_PROCESS_NAME_SIZE);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFirst, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamSecond, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamThird, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFourth, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFifth, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamSixth, 0);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamSeventh, 0);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_009
 * @tc.desc: InitKillInfo中memset_s失败时提前返回，magic/structSize等字段未被填充；
 *           当前实现仍会下发ioctl，恢复后可正常复用fd上报（覆盖memset_s失败分支）
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_009, TestSize.Level0)
{
    g_memsetSFail = true;

    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);

    // fd is acquired normally; InitKillInfo bails out before filling any field.
    // The downstream info content is indeterminate here, so it is not asserted.
    EXPECT_EQ(g_openSysloadCount, 1);
    EXPECT_EQ(g_ioctlCallCount, 1);

    // Recovering the mock restores normal reporting on the reused fd
    g_memsetSFail = false;
    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);
    EXPECT_EQ(g_ioctlCallCount, 2);
    EXPECT_EQ(g_openSysloadCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.magic, TEST_SET_KILL_INFO_MAGIC);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_APPSPAWN_STOP);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_010
 * @tc.desc: 队列中存在count=0的TYPE_KILL_REASON_FD节点时不复用其fds数组，
 *           GetKillReasonFd重新打开/dev/sysload并正常上报
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_010, TestSize.Level0)
{
    // RegisterSpawningFds rejects count=0, so hand-craft the node to reach that branch
    AppSpawnFds *emptyNode = static_cast<AppSpawnFds *>(calloc(1, sizeof(AppSpawnFds)));
    ASSERT_NE(emptyNode, nullptr);
    OH_ListInit(&emptyNode->node);
    emptyNode->type = TYPE_KILL_REASON_FD;
    emptyNode->count = 0;
    emptyNode->pid = getpid();
    OH_ListAddTail(&mgr_->spawningFdsQueue, &emptyNode->node);

    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);

    // The count=0 node is matched but must not be reused as an fd source
    EXPECT_EQ(g_openSysloadCount, 1);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_GE(g_ioctlLastFd, 0);
    EXPECT_EQ(g_ioctlLastInfo.magic, TEST_SET_KILL_INFO_MAGIC);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_APPSPAWN_STOP);
    // emptyNode is released by DeleteAppSpawnMgr in TearDown
}

/**
 * @tc.name: App_Spawn_KillReason_InitKillInfo_001
 * @tc.desc: InitKillInfo入参info为NULL时提前返回，不解引用空指针也不崩溃，
 *           且不产生任何ioctl下发
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_InitKillInfo_001, TestSize.Level0)
{
    InitKillInfo(nullptr, TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);

    EXPECT_EQ(g_ioctlCallCount, 0);
    EXPECT_EQ(g_openSysloadCount, 0);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_006
 * @tc.desc: KillReasonReportHook直接被调用且appInfo为NULL时返回0，不解引用空指针也不上报。
 *           正常路径下该入参已由ProcessMgrHookExecute拦截，此处直调以覆盖hook自身的防御分支
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_006, TestSize.Level0)
{
    EXPECT_EQ(KillReasonReportHook(mgr_, nullptr), 0);

    EXPECT_EQ(g_ioctlCallCount, 0);
    EXPECT_EQ(g_openSysloadCount, 0);
}

/**
 * @tc.name: App_Spawn_KillReason_Set_011
 * @tc.desc: SetKillReason携带非零errorCause时，errorCause被填充到KillInfo.data.eventParamFirst，
 *           同时reason/pid/uid等字段正确，验证errorCause参数端到端传播
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_011, TestSize.Level0)
{
    constexpr int testErrorCause = 9;  // e.g. SIGKILL
    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_SIGNAL_KILL, testErrorCause);

    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_SIGNAL_KILL);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFirst, testErrorCause);
    EXPECT_EQ(g_ioctlLastInfo.data.pid, TEST_APP_PID);
    EXPECT_EQ(g_ioctlLastInfo.data.uid, static_cast<int>(TEST_APP_UID));
}

/**
 * @tc.name: App_Spawn_KillReason_Set_012
 * @tc.desc: SetKillReason连续两次携带不同errorCause，第二次的eventParamFirst不残留第一次的值，
 *           KillInfo通过={0}初始化保证每次干净填充
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Set_012, TestSize.Level0)
{
    SetKillReason(mgr_, TEST_APP_PID, TEST_APP_UID, REASON_SIGNAL_KILL, 9);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFirst, 9);

    SetKillReason(mgr_, TEST_APP_PID + 1, TEST_APP_UID, REASON_SIGNAL_EXIT, 42);
    EXPECT_EQ(g_ioctlCallCount, 2);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFirst, 42);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_SIGNAL_EXIT);
}

/**
 * @tc.name: App_Spawn_KillReason_InitKillInfo_002
 * @tc.desc: InitKillInfo携带非零errorCause时，eventParamFirst被正确填充为errorCause值，
 *           其余字段（magic/pid/uid/id/structSize）同时正确
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_InitKillInfo_002, TestSize.Level0)
{
    TestKillInfo info = {};
    InitKillInfo(reinterpret_cast<struct KillInfo *>(&info), TEST_APP_PID, TEST_APP_UID, REASON_SIGNAL_KILL, 15);

    EXPECT_EQ(info.magic, TEST_SET_KILL_INFO_MAGIC);
    EXPECT_EQ(info.pid, TEST_APP_PID);
    EXPECT_EQ(info.data.id, REASON_SIGNAL_KILL);
    EXPECT_EQ(info.data.uid, static_cast<int>(TEST_APP_UID));
    EXPECT_EQ(info.data.eventParamFirst, 15);
    EXPECT_EQ(info.structSize, static_cast<unsigned int>(sizeof(TestKillInfo)));
}

/**
 * @tc.name: App_Spawn_KillReason_InitKillInfo_003
 * @tc.desc: InitKillInfo携带errorCause=0时，eventParamFirst为0，验证默认值填充正确
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_InitKillInfo_003, TestSize.Level0)
{
    TestKillInfo info = {};
    // poison eventParamFirst to ensure InitKillInfo overwrites it with 0
    info.data.eventParamFirst = -1;
    InitKillInfo(reinterpret_cast<struct KillInfo *>(&info), TEST_APP_PID, TEST_APP_UID, REASON_APPSPAWN_STOP, 0);

    EXPECT_EQ(info.data.eventParamFirst, 0);
    EXPECT_EQ(info.data.id, REASON_APPSPAWN_STOP);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_007
 * @tc.desc: KillReasonReportHook携带非零errorCause时，errorCause端到端传播到ioctl下发的KillInfo，
 *           验证appInfo->killReason.errorCause正确传递到eventParamFirst
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_007, TestSize.Level0)
{
    AppSpawnedProcess *appInfo =
        CreateTestAppInfo("kill-reason-test-007", TEST_APP_PID, TEST_APP_UID, REASON_SIGNAL_KILL);
    ASSERT_NE(appInfo, nullptr);
    appInfo->killReason.errorCause = 9;  // SIGKILL

    int ret = ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, reinterpret_cast<AppSpawnContent *>(mgr_), appInfo);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_SIGNAL_KILL);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFirst, 9);

    free(appInfo);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_008
 * @tc.desc: KillReasonReportHook在reason=0但errorCause非零时仍不上报（reason=0为守卫条件），
 *           验证errorCause不影响reason=0的短路逻辑
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_008, TestSize.Level0)
{
    AppSpawnedProcess *appInfo =
        CreateTestAppInfo("kill-reason-test-008", TEST_APP_PID, TEST_APP_UID, 0);
    ASSERT_NE(appInfo, nullptr);
    appInfo->killReason.errorCause = 99;  // non-zero errorCause but reason=0

    int ret = ProcessMgrHookExecute(STAGE_SERVER_APP_CLEANUP, reinterpret_cast<AppSpawnContent *>(mgr_), appInfo);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(g_ioctlCallCount, 0);
    EXPECT_EQ(g_openSysloadCount, 0);

    free(appInfo);
}

/**
 * @tc.name: App_Spawn_KillReason_Hook_009
 * @tc.desc: KillReasonReportHook直调且reason非零errorCause非零时返回0并上报，
 *           与Hook_006的NULL入参形成互补，覆盖hook正常路径直调场景
 * @tc.type: FUNC
 * @tc.level: Level0
 * @tc.require: Kill reason report
 */
HWTEST_F(AppSpawnKillReasonTest, App_Spawn_KillReason_Hook_009, TestSize.Level0)
{
    AppSpawnedProcess *appInfo =
        CreateTestAppInfo("kill-reason-test-009", TEST_APP_PID, TEST_APP_UID, REASON_KILL_CGROUP);
    ASSERT_NE(appInfo, nullptr);
    appInfo->killReason.errorCause = 7;

    EXPECT_EQ(KillReasonReportHook(mgr_, appInfo), 0);
    EXPECT_EQ(g_ioctlCallCount, 1);
    EXPECT_EQ(g_ioctlLastInfo.data.id, REASON_KILL_CGROUP);
    EXPECT_EQ(g_ioctlLastInfo.data.eventParamFirst, 7);

    free(appInfo);
}

}  // namespace OHOS
