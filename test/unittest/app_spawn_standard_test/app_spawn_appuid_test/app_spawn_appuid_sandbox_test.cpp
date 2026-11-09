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
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>

#include "app_spawn_appuid_test_helper.h"
#include "cJSON.h"
#include "sandbox_common.h"
#include "sandbox_core.h"
#include "sandbox_shared_mount.h"
#include "sandbox_dec.h"

using namespace testing::ext;
using OHOS::AppSpawn::SandboxCommon;
using OHOS::AppSpawn::SandboxCore;
using OHOS::AppUidTest::BuildAppUidProperty;
using OHOS::AppUidTest::CleanupAppUidProperty;

/* sandbox_shared_mount.cpp (APPSPAWN_STATIC is empty under APPSPAWN_TEST) */
bool IsUnlockStatus(uint32_t appUid);

namespace {

constexpr uint32_t CUSTOM_SANDBOX_UID = 100;           // userId-like 10x uid
constexpr uint32_t CUSTOM_SANDBOX_APPUID = 20010052;  // 200xxxxx -> userId 100
constexpr uint32_t NORMAL_APP_UID = 20010029;

class ScopedAppUidDirectory {
public:
    explicit ScopedAppUidDirectory(const std::string &parent)
    {
        std::string pattern = parent + "/appuid_ut_XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char *path = mkdtemp(buffer.data());
        if (path != nullptr) {
            path_ = path;
        }
    }
    ~ScopedAppUidDirectory()
    {
        if (!path_.empty()) {
            (void)rmdir((path_ + "/child").c_str());
            (void)rmdir(path_.c_str());
        }
    }
    const std::string &Path() const { return path_; }
private:
    std::string path_;
};

class AppSpawnAppUidSandboxTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override { AppUidMockReset(); }
    void TearDown() override
    {
        AppUidMockReset();
        g_lockBundleMap.clear();
    }
};

