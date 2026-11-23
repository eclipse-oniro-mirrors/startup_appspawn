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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_spawn_appenv_test_helper.h"
#include "app_spawn_test_helper.h"
#include "appspawn_adapter.h"
#include "appspawn.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_modulemgr.h"
#include "appspawn_server.h"
#include "appspawn_service.h"
#include "appspawn_utils.h"
#include "securec.h"

/* Production functions under test. InitAppCommonEnv / SetUserLevelEnvConfig
 * are declared in appspawn_adapter.h. SpawnEnsureAppenvDir / SpawnSetAppEnv
 * are APPSPAWN_STATIC (empty under APPSPAWN_TEST), so extern "C" reaches them. */
extern "C" {
int SpawnEnsureAppenvDir(AppSpawnMgr *content, AppSpawningCtx *property);
int SpawnSetAppEnv(AppSpawnMgr *content, AppSpawningCtx *property);
}

using namespace testing;
using namespace testing::ext;

namespace {
/* UID_BASE is defined as 200000 in util/include/appspawn_utils.h (included
 * above). InitAppCommonEnv derives userId = appUid / UID_BASE.
 * appUid 20010029 -> userId 100. */
constexpr uint32_t TEST_APPUID_USER100 = 20010029;  // userId = 100
constexpr const char *TEST_USERID = "100";
/* Distinct userId for the SpawnEnsureAppenvDir dedup/idempotency test: the
 * in-process static dedup array inside SpawnEnsureAppenvDir persists across
 * tests, so this test must use a userId not seen by earlier SpawnEnsureAppenvDir
 * calls to exercise the first-call (create) + second-call (dedupe) flow. */
constexpr const char *TEST_USERID_DEDUP = "101";
constexpr const char *TEST_PROCESS_NAME = "com.example.appenvtest";

/* Production constants mirrored here for assertions (defined static in
 * appspawn_common.c, not exported). */
constexpr uid_t APPENV_DIR_OWNER_UID = 1102;  // deviceinfo
constexpr gid_t APPENV_DIR_OWNER_GID = 1102;  // deviceinfo

void UnsetEnvIfPresent(const char *name)
{
    if (getenv(name) != nullptr) {
        (void)unsetenv(name);
    }
}
}  // namespace

namespace OHOS {
static AppenvTestHelper g_appenvHelper;

class AppSpawnAppenvTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " start";
        /* Avoid cross-test leakage of USER and the keys used in appenv.conf. */
        UnsetEnvIfPresent("USER");
        UnsetEnvIfPresent("APPENV_TEST_KEY");
        UnsetEnvIfPresent("APPENV_OVERRIDE_KEY");
        UnsetEnvIfPresent("APPENV_APP_KEY");
        g_appenvHelper.CleanupAppenv(TEST_USERID);
        g_appenvHelper.CleanupAppenv(TEST_USERID_DEDUP);
    }
    void TearDown()
    {
        g_appenvHelper.CleanupAppenv(TEST_USERID);
        g_appenvHelper.CleanupAppenv(TEST_USERID_DEDUP);
        UnsetEnvIfPresent("USER");
        UnsetEnvIfPresent("APPENV_TEST_KEY");
        UnsetEnvIfPresent("APPENV_OVERRIDE_KEY");
        UnsetEnvIfPresent("APPENV_APP_KEY");
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " end";
    }
};

