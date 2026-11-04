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
#include <sys/capability.h>

#include "appspawn.h"
#include "appspawn_manager.h"
#include "appspawn_utils.h"
#include "appspawn_client.h"
#include "appspawn_capability_mock.h"

using namespace testing::ext;

namespace OHOS {
namespace {
constexpr uint64_t DAC_CAP = CAP_TO_MASK(CAP_DAC_OVERRIDE);
constexpr uint64_t FOWNER_CAP = CAP_TO_MASK(CAP_FOWNER);
constexpr uint64_t NET_RAW_CAP = CAP_TO_MASK(CAP_NET_RAW);
constexpr char NET_RAW_PERMISSION[] = R"({"permissions":[{"ohos.permission.kernel.NET_RAW":true}]})";
}

// Exercise real TLV encoding/decoding and capability policy with mocked system calls.
// Real UID transitions and kernel capability enforcement require device integration tests.
class AppSpawnCapabilityTest : public testing::Test {
public:
    void SetUp() override
    {
        previousNoShareFs_ = IsNoShareFsEnable();
        SetNoShareFsEnable(true);
        SetCapabilityTestState(&state_);
        mgr_ = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr_, nullptr);
        property_ = CreateAppSpawningCtx();
        ASSERT_NE(property_, nullptr);
    }

    void TearDown() override
    {
        SetCapabilityTestState(nullptr);
        SetNoShareFsEnable(previousNoShareFs_);
        DeleteAppSpawningCtx(property_);
        DeleteAppSpawnMgr(mgr_);
    }

    // Serialize flags and permission JSON through the real request/decoder path.
    bool PrepareAppSpawnContext(std::initializer_list<AppFlagsIndex> flags, const char *permissions = nullptr)
    {
        AppSpawnReqMsgHandle req = INVALID_REQ_HANDLE;
        if (AppSpawnReqMsgCreate(MSG_APP_SPAWN, "com.example.capabilitytest", &req) != 0) {
            return false;
        }
        for (auto flag : flags) {
            if (AppSpawnReqMsgSetAppFlag(req, flag) != 0) {
                AppSpawnReqMsgFree(req);
                return false;
            }
        }
        if (permissions != nullptr &&
            AppSpawnReqMsgAddStringInfo(req, MSG_EXT_NAME_JIT_PERMISSIONS, permissions) != 0) {
            AppSpawnReqMsgFree(req);
            return false;
        }
        bool decoded = DecodeRequest(req);
        AppSpawnReqMsgFree(req);
        return decoded;
    }

    bool DecodeRequest(AppSpawnReqMsgHandle req)
    {
        const auto *reqNode = static_cast<const AppSpawnReqMsgNode *>(req);
        uint32_t received = 0;
        uint32_t remainder = 0;
        for (const ListNode *node = reqNode->msgBlocks.next; node != &reqNode->msgBlocks; node = node->next) {
            const auto *block = ListEntry(node, AppSpawnMsgBlock, node);
            if (GetAppSpawnMsgFromBuffer(block->buffer, block->currentIndex,
                &property_->message, &received, &remainder) != 0 || remainder != 0) {
                return false;
            }
        }
        return received == reqNode->msg->msgLen && DecodeAppSpawnMsg(property_->message) == 0;
    }

    // The full setup path calls capset once with identical effective/permitted/inheritable sets.
    void ExpectCapabilities(uint64_t caps, uint64_t ambient, unsigned int ambientCalls) const
    {
        EXPECT_EQ(state_.capsetCalls, 1u);
        EXPECT_EQ(state_.effective, caps);
        EXPECT_EQ(state_.permitted, caps);
        EXPECT_EQ(state_.inheritable, caps);
        EXPECT_EQ(state_.ambient, ambient);
        EXPECT_EQ(state_.ambientCalls, ambientCalls);
    }

    // Direct ambient setup must not call capset; verify attempted raises only.
    void ExpectAmbientCapabilities(uint64_t ambient, unsigned int ambientCalls) const
    {
        EXPECT_EQ(state_.capsetCalls, 0u);
        EXPECT_EQ(state_.ambient, ambient);
        EXPECT_EQ(state_.ambientCalls, ambientCalls);
    }

protected:
    AppSpawnMgr *mgr_ = nullptr;
    AppSpawningCtx *property_ = nullptr;
    CapabilityTestState state_ = {};
    bool previousNoShareFs_ = false;
};

HWTEST_F(AppSpawnCapabilityTest, App_Spawn_SetCapabilities_CustomSandbox_NoDangerCaps, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX}));
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    ExpectCapabilities(0, 0, 0);
}

HWTEST_F(AppSpawnCapabilityTest, CustomSandboxPreservesFownerAndExtendedAmbient, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SET_CAPS_FOWNER}));
    ASSERT_EQ(SetAmbientCapabilities(property_, NET_RAW_CAP), 0);
    ExpectAmbientCapabilities(FOWNER_CAP | NET_RAW_CAP, 2);
}

HWTEST_F(AppSpawnCapabilityTest, NormalAppPreservesDacAndNetRaw, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({}, NET_RAW_PERMISSION));
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    ExpectCapabilities(DAC_CAP | NET_RAW_CAP, DAC_CAP, 1);
}

HWTEST_F(AppSpawnCapabilityTest, NormalAppPreservesExtendedAmbientCaps, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_SET_CAPS_FOWNER}));
    ASSERT_EQ(SetAmbientCapabilities(property_, NET_RAW_CAP), 0);
    ExpectAmbientCapabilities(DAC_CAP | FOWNER_CAP | NET_RAW_CAP, 3);
}