/*
 * @tc.name: AppUid_SandboxRootPath_CustomSandbox_001
 * @tc.desc: sandbox root path userId comes from appUid (20010052/200000=100),
 *           never the 10x uid (which would produce a wrong userId 0).
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_SandboxRootPath_CustomSandbox_001, TestSize.Level0)
{
    AppSpawningCtx *property = BuildAppUidProperty("com.test.sbxroot",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    std::string root = SandboxCommon::GetSandboxRootPath(property, nullptr);
    // userId comes from appUid: /mnt/sandbox/100/<bundle>_preunlock
    EXPECT_NE(root.find("/mnt/sandbox/100/"), std::string::npos);
    EXPECT_NE(root.find("com.test.sbxroot"), std::string::npos);
    EXPECT_NE(root.find("_preunlock"), std::string::npos);
    // the 10x uid must never leak into the path (100/200000 = 0)
    EXPECT_EQ(root.find("/mnt/sandbox/0/"), std::string::npos);

    CleanupAppUidProperty(property);
}

/*
 * @tc.name: AppUid_SandboxRootPath_Isolated_001
 * @tc.desc: isolate-user check - the isolated sandbox variant also
 *           derives its userId from appUid.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_SandboxRootPath_Isolated_001, TestSize.Level1)
{
    AppSpawningCtx *property = BuildAppUidProperty("com.test.isolated",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);
    (void)SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_ISOLATED_SANDBOX_TYPE);

    std::string root = SandboxCommon::GetSandboxRootPath(property, nullptr);
    EXPECT_NE(root.find("/mnt/sandbox/100/isolated/"), std::string::npos);

    CleanupAppUidProperty(property);
}

/*
 * @tc.name: AppUid_SandboxRootPath_Unlocked_001
 * @tc.desc: - unlocked status drops the _preunlock suffix, path userId
 *           still comes from appUid.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_SandboxRootPath_Unlocked_001, TestSize.Level2)
{
    AppSpawningCtx *property = BuildAppUidProperty("com.test.unlocked",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);
    (void)SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_UNLOCKED_STATUS);

    std::string root = SandboxCommon::GetSandboxRootPath(property, nullptr);
    EXPECT_NE(root.find("/mnt/sandbox/100/com.test.unlocked"), std::string::npos);
    EXPECT_EQ(root.find("_preunlock"), std::string::npos);

    CleanupAppUidProperty(property);
}

/*
 * @tc.name: AppUid_MountOptions_UserId_001
 * @tc.desc: mount options - user_id is derived from appUid/UID_BASE.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_MountOptions_UserId_001, TestSize.Level1)
{
    AppSpawningCtx *property = BuildAppUidProperty("com.test.options",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    cJSON *config = cJSON_Parse("{\"options\":\"ro\"}");
    ASSERT_NE(config, nullptr);
    std::string options = SandboxCommon::GetOptions(property, config);
    cJSON_Delete(config);

    EXPECT_NE(options.find("ro"), std::string::npos);
    EXPECT_NE(options.find("user_id=100"), std::string::npos);

    CleanupAppUidProperty(property);
}

/*
 * @tc.name: AppUid_FileOwner_ChownUsesAppUid_001
 * @tc.desc: MakeAtomicServiceDir chowns the created dir to appUid (200xxxxx).
 *           A "/data/service/el2" path avoids the WITH_SELINUX restorecon branch.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_FileOwner_ChownUsesAppUid_001, TestSize.Level0)
{
    AppSpawningCtx *property = BuildAppUidProperty("com.test.chown",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    ScopedAppUidDirectory directory("/data/service/el2");
    ASSERT_FALSE(directory.Path().empty());
    std::string path = directory.Path() + "/child";
    SandboxCommon::MakeAtomicServiceDir(property, path, "com.test.chown");

    ASSERT_GE(AppUidMockChownCallCount(), 1);
    AppUidChownCall call = {};
    EXPECT_EQ(AppUidMockGetChownCall(0, &call), 0);
    EXPECT_EQ(call.owner, CUSTOM_SANDBOX_APPUID);
    EXPECT_STREQ(call.path, path.c_str());

    CleanupAppUidProperty(property);
}

/*
 * @tc.name: AppUid_DlpFuse_Path_001
 * @tc.desc: HandleDlpMount prepares /mnt/data/<appUid/UID_BASE>/fuse and opens
 *           /dev/fuse (real mount is compiled out under APPSPAWN_TEST).
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_DlpFuse_Path_001, TestSize.Level1)
{
    AppDacInfo dacInfo = {};
    dacInfo.uid = CUSTOM_SANDBOX_UID;
    dacInfo.appUid = CUSTOM_SANDBOX_APPUID;
    dacInfo.gid = 20010029;

    ASSERT_EQ(SandboxCore::HandleDlpMount(&dacInfo), 0);
    EXPECT_EQ(AppUidMockFindOpenPath("/dev/fuse"), 1);
    EXPECT_EQ(AppUidMockFindUmountPath("/mnt/data/100/fuse"), 1);
    struct stat st = {};
    EXPECT_EQ(stat("/mnt/data/100/fuse", &st), 0);
}

/*
 * @tc.name: AppUid_DecDir_PolicyBuild_001
 * @tc.desc: DEC policy construction reaches the device writer. The current
 *           implementation does not consume userId, so this is not an identity check.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_DecDir_PolicyBuild_001, TestSize.Level2)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.decdir",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    // Keep the current argument contract; userId is unused by the implementation.
    AppDacInfo *dacInfo = reinterpret_cast<AppDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    ASSERT_NE(dacInfo, nullptr);
    uint32_t userId = dacInfo->appUid / UID_BASE;
    EXPECT_EQ(userId, 100u);
    EXPECT_EQ(SandboxCore::SetDecWithDir(property, userId), 0);
    AppUidMockReset();
    SetDecPolicy();
    EXPECT_EQ(AppUidMockFindOpenPath("/dev/dec"), 1);
    EXPECT_GT(AppUidMockRawIoctlCallCount(), 0);

    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_SharedMount_LockBundleRef_001
 * @tc.desc: shared mount - AddLockBundleRef records appUid (200xxxxx)
 *           in the lock bundle map keyed by the appUid-derived lock path.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_SharedMount_LockBundleRef_001, TestSize.Level0)
{
    AppSpawnMgr *mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
    ASSERT_NE(mgr, nullptr);
    AppSpawningCtx *property = BuildAppUidProperty("com.test.shared",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);
    AppUidMockSetLockStatus("1");
    EXPECT_EQ(MountToShared(mgr, property), 0);
    const std::string lockPath = "/mnt/sandbox/100/com.test.shared_preunlock";
    auto it = g_lockBundleMap.find(lockPath);
    ASSERT_NE(it, g_lockBundleMap.end());
    EXPECT_EQ(it->second.appUid, CUSTOM_SANDBOX_APPUID);
    EXPECT_EQ(it->second.refCount, 1u);
    EXPECT_TRUE(property->lockBundleRefAdded);
    EXPECT_STREQ(property->lockPath, lockPath.c_str());
    EXPECT_FALSE(CheckAppMsgFlagsSet(property, APP_FLAGS_UNLOCKED_STATUS));
    CleanupAppUidProperty(property);
    DeleteAppSpawnMgr(mgr);
}

/*
 * @tc.name: AppUid_SharedMount_IsUnlockStatus_001
 * @tc.desc: IsUnlockStatus derives userId from appUid: same-user appUids agree;
 *           the 10x uid by mistake collapses to userId 0 (always unlocked).
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_SharedMount_IsUnlockStatus_001, TestSize.Level1)
{
    AppUidMockSetLockStatus("1");
    EXPECT_FALSE(IsUnlockStatus(CUSTOM_SANDBOX_APPUID));
    EXPECT_TRUE(IsUnlockStatus(CUSTOM_SANDBOX_UID));
    AppUidMockSetLockStatus("0");
    EXPECT_TRUE(IsUnlockStatus(CUSTOM_SANDBOX_APPUID));
}

/*
 * @tc.name: AppUid_ConvertPath_UserIdVariable_001
 * @tc.desc: <currentUserId>/<permissionUserId> are replaced with the userId
 *           from appUid (100), never 0 from the 10x uid.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_ConvertPath_UserIdVariable_001, TestSize.Level1)
{
    AppSpawningCtx *property = BuildAppUidProperty("com.test.convpath",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    std::string path = SandboxCommon::ConvertToRealPath(property, "/mnt/sandbox/<currentUserId>/app/data");
    EXPECT_NE(path.find("/mnt/sandbox/100/app/data"), std::string::npos);
    EXPECT_EQ(path.find("/mnt/sandbox/0/"), std::string::npos);

    std::string permPath = SandboxCommon::ConvertToRealPathWithPermission(
        property, "/mnt/sandbox/<permissionUserId>/app/permission");
    EXPECT_NE(permPath.find("/mnt/sandbox/100/app/permission"), std::string::npos);

    CleanupAppUidProperty(property);
}

/*
 * @tc.name: AppUid_DebugSharefs_UserId_001
 * @tc.desc: debug sharefs mount - the mount option user_id is derived from
 *           appUid/UID_BASE (100), not the 10x process uid.
 */
HWTEST_F(AppSpawnAppUidSandboxTest, AppUid_DebugSharefs_UserId_001, TestSize.Level2)
{
    AppSpawningCtx *property = BuildAppUidProperty("com.test.dbgsharefs",
        CUSTOM_SANDBOX_UID, CUSTOM_SANDBOX_APPUID, true);
    ASSERT_NE(property, nullptr);

    ScopedAppUidDirectory directory("/data/local/tmp");
    ASSERT_FALSE(directory.Path().empty());
    const std::string target = directory.Path() + "/child";
    AppUidMockReset();
    ASSERT_EQ(SandboxCore::MountDebugSharefs(property, "/dev/sharefs", target.c_str()), 0);
    EXPECT_EQ(AppUidMockFindMountData("user_id=100"), 1);
    EXPECT_EQ(AppUidMockFindMountData("user_id=0"), 0);
    EXPECT_EQ(AppUidMockMountCallCount(), 2);

    CleanupAppUidProperty(property);
}

}  // namespace