/*
 * @tc.name: AppSpawn_Appenv_InitAppCommonEnv_001
 * @tc.desc: InitAppCommonEnv derives userId = appUid / UID_BASE and setenv
 *           USER=<userId>. appUid 20010029 -> USER=100.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_InitAppCommonEnv_001, TestSize.Level0)
{
    UnsetEnvIfPresent("USER");
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, false, true, true);
    ASSERT_NE(property, nullptr);

    InitAppCommonEnv(property);
    const char *user = getenv("USER");
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(strcmp(user, "100"), 0);  // 20010029 / 200000 = 100

    DeleteAppSpawningCtx(property);
}

/*
 * @tc.name: AppSpawn_Appenv_InitAppCommonEnv_002
 * @tc.desc: InitAppCommonEnv tolerates a NULL property — GetAppProperty
 *           returns NULL, appInfo is NULL, early return. No crash, USER
 *           unchanged. (The "DAC TLV absent" branch inside a valid property
 *           is not reachable via the client builder, which always adds DacInfo;
 *           the NULL-property path exercises the same early-return guard.)
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_InitAppCommonEnv_002, TestSize.Level0)
{
    UnsetEnvIfPresent("USER");
    InitAppCommonEnv(nullptr);  // must not crash; USER unchanged
    EXPECT_EQ(getenv("USER"), nullptr);
}

/*
 * @tc.name: AppSpawn_Appenv_SetUserLevelEnvConfig_001
 * @tc.desc: SetUserLevelEnvConfig loads KEY=VALUE lines from appenv.conf into
 *           the process environment. Requires CUSTOM_SANDBOX compile flag and
 *           APP_FLAGS_CUSTOM_SANDBOX + appspawn mode.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SetUserLevelEnvConfig_001, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    ASSERT_EQ(g_appenvHelper.WriteAppenvConf(TEST_USERID, "APPENV_TEST_KEY=user_level_value\n"),
        0);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, true, true, true);
    ASSERT_NE(property, nullptr);

    UnsetEnvIfPresent("APPENV_TEST_KEY");
    SetUserLevelEnvConfig(mgr, property);
    const char *val = getenv("APPENV_TEST_KEY");
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(strcmp(val, "user_level_value"), 0);

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    GTEST_LOG_(INFO) << "SetUserLevelEnvConfig is compiled only under CUSTOM_SANDBOX; skipped.";
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SetUserLevelEnvConfig_002
 * @tc.desc: SetUserLevelEnvConfig skips when not in appspawn mode
 *           (IsAppSpawnMode false, e.g. nwebspawn). No env is loaded even if
 *           appenv.conf exists.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SetUserLevelEnvConfig_002, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);  // not appspawn mode
    ASSERT_NE(mgr, nullptr);
    ASSERT_EQ(g_appenvHelper.WriteAppenvConf(TEST_USERID, "APPENV_TEST_KEY=should_not_load\n"),
        0);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, true, true, true);
    ASSERT_NE(property, nullptr);

    UnsetEnvIfPresent("APPENV_TEST_KEY");
    SetUserLevelEnvConfig(mgr, property);
    EXPECT_EQ(getenv("APPENV_TEST_KEY"), nullptr);

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SetUserLevelEnvConfig_003
 * @tc.desc: SetUserLevelEnvConfig skips when APP_FLAGS_CUSTOM_SANDBOX is not
 *           set, even in appspawn mode with appenv.conf present.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SetUserLevelEnvConfig_003, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    ASSERT_EQ(g_appenvHelper.WriteAppenvConf(TEST_USERID, "APPENV_TEST_KEY=should_not_load\n"),
        0);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, false, true, true);
    ASSERT_NE(property, nullptr);

    UnsetEnvIfPresent("APPENV_TEST_KEY");
    SetUserLevelEnvConfig(mgr, property);
    EXPECT_EQ(getenv("APPENV_TEST_KEY"), nullptr);

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SetUserLevelEnvConfig_004
 * @tc.desc: SetUserLevelEnvConfig skips when userId TLV is absent
 *           (withUserId=false). Logs warning, no crash.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SetUserLevelEnvConfig_004, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, nullptr, TEST_APPUID_USER100, true, false, true);
    ASSERT_NE(property, nullptr);

    UnsetEnvIfPresent("APPENV_TEST_KEY");
    SetUserLevelEnvConfig(mgr, property);
    EXPECT_EQ(getenv("APPENV_TEST_KEY"), nullptr);

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SetUserLevelEnvConfig_005
 * @tc.desc: SetUserLevelEnvConfig is a no-op (LOGW) when appenv.conf is
 *           missing. No env set, no crash.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SetUserLevelEnvConfig_005, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    /* No appenv.conf written. */
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, true, true, true);
    ASSERT_NE(property, nullptr);

    UnsetEnvIfPresent("APPENV_TEST_KEY");
    SetUserLevelEnvConfig(mgr, property);
    EXPECT_EQ(getenv("APPENV_TEST_KEY"), nullptr);

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SetUserLevelEnvConfig_006
 * @tc.desc: SetUserLevelEnvConfig ignores comment lines, blank lines, and
 *           malformed entries (no '=', empty key, empty value). Only valid
 *           KEY=VALUE lines are setenv'd.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SetUserLevelEnvConfig_006, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    std::string conf =
        "# comment line\n"
        "\n"
        "APPENV_TEST_KEY=valid_value\n"
        "=no_key_value\n"
        "NO_EQUAL_SIGN\n"
        "KEY_EMPTY=\n"
        "=EMPTY_BOTH\n";
    ASSERT_EQ(g_appenvHelper.WriteAppenvConf(TEST_USERID, conf), 0);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, true, true, true);
    ASSERT_NE(property, nullptr);

    UnsetEnvIfPresent("APPENV_TEST_KEY");
    UnsetEnvIfPresent("KEY_EMPTY");
    SetUserLevelEnvConfig(mgr, property);
    const char *val = getenv("APPENV_TEST_KEY");
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(strcmp(val, "valid_value"), 0);
    /* "KEY_EMPTY=" has pos+1 == size -> skipped (empty value not allowed). */
    EXPECT_EQ(getenv("KEY_EMPTY"), nullptr);

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SpawnEnsureAppenvDir_001
 * @tc.desc: SpawnEnsureAppenvDir creates /data/service/el1/<userId>/appenv
 *           with mode 0755 and owner deviceinfo:deviceinfo (1102:1102).
 *           Requires CUSTOM_SANDBOX (hook is registered only then).
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SpawnEnsureAppenvDir_001, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, true, true, true);
    ASSERT_NE(property, nullptr);
    g_appenvHelper.CleanupAppenv(TEST_USERID);  // ensure absent before test

    int ret = SpawnEnsureAppenvDir(mgr, property);
    EXPECT_EQ(ret, 0);

    std::string dir = std::string(AppenvTestHelper::APPENV_DIR_PREFIX) +
        TEST_USERID + AppenvTestHelper::APPENV_DIR_SUFFIX;
    struct stat st = {};
    ASSERT_TRUE(AppenvTestHelper::GetStat(dir.c_str(), st));
    EXPECT_TRUE(S_ISDIR(st.st_mode));
    /* On device the test runs as root, so owner is enforced to 1102:1102.
     * Mode bits: 0755 & ~umask. Assert the permission bits that survive umask
     * (owner rwx always present). */
    EXPECT_EQ(static_cast<mode_t>(st.st_mode & 0700), static_cast<mode_t>(0700));
    EXPECT_EQ(st.st_uid, APPENV_DIR_OWNER_UID);
    EXPECT_EQ(st.st_gid, APPENV_DIR_OWNER_GID);

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    GTEST_LOG_(INFO) << "SpawnEnsureAppenvDir is compiled only under CUSTOM_SANDBOX; skipped.";
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SpawnEnsureAppenvDir_002
 * @tc.desc: SpawnEnsureAppenvDir skips (returns 0) when not in appspawn mode.
 *           No dir is created.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SpawnEnsureAppenvDir_002, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_NWEB_SPAWN);  // not appspawn
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100, true, true, true);
    ASSERT_NE(property, nullptr);
    g_appenvHelper.CleanupAppenv(TEST_USERID);

    int ret = SpawnEnsureAppenvDir(mgr, property);
    EXPECT_EQ(ret, 0);
    std::string dir = std::string(AppenvTestHelper::APPENV_DIR_PREFIX) +
        TEST_USERID + AppenvTestHelper::APPENV_DIR_SUFFIX;
    EXPECT_FALSE(AppenvTestHelper::DirExists(dir.c_str()));

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SpawnEnsureAppenvDir_003
 * @tc.desc: SpawnEnsureAppenvDir skips when userId TLV is absent. No dir, no
 *           crash.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SpawnEnsureAppenvDir_003, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, nullptr, TEST_APPUID_USER100, true, false, true);
    ASSERT_NE(property, nullptr);
    g_appenvHelper.CleanupAppenv(TEST_USERID);

    int ret = SpawnEnsureAppenvDir(mgr, property);
    EXPECT_EQ(ret, 0);
    std::string dir = std::string(AppenvTestHelper::APPENV_DIR_PREFIX) +
        TEST_USERID + AppenvTestHelper::APPENV_DIR_SUFFIX;
    EXPECT_FALSE(AppenvTestHelper::DirExists(dir.c_str()));

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SpawnEnsureAppenvDir_004
 * @tc.desc: SpawnEnsureAppenvDir is idempotent — calling twice on the same
 *           userId succeeds (EEXIST tolerated) and leaves the dir intact.
 *           Also exercises the in-process dedupe (second call is a no-op).
 *           Uses a distinct userId (101) so the first call is NOT deduped by
 *           entries left over from test 001 (userId 100).
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SpawnEnsureAppenvDir_004, TestSize.Level0)
{
#ifdef CUSTOM_SANDBOX
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    /* Build with userId "101" — different from TEST_USERID to avoid the
     * process-level dedupe array left over from test 001. */
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID_DEDUP, TEST_APPUID_USER100, true, true, true);
    ASSERT_NE(property, nullptr);
    g_appenvHelper.CleanupAppenv(TEST_USERID_DEDUP);

    EXPECT_EQ(SpawnEnsureAppenvDir(mgr, property), 0);
    EXPECT_EQ(SpawnEnsureAppenvDir(mgr, property), 0);  // idempotent + dedupe
    std::string dir = std::string(AppenvTestHelper::APPENV_DIR_PREFIX) +
        TEST_USERID_DEDUP + AppenvTestHelper::APPENV_DIR_SUFFIX;
    EXPECT_TRUE(AppenvTestHelper::DirExists(dir.c_str()));

    DeleteAppSpawningCtx(property);
    g_appenvHelper.CleanupAppenv(TEST_USERID_DEDUP);
    DeleteAppSpawnMgr(mgr);
