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
#include <initializer_list>
#include <map>
#include <string>

#include "app_spawn_stub.h"
#include "app_spawn_test_helper.h"
#include "appspawn_manager.h"
#include "lib_wrapper.h"
#include "sandbox_common.h"
#include "sandbox_core.h"

using namespace testing::ext;

namespace OHOS {
namespace AppSpawn {
namespace {
constexpr uint32_t TEST_USER_ID = 100;
constexpr uint32_t TEST_APP_ID = 10029;
constexpr mode_t TEST_DEFAULT_DIR_MODE = 0711;
constexpr mode_t WEAK_DIR_MODE = 0755;
constexpr mode_t LEGACY_DIR_MODE = 0750;
std::map<std::string, mode_t> g_directoryModes;
std::string g_mkdirFailurePath;

// Model mkdir/EEXIST without creating or changing directories on the device.
int RecordMkdir(const char *path, mode_t mode)
{
    std::string directory = path;
    if (directory.size() > 1 && directory.back() == '/') {
        directory.pop_back();
    }
    if (directory == g_mkdirFailurePath) {
        errno = EACCES;
        return -1;
    }
    if (!g_directoryModes.emplace(directory, mode).second) {
        errno = EEXIST;
        return -1;
    }
    return 0;
}
}

class AppSpawnWeakSandboxPermissionTest : public testing::Test {
public:
    void SetUp() override
    {
        savedNoShareFs_ = IsNoShareFsEnable();
        SetNoShareFsEnable(true);
        StubNode *mountNode = GetStubNode(STUB_MOUNT);
        ASSERT_NE(mountNode, nullptr);
        savedMountNode_ = *mountNode;
        mountStateSaved_ = true;
        mountNode->flags = 0;
        AppSpawnClientHandle client = nullptr;
        ASSERT_EQ(AppSpawnClientInit(APPSPAWN_SERVER_NAME, &client), 0);
        ASSERT_EQ(AppSpawnReqMsgCreate(MSG_APP_SPAWN, "com.example.myapplication", &request_), 0);
        ASSERT_EQ(AppSpawnReqMsgSetBundleInfo(request_, 0, "com.example.myapplication"), 0);
        AppDacInfo dacInfo = {};
        dacInfo.uid = TEST_USER_ID * UID_BASE + TEST_APP_ID;
        dacInfo.gid = dacInfo.uid;
        dacInfo.appUid = dacInfo.uid;
        ASSERT_EQ(AppSpawnReqMsgSetAppDacInfo(request_, &dacInfo), 0);
        AppSpawnTestHelper helper;
        property_ = helper.GetAppProperty(client, request_);
        request_ = INVALID_REQ_HANDLE; // GetAppProperty consumes the request.
        ASSERT_NE(property_, nullptr);
        ASSERT_NE(property_->message, nullptr);
        ASSERT_NE(GetAppProperty(property_, TLV_DAC_INFO), nullptr);
        g_directoryModes.clear();
        g_mkdirFailurePath.clear();
        UpdateMkdirFunc(RecordMkdir);
    }

    void TearDown() override
    {
        UpdateMkdirFunc(nullptr);
        g_directoryModes.clear();
        g_mkdirFailurePath.clear();
        SetNoShareFsEnable(savedNoShareFs_);
        if (mountStateSaved_) {
            *GetStubNode(STUB_MOUNT) = savedMountNode_;
        }
        if (request_ != INVALID_REQ_HANDLE) {
            AppSpawnReqMsgFree(request_);
            request_ = INVALID_REQ_HANDLE;
        }
        if (property_ != nullptr) {
            DeleteAppSpawningCtx(property_);
            property_ = nullptr;
        }
    }

protected:
    void ExpectMode(const std::string &path, mode_t mode)
    {
        auto iter = g_directoryModes.find(path);
        ASSERT_NE(iter, g_directoryModes.end()) << path;
        EXPECT_EQ(iter->second, mode) << path;
    }

    void ExpectAncestors()
    {
        ExpectMode("/mnt", TEST_DEFAULT_DIR_MODE);
        ExpectMode("/mnt/sandbox", TEST_DEFAULT_DIR_MODE);
        ExpectMode("/mnt/sandbox/100", TEST_DEFAULT_DIR_MODE);
    }

