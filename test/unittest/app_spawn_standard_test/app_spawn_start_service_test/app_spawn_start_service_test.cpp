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

#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <gtest/gtest.h>

#include "appspawn_manager.h"
#include "appspawn_modulemgr.h"
#include "appspawn_service.h"
#include "parameter.h"
#include "securec.h"

using namespace testing;
using namespace testing::ext;

#ifdef __cplusplus
extern "C" {
#endif

APPSPAWN_STATIC int SetParameterWithRetry(const char *name, const char *value);

int RestoreconRecurse(const char *path)
{
    (void)path;
    return 0;
}

void __gcov_dump(void) __attribute__((weak));

int __real_SetParameter(const char *key, const char *value);

LoopHandle __real_LE_GetDefaultLoop(void);

#ifdef __cplusplus
}
#endif

namespace OHOS {
static int g_utSetParamFailFirst = 0;

extern "C" int __wrap_SetParameter(const char *key, const char *value)
{
    if (g_utSetParamFailFirst > 0 && strcmp(key, "appspawn.ut.retry.flaky") == 0) {
        g_utSetParamFailFirst--;
        return -1;
    }
    return __real_SetParameter(key, value);
}

static int g_utLoopNullArm = 0;

extern "C" LoopHandle __wrap_LE_GetDefaultLoop(void)
{
    if (g_utLoopNullArm > 0) {
        g_utLoopNullArm--;
        return NULL;
    }
    return __real_LE_GetDefaultLoop();
}

class AppSpawnServiceParamTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() {}
    void TearDown() {}
};

HWTEST_F(AppSpawnServiceParamTest, SetParameterWithRetry_001, TestSize.Level0)
{
    int ret = SetParameter("appspawn.ut.probe", "1");
    if (ret != 0) {
        GTEST_SKIP() << "no permission to set system parameter, ret: " << ret;
    }
    EXPECT_EQ(SetParameterWithRetry("appspawn.ut.retry.check", "true"), 0);
    char value[32] = {0};
    GetParameter("appspawn.ut.retry.check", "", value, sizeof(value));
    EXPECT_STREQ(value, "true");
}

HWTEST_F(AppSpawnServiceParamTest, SetParameterWithRetry_002, TestSize.Level0)
{
    EXPECT_NE(SetParameterWithRetry("%%invalid.name%%", "x"), 0);
}

HWTEST_F(AppSpawnServiceParamTest, SetParameterWithRetry_003, TestSize.Level0)
{
    int ret = SetParameter("appspawn.ut.probe", "1");
    if (ret != 0) {
        GTEST_SKIP() << "no permission to set system parameter, ret: " << ret;
    }
    EXPECT_EQ(SetParameterWithRetry("appspawn.ut.retry.value", "42"), 0);
    char value[32] = {0};
    GetParameter("appspawn.ut.retry.value", "", value, sizeof(value));
    EXPECT_STREQ(value, "42");
}

HWTEST_F(AppSpawnServiceParamTest, SetParameterWithRetry_004, TestSize.Level0)
{
    EXPECT_NE(SetParameterWithRetry(std::string(120, 'n').c_str(), "x"), 0);
}

HWTEST_F(AppSpawnServiceParamTest, SetParameterWithRetry_005, TestSize.Level0)
{
    EXPECT_NE(SetParameterWithRetry("", "x"), 0);
}

HWTEST_F(AppSpawnServiceParamTest, SetParameterWithRetry_006, TestSize.Level0)
{
    int ret = SetParameter("appspawn.ut.probe", "1");
    if (ret != 0) {
        GTEST_SKIP() << "no permission to set system parameter, ret: " << ret;
    }
    g_utSetParamFailFirst = 1;
    EXPECT_EQ(SetParameterWithRetry("appspawn.ut.retry.flaky", "true"), 0);
    EXPECT_EQ(g_utSetParamFailFirst, 0);
    char value[32] = {0};
    GetParameter("appspawn.ut.retry.flaky", "", value, sizeof(value));
    EXPECT_STREQ(value, "true");
}

#define UT_CHILD_ABORT_EXIT 134

static void ChildAbortDumpHandler(int sig)
{
    (void)sig;
    if (__gcov_dump != nullptr) {
        __gcov_dump();
    }
    _exit(UT_CHILD_ABORT_EXIT);
}

class CheckSingleThreadTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() {}
    void TearDown() {}
};

HWTEST_F(CheckSingleThreadTest, CheckSingleThread_001, TestSize.Level0)
{
    DIR *dir = opendir("/proc/self/task");
    ASSERT_NE(dir, nullptr);
    int count = 0;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }
    closedir(dir);
    ASSERT_EQ(count, 1);
    CheckSingleThread();
    SUCCEED();
}