#else
    SUCCEED();
#endif
}

/*
 * @tc.name: AppSpawn_Appenv_SpawnSetAppEnv_Order_001
 * @tc.desc: SpawnSetAppEnv applies the documented order
 *           InitAppCommonEnv (USER) -> SetUserLevelEnvConfig (appenv.conf) ->
 *           SetEnvInfo (per-app AppEnv JSON, overrides user-level). When the
 *           same key is set by appenv.conf and AppEnv JSON, the AppEnv JSON
 *           value must win.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SpawnSetAppEnv_Order_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
#ifdef CUSTOM_SANDBOX
    ASSERT_EQ(g_appenvHelper.WriteAppenvConf(TEST_USERID, "APPENV_OVERRIDE_KEY=user_level\n"),
        0);
#endif

    /* Build a property carrying userId TLV + AppEnv JSON TLV + custom sandbox
     * flag via the shared AppSpawnTestHelper (AppenvTestHelper.BuildProperty
     * does not add the AppEnv JSON TLV). */
    AppSpawnClientHandle clientHandle = nullptr;
    ASSERT_EQ(AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle), 0);
    AppSpawnReqMsgHandle reqHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        AppSpawnTestHelper helper;
        helper.SetProcessName(TEST_PROCESS_NAME);
        helper.SetTestUid(TEST_APPUID_USER100);
        helper.SetTestGid(TEST_APPUID_USER100);
        reqHandle = helper.CreateMsg(clientHandle, MSG_APP_SPAWN, 0);
        ASSERT_NE(reqHandle, nullptr);
        ASSERT_EQ(AppSpawnReqMsgAddStringInfo(reqHandle, MSG_EXT_NAME_USERID, TEST_USERID), 0);
        const char *appEnvJson = "{\"APPENV_OVERRIDE_KEY\":\"app_json_wins\","
                                 "\"APPENV_APP_KEY\":\"app_value\"}";
        ASSERT_EQ(AppSpawnReqMsgAddStringInfo(reqHandle, MSG_EXT_NAME_APP_ENV, appEnvJson), 0);