HWTEST_F(AppSpawnCapabilityTest, App_Spawn_SetCapabilities_NonNoShareFs_Unchanged, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SET_CAPS_FOWNER}, NET_RAW_PERMISSION));
    SetNoShareFsEnable(false);
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    EXPECT_EQ(SetAmbientCapabilities(property_, DAC_CAP | NET_RAW_CAP), 0);
    ExpectCapabilities(0, 0, 0);
}

HWTEST_F(AppSpawnCapabilityTest, IsolatedSandboxPreservesBehavior, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_ISOLATED_SANDBOX_TYPE,
        APP_FLAGS_SET_CAPS_FOWNER}, NET_RAW_PERMISSION));
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    ExpectCapabilities(0, 0, 0);
}

HWTEST_F(AppSpawnCapabilityTest, SpawnModesPreserveCapabilityGates, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SET_CAPS_FOWNER}, NET_RAW_PERMISSION));
    const RunMode modes[] = {MODE_FOR_APP_SPAWN, MODE_FOR_APP_COLD_RUN,
        MODE_FOR_NATIVE_SPAWN, MODE_FOR_NATIVE_COLD_RUN, MODE_FOR_NWEB_SPAWN, MODE_FOR_NWEB_COLD_RUN,
        MODE_FOR_CJAPP_SPAWN, MODE_FOR_CJAPP_COLD_RUN, MODE_FOR_HYBRID_SPAWN, MODE_FOR_HYBRID_COLD_RUN};
    for (auto mode : modes) {
        SCOPED_TRACE(static_cast<int>(mode));
        mgr_->content.mode = mode;
        state_ = {};
        ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
        // NeedExtPerm enables APP/NATIVE (including cold runs); others set zero E/P/I and skip ambient.
        bool enabled = mode == MODE_FOR_APP_SPAWN || mode == MODE_FOR_APP_COLD_RUN ||
            mode == MODE_FOR_NATIVE_SPAWN || mode == MODE_FOR_NATIVE_COLD_RUN;
        ExpectCapabilities(enabled ? FOWNER_CAP | NET_RAW_CAP : 0,
            enabled ? FOWNER_CAP : 0, enabled ? 1 : 0);
    }
}

HWTEST_F(AppSpawnCapabilityTest, CapsetFailureStopsAmbientSetup, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SET_CAPS_FOWNER}));
    state_.capsetResult = -1;
    EXPECT_EQ(SetCapabilities(mgr_, property_), -EPERM);
    ExpectCapabilities(FOWNER_CAP, 0, 0);
}

HWTEST_F(AppSpawnCapabilityTest, FownerAmbientFailurePropagates, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SET_CAPS_FOWNER}));
    state_.ambientResult = -1;
    EXPECT_EQ(SetCapabilities(mgr_, property_), -1);
    ExpectCapabilities(FOWNER_CAP, FOWNER_CAP, 1);
}

HWTEST_F(AppSpawnCapabilityTest, NormalAmbientFailurePropagates, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({}));
    state_.ambientResult = -1;
    EXPECT_EQ(SetCapabilities(mgr_, property_), -1);
    ExpectCapabilities(DAC_CAP, DAC_CAP, 1);
}

HWTEST_F(AppSpawnCapabilityTest, ExtendedAmbientFailurePropagates, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX}));
    state_.ambientResult = -1;
    EXPECT_EQ(SetAmbientCapabilities(property_, NET_RAW_CAP), -1);
    ExpectAmbientCapabilities(NET_RAW_CAP, 1);
}

HWTEST_F(AppSpawnCapabilityTest, App_Spawn_SetAmbientCapabilities_CustomSandbox_NoDangerCaps, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX}));
    ASSERT_EQ(SetAmbientCapabilities(property_, 0), 0);
    ExpectAmbientCapabilities(0, 0);
}

HWTEST_F(AppSpawnCapabilityTest, App_Spawn_SetAmbientCapabilities_Normal_KeepsDangerCaps, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({}));
    ASSERT_EQ(SetAmbientCapabilities(property_, 0), 0);
    ExpectAmbientCapabilities(DAC_CAP, 1);
}

HWTEST_F(AppSpawnCapabilityTest, App_Spawn_SetCapabilities_Normal_KeepsDangerCaps, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({}));
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    ExpectCapabilities(DAC_CAP, DAC_CAP, 1);
}

HWTEST_F(AppSpawnCapabilityTest, CustomSandboxPreservesFowner, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SET_CAPS_FOWNER}));
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    ExpectCapabilities(FOWNER_CAP, FOWNER_CAP, 1);
}

HWTEST_F(AppSpawnCapabilityTest, CustomSandboxPreservesNetRaw, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX}, NET_RAW_PERMISSION));
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    // Production NET_RAW is not configured as ambient.
    ExpectCapabilities(NET_RAW_CAP, 0, 0);
}

HWTEST_F(AppSpawnCapabilityTest, CustomSandboxPreservesFownerAndNetRaw, TestSize.Level0)
{
    ASSERT_TRUE(PrepareAppSpawnContext({APP_FLAGS_CUSTOM_SANDBOX, APP_FLAGS_SET_CAPS_FOWNER}, NET_RAW_PERMISSION));
    ASSERT_EQ(SetCapabilities(mgr_, property_), 0);
    ExpectCapabilities(FOWNER_CAP | NET_RAW_CAP, FOWNER_CAP, 1);
}
} // namespace OHOS