HWTEST_F(CheckSingleThreadTest, CheckSingleThread_002, TestSize.Level0)
{
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        std::thread t([]() { while (true) { sleep(1); } });
        t.detach();
        CheckSingleThread();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

HWTEST_F(CheckSingleThreadTest, CheckSingleThread_003, TestSize.Level0)
{
    struct rlimit rl = {};
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &rl), 0);
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 1024 * 1024) {
        GTEST_SKIP() << "fd limit too high to exhaust: " << rl.rlim_cur;
    }
    std::vector<int> fds;
    while (fds.size() < rl.rlim_cur) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            break;
        }
        fds.push_back(fd);
        DIR *dir = opendir("/proc/self/task");
        if (dir == nullptr) {
            break;
        }
        closedir(dir);
    }
    DIR *dir = opendir("/proc/self/task");
    ASSERT_EQ(dir, nullptr) << "fd exhaustion not achieved, limit: " << rl.rlim_cur;
    CheckSingleThread();
    for (int fd : fds) {
        close(fd);
    }
    dir = opendir("/proc/self/task");
    ASSERT_NE(dir, nullptr);
    closedir(dir);
}

HWTEST_F(CheckSingleThreadTest, CheckSingleThread_004, TestSize.Level0)
{
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        signal(SIGABRT, ChildAbortDumpHandler);
        std::thread t([]() { while (true) { sleep(1); } });
        t.detach();
        prctl(PR_SET_NAME, "ut_short_name");
        CheckSingleThread();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), UT_CHILD_ABORT_EXIT);
}

class AppSpawnStartServiceTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
        if (GetAppSpawnMgr() != nullptr) {
            DeleteAppSpawnMgr(GetAppSpawnMgr());
        }
    }
    void TearDown()
    {
        if (GetAppSpawnMgr() != nullptr) {
            DeleteAppSpawnMgr(GetAppSpawnMgr());
        }
    }
};

static int g_utPreloadFailCount = 0;