#ifdef CUSTOM_SANDBOX
        ASSERT_EQ(AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_CUSTOM_SANDBOX), 0);
#endif
        property = helper.GetAppProperty(clientHandle, reqHandle);
        ASSERT_NE(property, nullptr);
    } while (0);
    reqHandle = nullptr;

    UnsetEnvIfPresent("USER");
    UnsetEnvIfPresent("APPENV_OVERRIDE_KEY");
    UnsetEnvIfPresent("APPENV_APP_KEY");
    int ret = SpawnSetAppEnv(mgr, property);
    EXPECT_EQ(ret, 0);

    /* InitAppCommonEnv: USER = 100 (20010029 / 200000). */
    const char *user = getenv("USER");
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(strcmp(user, "100"), 0);
    /* SetEnvInfo (AppEnv JSON) overrides appenv.conf value. */
    const char *overrideVal = getenv("APPENV_OVERRIDE_KEY");
    ASSERT_NE(overrideVal, nullptr);
    EXPECT_EQ(strcmp(overrideVal, "app_json_wins"), 0);
    /* AppEnv-only key is set. */
    const char *appVal = getenv("APPENV_APP_KEY");
    ASSERT_NE(appVal, nullptr);
    EXPECT_EQ(strcmp(appVal, "app_value"), 0);

    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppSpawn_Appenv_SpawnSetAppEnv_NoAppEnv_001
 * @tc.desc: SpawnSetAppEnv succeeds when AppEnv JSON TLV is absent — SetEnvInfo
 *           returns 0 (size == 0 path). USER is still set by InitAppCommonEnv.
 *           Under CUSTOM_SANDBOX, appenv.conf values still apply.
 */
HWTEST_F(AppSpawnAppenvTest, AppSpawn_Appenv_SpawnSetAppEnv_NoAppEnv_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
#ifdef CUSTOM_SANDBOX
    ASSERT_EQ(g_appenvHelper.WriteAppenvConf(TEST_USERID, "APPENV_TEST_KEY=user_level_only\n"),
        0);
#endif
    AppSpawningCtx *property = g_appenvHelper.BuildProperty(
        TEST_PROCESS_NAME, TEST_USERID, TEST_APPUID_USER100,
#ifdef CUSTOM_SANDBOX
        true,
#else
        false,
#endif
        true, true);
    ASSERT_NE(property, nullptr);

    UnsetEnvIfPresent("USER");
    UnsetEnvIfPresent("APPENV_TEST_KEY");
    int ret = SpawnSetAppEnv(mgr, property);
    EXPECT_EQ(ret, 0);
    const char *user = getenv("USER");
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(strcmp(user, "100"), 0);
#ifdef CUSTOM_SANDBOX
    const char *val = getenv("APPENV_TEST_KEY");
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(strcmp(val, "user_level_only"), 0);
#endif

    DeleteAppSpawningCtx(property);
    DeleteAppSpawnMgr(mgr);
}

}  // namespace OHOS