    AppSpawningCtx *property_ = nullptr;

private:
    AppSpawnReqMsgHandle request_ = INVALID_REQ_HANDLE;
    bool savedNoShareFs_ = false;
    bool mountStateSaved_ = false;
    StubNode savedMountNode_ = {};
};

/**
 * @tc.name: CreateDirRecursive_001
 * @tc.desc: PC weak sandbox creates a readable root and nested parents with restricted ancestors.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_001, TestSize.Level1)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root, TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode(root, WEAK_DIR_MODE);
    g_directoryModes.clear();
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data/storage/el2/", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    for (const auto &suffix : {"", "/data", "/data/storage", "/data/storage/el2"}) {
        ExpectMode(root + suffix, WEAK_DIR_MODE);
    }
}

/**
 * @tc.name: CreateDirRecursive_003
 * @tc.desc: Creating an isolated application root does not widen the isolated ancestor.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_003, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_ISOLATED_SANDBOX_TYPE), 0);
    std::string root = "/mnt/sandbox/100/isolated/com.example.myapplication";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root, TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode("/mnt/sandbox/100/isolated", TEST_DEFAULT_DIR_MODE);
    ExpectMode(root, WEAK_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_004
 * @tc.desc: A deep isolated path creates the application root and all following levels as 0755.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_004, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string root = "/mnt/sandbox/100/isolated/com.example.myapplication";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/system/etc/", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode("/mnt/sandbox/100/isolated", TEST_DEFAULT_DIR_MODE);
    for (const auto &suffix : {"", "/system", "/system/etc"}) {
        ExpectMode(root + suffix, WEAK_DIR_MODE);
    }
}

/**
 * @tc.name: CreateDirRecursive_005
 * @tc.desc: A weak sandbox on a non-PC platform keeps every new directory at 0711.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_005, TestSize.Level1)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    SetNoShareFsEnable(false);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode(root, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/data", TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_006
 * @tc.desc: Non-weak applications preserve the original mode on both PC and non-PC platforms.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_006, TestSize.Level1)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    ASSERT_EQ(CheckAppMsgFlagsSet(property_, APP_FLAGS_CUSTOM_SANDBOX), 0);
    for (bool noShareFs : {true, false}) {
        SetNoShareFsEnable(noShareFs);
        g_directoryModes.clear();
        EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
        ExpectAncestors();
        ExpectMode(root, TEST_DEFAULT_DIR_MODE);
        ExpectMode(root + "/data", TEST_DEFAULT_DIR_MODE);
    }
}

/**
 * @tc.name: CreateDirRecursive_008
 * @tc.desc: Explicitly missing and default application contexts preserve the caller mode.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_008, TestSize.Level2)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/explicit", TEST_DEFAULT_DIR_MODE, nullptr), 0);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/implicit", TEST_DEFAULT_DIR_MODE), 0);
    ExpectAncestors();
    ExpectMode(root, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/explicit", TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/implicit", TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_010
 * @tc.desc: Legacy callers can still explicitly select a mode other than 0711.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_010, TestSize.Level1)
{
    std::string path = "/mnt/sandbox/100/com.example.myapplication/data";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(path, LEGACY_DIR_MODE), 0);
    ExpectMode("/mnt", LEGACY_DIR_MODE);
    ExpectMode("/mnt/sandbox", LEGACY_DIR_MODE);
    ExpectMode("/mnt/sandbox/100", LEGACY_DIR_MODE);
    ExpectMode("/mnt/sandbox/100/com.example.myapplication", LEGACY_DIR_MODE);
    ExpectMode(path, LEGACY_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_011
 * @tc.desc: Missing DAC information does not select weak-sandbox permissions.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_011, TestSize.Level2)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    property_->message->tlvOffset[TLV_DAC_INFO] = INVALID_OFFSET;
    ASSERT_EQ(GetAppProperty(property_, TLV_DAC_INFO), nullptr);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode(root, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/data", TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_012
 * @tc.desc: NWEB paths are not mistaken for a current-user sandbox.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_012, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string root = "/mnt/sandbox/com.ohos.render/com.example.myapplication";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/system/lib", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectMode("/mnt/sandbox/com.ohos.render", TEST_DEFAULT_DIR_MODE);
    ExpectMode(root, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/system", TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/system/lib", TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_013
 * @tc.desc: A user number sharing the current-user prefix does not match the current user.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_013, TestSize.Level2)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string otherUserRoot = "/mnt/sandbox/1000";
    std::string root = otherUserRoot + "/com.example.myapplication";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectMode(otherUserRoot, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/data", TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_014
 * @tc.desc: User-only and isolated-only paths keep their mode with and without trailing slashes.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_014, TestSize.Level2)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    for (std::string path : {"/mnt/sandbox/100", "/mnt/sandbox/100/isolated"}) {
        EXPECT_EQ(SandboxCommon::CreateDirRecursive(path, TEST_DEFAULT_DIR_MODE, property_), 0);
        EXPECT_EQ(SandboxCommon::CreateDirRecursive(path + "/", TEST_DEFAULT_DIR_MODE, property_), 0);
        ExpectMode(path, TEST_DEFAULT_DIR_MODE);
    }
    ExpectAncestors();
    EXPECT_EQ(g_directoryModes.count("/mnt/sandbox/100/com.example.myapplication"), 0U);
    EXPECT_EQ(g_directoryModes.count("/mnt/sandbox/100/isolated/com.example.myapplication"), 0U);
}

/**
 * @tc.name: CreateDirRecursive_016
 * @tc.desc: A debug path outside the user sandbox keeps the original permissions.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_016, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string root = "/mnt/debugtmp/100/debug_hap/com.example.myapplication";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectMode("/mnt/debugtmp", TEST_DEFAULT_DIR_MODE);
    ExpectMode(root, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/data", TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_017
 * @tc.desc: An ancestor mkdir failure is returned and no application directory is created.
 * @tc.type: RELI
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_017, TestSize.Level2)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string root = "/mnt/sandbox/100/isolated/com.example.myapplication";
    g_mkdirFailurePath = "/mnt/sandbox/100/isolated";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), EACCES);
    ExpectAncestors();
    EXPECT_EQ(g_directoryModes.count(g_mkdirFailurePath), 0U);
    EXPECT_EQ(g_directoryModes.count(root), 0U);
    EXPECT_EQ(g_directoryModes.count(root + "/data"), 0U);
}

/**
 * @tc.name: CreateDirRecursive_018
 * @tc.desc: A target mkdir failure is returned without creating descendants.
 * @tc.type: RELI
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_018, TestSize.Level2)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    g_mkdirFailurePath = root + "/data/storage";
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(g_mkdirFailurePath + "/el2", TEST_DEFAULT_DIR_MODE, property_), EACCES);
    ExpectAncestors();
    ExpectMode(root, WEAK_DIR_MODE);
    ExpectMode(root + "/data", WEAK_DIR_MODE);
    EXPECT_EQ(g_directoryModes.count(g_mkdirFailurePath), 0U);
    EXPECT_EQ(g_directoryModes.count(g_mkdirFailurePath + "/el2"), 0U);
}

/**
 * @tc.name: CreateDirRecursive_019
 * @tc.desc: Reusing newly created directories tolerates EEXIST and creates further children.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_019, TestSize.Level1)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    ASSERT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data/config", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode(root, WEAK_DIR_MODE);
    ExpectMode(root + "/data", WEAK_DIR_MODE);
    ExpectMode(root + "/data/config", WEAK_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_020
 * @tc.desc: Isolated preunlock directories can be created first and reused by another consumer.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_020, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string root = "/mnt/sandbox/100/isolated/com.example.myapplication_preunlock";
    ASSERT_EQ(SandboxCommon::CreateDirRecursive(root + "/data/storage/el2", TEST_DEFAULT_DIR_MODE, property_), 0);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data/storage/el3", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode("/mnt/sandbox/100/isolated", TEST_DEFAULT_DIR_MODE);
    for (const auto &suffix : {"", "/data", "/data/storage", "/data/storage/el2", "/data/storage/el3"}) {
        ExpectMode(root + suffix, WEAK_DIR_MODE);
    }
}

/**
 * @tc.name: CreateDirRecursive_021
 * @tc.desc: Missing application flag data cannot widen the application directories.
 * @tc.type: RELI
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_021, TestSize.Level2)
{
    std::string root = "/mnt/sandbox/100/com.example.myapplication";
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    property_->message->tlvOffset[TLV_MSG_FLAGS] = INVALID_OFFSET;
    ASSERT_EQ(GetAppProperty(property_, TLV_MSG_FLAGS), nullptr);
    EXPECT_EQ(CheckAppMsgFlagsSet(property_, APP_FLAGS_CUSTOM_SANDBOX), 0);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode(root, TEST_DEFAULT_DIR_MODE);
    ExpectMode(root + "/data", TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: CreateDirRecursive_022
 * @tc.desc: The generated normal preunlock root uses the platform-specific weak-sandbox mode.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_022, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    ASSERT_EQ(CheckAppMsgFlagsSet(property_, APP_FLAGS_UNLOCKED_STATUS), 0);
    std::string root = SandboxCommon::GetSandboxRootPath(property_, nullptr);
    ASSERT_EQ(root, "/mnt/sandbox/100/com.example.myapplication_preunlock");
    for (bool noShareFs : {true, false}) {
        SetNoShareFsEnable(noShareFs);
        g_directoryModes.clear();
        EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data/storage/el2", TEST_DEFAULT_DIR_MODE, property_), 0);
        ExpectAncestors();
        mode_t expectedMode = noShareFs ? WEAK_DIR_MODE : TEST_DEFAULT_DIR_MODE;
        for (const auto &suffix : {"", "/data", "/data/storage", "/data/storage/el2"}) {
            ExpectMode(root + suffix, expectedMode);
        }
    }
}

/**
 * @tc.name: CreateDirRecursive_023
 * @tc.desc: The generated isolated preunlock root preserves its ancestors on both platforms.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_023, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_ISOLATED_SANDBOX_TYPE), 0);
    ASSERT_EQ(CheckAppMsgFlagsSet(property_, APP_FLAGS_UNLOCKED_STATUS), 0);
    std::string root = SandboxCommon::GetSandboxRootPath(property_, nullptr);
    ASSERT_EQ(root, "/mnt/sandbox/100/isolated/com.example.myapplication_preunlock");
    for (bool noShareFs : {true, false}) {
        SetNoShareFsEnable(noShareFs);
        g_directoryModes.clear();
        EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data/storage/el2", TEST_DEFAULT_DIR_MODE, property_), 0);
        ExpectAncestors();
        ExpectMode("/mnt/sandbox/100/isolated", TEST_DEFAULT_DIR_MODE);
        mode_t expectedMode = noShareFs ? WEAK_DIR_MODE : TEST_DEFAULT_DIR_MODE;
        for (const auto &suffix : {"", "/data", "/data/storage", "/data/storage/el2"}) {
            ExpectMode(root + suffix, expectedMode);
        }
    }
}

/**
 * @tc.name: CreateDirRecursive_024
 * @tc.desc: Non-weak normal and isolated preunlock directories retain the original mode.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_024, TestSize.Level1)
{
    ASSERT_EQ(CheckAppMsgFlagsSet(property_, APP_FLAGS_CUSTOM_SANDBOX), 0);
    ASSERT_EQ(CheckAppMsgFlagsSet(property_, APP_FLAGS_UNLOCKED_STATUS), 0);
    for (bool isolated : {false, true}) {
        if (isolated) {
            ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_ISOLATED_SANDBOX_TYPE), 0);
        }
        std::string root = SandboxCommon::GetSandboxRootPath(property_, nullptr);
        std::string expectedRoot = isolated ? "/mnt/sandbox/100/isolated/com.example.myapplication_preunlock" :
            "/mnt/sandbox/100/com.example.myapplication_preunlock";
        ASSERT_EQ(root, expectedRoot);
        for (bool noShareFs : {true, false}) {
            SetNoShareFsEnable(noShareFs);
            g_directoryModes.clear();
            EXPECT_EQ(SandboxCommon::CreateDirRecursive(
                root + "/data/storage/el2", TEST_DEFAULT_DIR_MODE, property_), 0);
            ExpectAncestors();
            if (isolated) {
                ExpectMode("/mnt/sandbox/100/isolated", TEST_DEFAULT_DIR_MODE);
            }
            for (const auto &suffix : {"", "/data", "/data/storage", "/data/storage/el2"}) {
                ExpectMode(root + suffix, TEST_DEFAULT_DIR_MODE);
            }
        }
    }
}

/**
 * @tc.name: CreateDirRecursive_025
 * @tc.desc: Unlocking selects the main root while both newly created weak roots retain their mode.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, CreateDirRecursive_025, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    ASSERT_EQ(CheckAppMsgFlagsSet(property_, APP_FLAGS_UNLOCKED_STATUS), 0);
    std::string lockedRoot = SandboxCommon::GetSandboxRootPath(property_, nullptr);
    ASSERT_EQ(lockedRoot, "/mnt/sandbox/100/com.example.myapplication_preunlock");
    ASSERT_EQ(SandboxCommon::CreateDirRecursive(lockedRoot + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_UNLOCKED_STATUS), 0);
    std::string root = SandboxCommon::GetSandboxRootPath(property_, nullptr);
    EXPECT_EQ(root, "/mnt/sandbox/100/com.example.myapplication");
    EXPECT_NE(root, lockedRoot);
    EXPECT_EQ(SandboxCommon::CreateDirRecursive(root + "/data", TEST_DEFAULT_DIR_MODE, property_), 0);
    ExpectAncestors();
    ExpectMode(lockedRoot, WEAK_DIR_MODE);
    ExpectMode(lockedRoot + "/data", WEAK_DIR_MODE);
    ExpectMode(root, WEAK_DIR_MODE);
    ExpectMode(root + "/data", WEAK_DIR_MODE);
}

/**
 * @tc.name: DoAppSandboxMountOnce_001
 * @tc.desc: The checked directory mount forwards the weak-sandbox context to parent creation.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, DoAppSandboxMountOnce_001, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string path = "/mnt/sandbox/100/com.example.myapplication/system/etc";
    SharedMountArgs args = {};
    args.srcPath = "/system/etc";
    args.destPath = path.c_str();
    EXPECT_EQ(SandboxCommon::DoAppSandboxMountOnce(property_, &args), 0);
    ExpectAncestors();
    ExpectMode("/mnt/sandbox/100/com.example.myapplication", WEAK_DIR_MODE);
    ExpectMode("/mnt/sandbox/100/com.example.myapplication/system", WEAK_DIR_MODE);
    ExpectMode(path, WEAK_DIR_MODE);
}

/**
 * @tc.name: DoAppSandboxMountOnce_002
 * @tc.desc: The checked directory mount keeps 0711 when the platform is not PC.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, DoAppSandboxMountOnce_002, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    SetNoShareFsEnable(false);
    std::string path = "/mnt/sandbox/100/com.example.myapplication/system/etc";
    SharedMountArgs args = {};
    args.srcPath = "/system/etc";
    args.destPath = path.c_str();
    EXPECT_EQ(SandboxCommon::DoAppSandboxMountOnce(property_, &args), 0);
    ExpectAncestors();
    ExpectMode("/mnt/sandbox/100/com.example.myapplication", TEST_DEFAULT_DIR_MODE);
    ExpectMode("/mnt/sandbox/100/com.example.myapplication/system", TEST_DEFAULT_DIR_MODE);
    ExpectMode(path, TEST_DEFAULT_DIR_MODE);
}

/**
 * @tc.name: DoAppSandboxMountOnceNocheck_001
 * @tc.desc: The nocheck directory mount also forwards the weak-sandbox context.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, DoAppSandboxMountOnceNocheck_001, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    std::string path = "/mnt/sandbox/100/com.example.myapplication/system/etc";
    SharedMountArgs args = {};
    args.srcPath = "/system/etc";
    args.destPath = path.c_str();
    EXPECT_EQ(SandboxCommon::DoAppSandboxMountOnceNocheck(property_, &args), 0);
    ExpectAncestors();
    ExpectMode("/mnt/sandbox/100/com.example.myapplication", WEAK_DIR_MODE);
    ExpectMode("/mnt/sandbox/100/com.example.myapplication/system", WEAK_DIR_MODE);
    ExpectMode(path, WEAK_DIR_MODE);
}

/**
 * @tc.name: DoSandboxRootFolderCreateAdapt_001
 * @tc.desc: Adapted normal and isolated root creation forwards context and preserves ancestor modes.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnWeakSandboxPermissionTest, DoSandboxRootFolderCreateAdapt_001, TestSize.Level1)
{
    ASSERT_EQ(SetAppSpawnMsgFlag(property_->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX), 0);
    for (std::string root : {"/mnt/sandbox/100/com.example.myapplication",
        "/mnt/sandbox/100/isolated/com.example.myapplication"}) {
        EXPECT_EQ(SandboxCore::DoSandboxRootFolderCreateAdapt(property_, root), 0);
        ExpectMode(root, WEAK_DIR_MODE);
    }
    ExpectAncestors();
    ExpectMode("/mnt/sandbox/100/isolated", TEST_DEFAULT_DIR_MODE);
}
} // namespace AppSpawn
} // namespace OHOS