static int UtPreloadFailHook(AppSpawnMgr *content)
{
    (void)content;
    if (g_utPreloadFailCount > 0) {
        g_utPreloadFailCount--;
        return -1;
    }
    return 0;
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_InvalidArgs_001, TestSize.Level0)
{
    EXPECT_EQ(StartSpawnService(nullptr, 256, 1, nullptr), nullptr);

    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_invalid";
    arg.serviceName = "ut_sts_invalid_svc";
    arg.initArg = 0;
    EXPECT_EQ(StartSpawnService(&arg, 256, 1, nullptr), nullptr);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_InitArgCopy_002, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_copy";
    arg.serviceName = "ut_sts_copy_svc";
    arg.initArg = 1;
    char procName[256] = {0};
    (void)strcpy_s(procName, sizeof(procName), "placeholder");
    char *argv[] = {procName, nullptr};
    AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
    ASSERT_NE(content, nullptr);
    EXPECT_STREQ(procName, "ut_sts_copy_svc");
    EXPECT_EQ(content->mode, MODE_FOR_APP_SPAWN);
    EXPECT_NE(((AppSpawnMgr *)content)->server, nullptr);
    AppSpawnDestroyContent(content);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_InitArgSkip_003, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_skip";
    arg.serviceName = "ut_sts_skip_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    (void)strcpy_s(procName, sizeof(procName), "keep_me");
    char *argv[] = {procName, nullptr};
    AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
    ASSERT_NE(content, nullptr);
    EXPECT_STREQ(procName, "keep_me");
    EXPECT_EQ(content->mode, MODE_FOR_APP_SPAWN);
    EXPECT_NE(((AppSpawnMgr *)content)->server, nullptr);
    AppSpawnDestroyContent(content);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_NullProcName_004, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_nullname";
    arg.serviceName = "ut_sts_nullname_svc";
    arg.initArg = 1;
    char *argv[] = {nullptr, nullptr};
    EXPECT_EQ(StartSpawnService(&arg, 256, 2, argv), nullptr);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_NameTooLong_005, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_longname";
    arg.serviceName = "ut_sts_service_name_too_long_for_argv_buffer";
    arg.initArg = 1;
    char procName[16] = {0};
    char *argv[] = {procName, nullptr};
    EXPECT_EQ(StartSpawnService(&arg, sizeof(procName), 2, argv), nullptr);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_SocketPathTooLong_006, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    std::string longSocket(150, 'x');
    arg.socketName = longSocket.c_str();
    arg.serviceName = "ut_sts_longsock_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    char *argv[] = {procName, nullptr};
    EXPECT_EQ(StartSpawnService(&arg, sizeof(procName), 2, argv), nullptr);
    EXPECT_EQ(GetAppSpawnMgr(), nullptr);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_PreloadFail_007, TestSize.Level0)
{
    static bool hookAdded = false;
    if (!hookAdded) {
        (void)AddServerStageHook(STAGE_SERVER_PRELOAD, HOOK_PRIO_HIGHEST, UtPreloadFailHook);
        hookAdded = true;
    }
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_preload";
    arg.serviceName = "ut_sts_preload_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    char *argv[] = {procName, nullptr};
    g_utPreloadFailCount = 1;
    EXPECT_EQ(StartSpawnService(&arg, sizeof(procName), 2, argv), nullptr);
    EXPECT_EQ(g_utPreloadFailCount, 0);
    EXPECT_EQ(GetAppSpawnMgr(), nullptr);

    AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
    ASSERT_NE(content, nullptr);
    EXPECT_NE(((AppSpawnMgr *)content)->server, nullptr);
    AppSpawnDestroyContent(content);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_NoBooteventNweb_008, TestSize.Level0)
{
    int ret = SetParameter("bootevent.appspawn.started", "false");
    if (ret != 0) {
        GTEST_SKIP() << "no permission to set bootevent param, ret: " << ret;
    }
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_NWEB_SPAWN;
    arg.moduleType = MODULE_NWEBSPAWN;
    arg.socketName = "ut_sts_nweb";
    arg.serviceName = "ut_sts_nweb_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    char *argv[] = {procName, nullptr};
    AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(content->mode, MODE_FOR_NWEB_SPAWN);
    char value[32] = {0};
    GetParameter("bootevent.appspawn.started", "", value, sizeof(value));
    EXPECT_STREQ(value, "false");
    AppSpawnDestroyContent(content);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_BooteventAppSpawn_009, TestSize.Level0)
{
    int ret = SetParameter("bootevent.appspawn.started", "false");
    if (ret != 0) {
        GTEST_SKIP() << "no permission to set bootevent param, ret: " << ret;
    }
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_boot";
    arg.serviceName = "ut_sts_boot_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    char *argv[] = {procName, nullptr};
    AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(content->mode, MODE_FOR_APP_SPAWN);
    char value[32] = {0};
    GetParameter("bootevent.appspawn.started", "", value, sizeof(value));
    EXPECT_STREQ(value, "true");
    AppSpawnDestroyContent(content);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_ColdRun_010, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_COLD_RUN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_cold";
    arg.serviceName = "ut_sts_cold_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    char *argv[] = {procName, nullptr};
    AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(content->mode, MODE_FOR_APP_COLD_RUN);
    EXPECT_EQ(((AppSpawnMgr *)content)->server, nullptr);
    AppSpawnDestroyContent(content);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_MultiMode_011, TestSize.Level0)
{
    struct ModeCase {
        RunMode mode;
        uint32_t moduleType;
        const char *socketName;
    };
    const ModeCase cases[] = {
        {MODE_FOR_APP_SPAWN, MODULE_APPSPAWN, "ut_sts_mode_app"},
        {MODE_FOR_NWEB_SPAWN, MODULE_NWEBSPAWN, "ut_sts_mode_nweb"},
        {MODE_FOR_NATIVE_SPAWN, MODULE_NATIVESPAWN, "ut_sts_mode_native"},
        {MODE_FOR_HYBRID_SPAWN, MODULE_HYBRIDSPAWN, "ut_sts_mode_hybrid"},
    };
    for (const auto &c : cases) {
        AppSpawnStartArg arg = {};
        arg.mode = c.mode;
        arg.moduleType = c.moduleType;
        arg.socketName = c.socketName;
        arg.serviceName = "ut_sts_mode_svc";
        arg.initArg = 0;
        char procName[256] = {0};
        char *argv[] = {procName, nullptr};
        AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
        ASSERT_NE(content, nullptr) << "mode " << c.mode;
        EXPECT_EQ(content->mode, c.mode);
        EXPECT_NE(((AppSpawnMgr *)content)->server, nullptr);
        AppSpawnDestroyContent(content);
    }
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_InvalidMode_012, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_INVALID;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_badmode";
    arg.serviceName = "ut_sts_badmode_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    char *argv[] = {procName, nullptr};
    EXPECT_EQ(StartSpawnService(&arg, sizeof(procName), 2, argv), nullptr);
    EXPECT_EQ(GetAppSpawnMgr(), nullptr);
}

HWTEST_F(AppSpawnStartServiceTest, StartSpawnService_NoDefaultLoop_013, TestSize.Level0)
{
    AppSpawnStartArg arg = {};
    arg.mode = MODE_FOR_APP_SPAWN;
    arg.moduleType = MODULE_APPSPAWN;
    arg.socketName = "ut_sts_noloop";
    arg.serviceName = "ut_sts_noloop_svc";
    arg.initArg = 0;
    char procName[256] = {0};
    char *argv[] = {procName, nullptr};
    g_utLoopNullArm = 1;
    EXPECT_EQ(StartSpawnService(&arg, sizeof(procName), 2, argv), nullptr);
    EXPECT_EQ(g_utLoopNullArm, 0);

    AppSpawnContent *content = StartSpawnService(&arg, sizeof(procName), 2, argv);
    ASSERT_NE(content, nullptr);
    EXPECT_NE(((AppSpawnMgr *)content)->server, nullptr);
    AppSpawnDestroyContent(content);
}
}  // namespace OHOS
