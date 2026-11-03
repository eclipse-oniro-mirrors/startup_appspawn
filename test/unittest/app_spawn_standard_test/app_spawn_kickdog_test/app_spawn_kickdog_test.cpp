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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "appspawn_kickdog.h"
#include "securec.h"
#include "server_test_helper.h"

#ifdef __cplusplus
extern "C" {
#endif
 	 
int SpawnKickDogStart(AppSpawnMgr *mgrContent);
APPSPAWN_STATIC int OpenAndWriteToProc(const char *procName, const char *writeStr, size_t writeLen);
APPSPAWN_STATIC const char *GetProcFile(bool isLinux);
APPSPAWN_STATIC const char *GetProcContent(bool isLinux, bool isOpen, int mode);
APPSPAWN_STATIC int CheckKernelType(bool *isLinux);
 	 
#ifdef __cplusplus
}
#endif

using namespace testing;
using namespace testing::ext;

namespace OHOS {
class AppSpawnKickDogTest : public testing::Test {
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

static int CheckFileContent(const char *filePath, const char *targetStr)
{
    char buf[100];

    if (filePath == nullptr || targetStr == nullptr) {
        printf("para invalid\n");
        return -1;
    }

    int fileFd = open(filePath, O_RDWR);
    if (fileFd == -1) {
        printf("open %s fail,errno:%d\n", filePath, errno);
        return fileFd;
    }

    int readResult = read(fileFd, buf, sizeof(buf) - 1);
    if (readResult <= 0) {
        printf("read %s fail,result:%d\n", filePath, readResult);
        close(fileFd);
        return -1;
    }

    if (strcmp(buf, targetStr) != 0) {
        printf("read buf %s is not euqal target str:%s\n", buf, targetStr);
        close(fileFd);
        return -1;
    } else {
        printf("read buf %s is euqal to target str:%s\n", buf, targetStr);
        ftruncate(fileFd, 0);
    }

    close(fileFd);
    return 0;
}

static bool CheckDeviceInLinux()
{
    struct utsname uts;
    if (uname(&uts) == -1) {
        printf("uname get failed");
        return false;
    }
    if (strcmp(uts.sysname, "Linux") == 0) {
        printf("uname sysname is Linux");
        return true;
    }
    return false;
}

/**
 * @brief watchdog开启及定时kickdog
 *
 */
HWTEST_F(AppSpawnKickDogTest, App_Spawn_AppSpawnKickDog_001, TestSize.Level0)
{
    int fileFd = open(HM_APPSPAWN_WATCHDOG_FILE, O_CREAT);
    EXPECT_EQ(fileFd != -1, 1);
    close(fileFd);

    std::unique_ptr<OHOS::ServerTestHelper> testServer =
        std::make_unique<OHOS::ServerTestHelper>("appspawn -mode appspawn");
    AddPreloadHook(HOOK_PRIO_COMMON, SpawnKickDogStart);
    testServer->Start(nullptr);
    if (CheckDeviceInLinux()) {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_ON), 0);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_KICK), -1);
    } else {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_APPSPAWN_WATCHDOG_ON), 0);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_APPSPAWN_WATCHDOG_KICK), -1);
    }
    usleep(12 * 50 * 1000); // wait for kick dog(kick every 1 seconds)
    if (CheckDeviceInLinux()) {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_ON), -1);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_KICK), 0);
    } else {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_APPSPAWN_WATCHDOG_ON), -1);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_APPSPAWN_WATCHDOG_KICK), 0);
    }
    usleep(10 * 50 * 1000);
    if (CheckDeviceInLinux()) {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_KICK), 0);
    } else {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_APPSPAWN_WATCHDOG_KICK), 0);
    }
    testServer->Stop();
    EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, nullptr), -1);

    remove(HM_APPSPAWN_WATCHDOG_FILE);
}

/**
 * @brief nwebspawn接入hungtask定时kickdog
 *
 */
HWTEST_F(AppSpawnKickDogTest, App_Spawn_AppSpawnKickDog_002, TestSize.Level0)
{
    int fileFd = open(HM_APPSPAWN_WATCHDOG_FILE, O_CREAT);
    EXPECT_EQ(fileFd != -1, 1);
    close(fileFd);

    std::unique_ptr<OHOS::ServerTestHelper> testServer =
        std::make_unique<OHOS::ServerTestHelper>("nwebspawn -mode nwebspawn");
    AddPreloadHook(HOOK_PRIO_COMMON, SpawnKickDogStart);
    testServer->Start(nullptr);
    usleep(1 * 50 * 1000); // wait 50ms for writing kick log before reading
    if (CheckDeviceInLinux()) {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_ON), 0);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_KICK), -1);
    } else {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_NWEBSPAWN_WATCHDOG_ON), 0);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_NWEBSPAWN_WATCHDOG_KICK), -1);
    }
    usleep(12 * 50 * 1000); // wait for kick dog(kick every 1 seconds)
    if (CheckDeviceInLinux()) {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_ON), -1);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_KICK), 0);
    } else {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_NWEBSPAWN_WATCHDOG_ON), -1);
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_NWEBSPAWN_WATCHDOG_KICK), 0);
    }
    usleep(10 * 50 * 1000);
    if (CheckDeviceInLinux()) {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, LINUX_APPSPAWN_WATCHDOG_KICK), 0);
    } else {
        EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, HM_NWEBSPAWN_WATCHDOG_KICK), 0);
    }
    testServer->Stop();
    EXPECT_EQ(CheckFileContent(HM_APPSPAWN_WATCHDOG_FILE, nullptr), -1);

    remove(HM_APPSPAWN_WATCHDOG_FILE);
}

// ==================== Unit tests for static functions ====================

/**
 * @brief OpenAndWriteToProc with NULL writeStr
 *        Covers: line 22-24 (writeStr == NULL -> return -1)
 */
HWTEST_F(AppSpawnKickDogTest, OpenAndWriteToProc_NullWriteStr_001, TestSize.Level0)
{
    int ret = OpenAndWriteToProc("/tmp/test", nullptr, 0);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief OpenAndWriteToProc with non-existent file
 *        Covers: line 27-30 (open fails -> return -1)
 */
HWTEST_F(AppSpawnKickDogTest, OpenAndWriteToProc_OpenFail_001, TestSize.Level0)
{
    int ret = OpenAndWriteToProc("/tmp/nonexistent_proc_file", "test", 4);
    EXPECT_EQ(ret, -1);
}

/**
 * @brief GetProcFile with isLinux=true
 *        Covers: line 46-47 (isLinux -> LINUX_APPSPAWN_WATCHDOG_FILE)
 */
HWTEST_F(AppSpawnKickDogTest, GetProcFile_Linux_001, TestSize.Level0)
{
    const char *file = GetProcFile(true);
    EXPECT_STREQ(file, LINUX_APPSPAWN_WATCHDOG_FILE);
}

/**
 * @brief GetProcFile with isLinux=false
 *        Covers: line 49 (not linux -> HM_APPSPAWN_WATCHDOG_FILE)
 */
HWTEST_F(AppSpawnKickDogTest, GetProcFile_Hm_001, TestSize.Level0)
{
    const char *file = GetProcFile(false);
    EXPECT_STREQ(file, HM_APPSPAWN_WATCHDOG_FILE);
}

/**
 * @brief GetProcContent for all combinations
 *        Covers: line 54-70 (all branches)
 */
HWTEST_F(AppSpawnKickDogTest, GetProcContent_AllBranches_001, TestSize.Level0)
{
    // isOpen=true, isLinux=true -> LINUX_APPSPAWN_WATCHDOG_ON (line 55-56)
    EXPECT_STREQ(GetProcContent(true, true, MODE_FOR_APP_SPAWN), LINUX_APPSPAWN_WATCHDOG_ON);
    // isOpen=true, isLinux=false, NWEB -> HM_NWEBSPAWN_WATCHDOG_ON (line 58-59)
    EXPECT_STREQ(GetProcContent(false, true, MODE_FOR_NWEB_SPAWN), HM_NWEBSPAWN_WATCHDOG_ON);
    // isOpen=true, isLinux=false, HYBRID -> HM_HYBRIDSPAWN_WATCHDOG_ON (line 61)
    EXPECT_STREQ(GetProcContent(false, true, MODE_FOR_HYBRID_SPAWN), HM_HYBRIDSPAWN_WATCHDOG_ON);
    // isOpen=true, isLinux=false, APP -> HM_APPSPAWN_WATCHDOG_ON (line 61)
    EXPECT_STREQ(GetProcContent(false, true, MODE_FOR_APP_SPAWN), HM_APPSPAWN_WATCHDOG_ON);
    // isOpen=false, isLinux=true -> LINUX_APPSPAWN_WATCHDOG_KICK (line 64-65)
    EXPECT_STREQ(GetProcContent(true, false, MODE_FOR_APP_SPAWN), LINUX_APPSPAWN_WATCHDOG_KICK);
    // isOpen=false, isLinux=false, NWEB -> HM_NWEBSPAWN_WATCHDOG_KICK (line 67-68)
    EXPECT_STREQ(GetProcContent(false, false, MODE_FOR_NWEB_SPAWN), HM_NWEBSPAWN_WATCHDOG_KICK);
    // isOpen=false, isLinux=false, HYBRID -> HM_HYBRIDSPAWN_WATCHDOG_KICK (line 70)
    EXPECT_STREQ(GetProcContent(false, false, MODE_FOR_HYBRID_SPAWN), HM_HYBRIDSPAWN_WATCHDOG_KICK);
    // isOpen=false, isLinux=false, APP -> HM_APPSPAWN_WATCHDOG_KICK (line 70)
    EXPECT_STREQ(GetProcContent(false, false, MODE_FOR_APP_SPAWN), HM_APPSPAWN_WATCHDOG_KICK);
}

/**
 * @brief CheckKernelType - call on real device
 *        Covers: line 111-124 (uname, compare, set isLinux)
 *        On Linux the function should return 0 and set isLinux=true.
 */
HWTEST_F(AppSpawnKickDogTest, CheckKernelType_001, TestSize.Level0)
{
    bool isLinux = false;
    int ret = CheckKernelType(&isLinux);
    EXPECT_EQ(ret, 0);
    // On the Linux test environment, isLinux should be true
    EXPECT_TRUE(isLinux);
}

/**
 * @brief SpawnKickDogStart with native spawn mode (skip)
 *        Covers: line 130-131 (MODE_FOR_NATIVE_SPAWN -> return 0)
 */
HWTEST_F(AppSpawnKickDogTest, SpawnKickDogStart_NativeMode_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_NATIVE_SPAWN);
    ASSERT_NE(mgr, nullptr);
    int ret = SpawnKickDogStart(mgr);
    EXPECT_EQ(ret, 0);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @brief SpawnKickDogStart with invalid mode (skip)
 *        Covers: line 131-133 (not APP/NWEB/HYBRID -> return 0)
 */
HWTEST_F(AppSpawnKickDogTest, SpawnKickDogStart_InvalidMode_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_COLD_RUN);
    ASSERT_NE(mgr, nullptr);
    int ret = SpawnKickDogStart(mgr);
    EXPECT_EQ(ret, 0);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @brief SpawnKickDogStart with null content
 *        Covers: line 129 (mgrContent == NULL -> return 0)
 */
HWTEST_F(AppSpawnKickDogTest, SpawnKickDogStart_Null_001, TestSize.Level0)
{
    int ret = SpawnKickDogStart(nullptr);
    EXPECT_EQ(ret, 0);
}
}  // namespace OHOS
