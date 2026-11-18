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
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "appspawn.h"
#include "appspawn_utils.h"

using namespace testing;
using namespace testing::ext;

namespace OHOS {
namespace {
constexpr int CLEANUP_RETRY = 20;
constexpr int CLEANUP_INTERVAL_US = 50 * 1000;

uint64_t ReadNumber(const char *value, uint64_t maxValue)
{
    if (value == nullptr || value[0] < '0' || value[0] > '9') {
        return 0;
    }
    char *end = nullptr;
    errno = 0;
    uint64_t number = strtoull(value, &end, 10);
    return errno == 0 && *end == '\0' && number <= maxValue ? number : 0;
}

struct TestIdentity {
    const char *bundle = nullptr;
    uint64_t tokenId = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t appIndex = 0;
};

int FillIdentity(AppSpawnReqMsgHandle req, const TestIdentity &identity)
{
    AppDacInfo dac = {};
    dac.uid = identity.uid;
    dac.appUid = identity.uid;
    dac.gid = identity.gid;
    dac.gidCount = 1;
    dac.gidTable[0] = identity.gid;
    int results[] = {
        AppSpawnReqMsgSetBundleInfo(req, identity.appIndex, identity.bundle),
        AppSpawnReqMsgSetAppFlag(req, APP_FLAGS_ISOLATED_SANDBOX_TYPE),
        AppSpawnReqMsgSetAppDomainInfo(req, 0, "normal"),
        AppSpawnReqMsgSetAppInternetPermissionInfo(req, 0, 0),
        AppSpawnReqMsgSetAppAccessToken(req, identity.tokenId),
        AppSpawnReqMsgSetAppDacInfo(req, &dac),
    };
    for (int ret : results) {
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

int SendRequest(AppSpawnClientHandle handle, AppSpawnMsgType type, const TestIdentity &identity,
    AppSpawnResult &result, pid_t instancePid = 0)
{
    AppSpawnReqMsgHandle req = INVALID_REQ_HANDLE;
    int ret = AppSpawnReqMsgCreate(type, identity.bundle, &req);
    if (ret != 0) {
        return ret;
    }
    std::unique_ptr<void, decltype(&AppSpawnReqMsgFree)> request(req, &AppSpawnReqMsgFree);
    ret = FillIdentity(req, identity);
    if (ret != 0) {
        return ret;
    }
    if (type == MSG_APP_SPAWN) {
        ret = AppSpawnReqMsgSetAppFlag(req, APP_FLAGS_CHILDPROCESS);
        if (ret == 0) {
            const std::string parentUid = std::to_string(identity.uid);
            ret = AppSpawnReqMsgAddStringInfo(req, MSG_EXT_NAME_PARENT_UID, parentUid.c_str());
        }
    } else if (type == MSG_SUBSPAWN_SHUTDOWN) {
        ret = AppSpawnReqMsgAddExtInfo(req, MSG_EXT_NAME_SUBSPAWN_PID,
            reinterpret_cast<const uint8_t *>(&instancePid), sizeof(instancePid));
    }
    if (ret != 0) {
        return ret;
    }
    return AppSpawnClientSendMsg(handle, request.release(), &result);
}

bool IsProcessGone(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == -1 && errno == ESRCH;
}

bool IsSocketGone(const char *name)
{
    struct stat info = {};
    return lstat(name, &info) == -1 && errno == ENOENT;
}

bool WaitForCleanup(pid_t pid, pid_t childPid, const char *socketName)
{
    for (int i = 0; i < CLEANUP_RETRY; ++i) {
        if (IsProcessGone(pid) && (childPid <= 0 || IsProcessGone(childPid)) && IsSocketGone(socketName)) {
            return true;
        }
        usleep(CLEANUP_INTERVAL_US);
    }
    return false;
}
}  // namespace

class SubspawnMstTest : public testing::Test {
protected:
    void SetUp() override
    {
        const char *token = getenv("SUBSPAWN_MST_TOKEN");
        if (!IsSupportSubSpawn() || token == nullptr) {
            GTEST_SKIP() << "subspawn is disabled or SUBSPAWN_MST_TOKEN is not set";
        }
        identity_.tokenId = ReadNumber(token, UINT64_MAX);
        ASSERT_NE(identity_.tokenId, 0u) << "Invalid SUBSPAWN_MST_TOKEN";
        identity_.bundle = getenv("SUBSPAWN_MST_BUNDLE");
        ASSERT_NE(identity_.bundle, nullptr) << "Set SUBSPAWN_MST_BUNDLE";
        ASSERT_NE(identity_.bundle[0], '\0');
        identity_.uid = static_cast<uint32_t>(ReadNumber(getenv("SUBSPAWN_MST_UID"), INT32_MAX));
        ASSERT_NE(identity_.uid, 0u) << "Set SUBSPAWN_MST_UID to the installed HAP UID";
        const char *gid = getenv("SUBSPAWN_MST_GID");
        identity_.gid = gid == nullptr ? identity_.uid : static_cast<uint32_t>(ReadNumber(gid, UINT32_MAX));
        ASSERT_NE(identity_.gid, 0u) << "Invalid SUBSPAWN_MST_GID";
        const char *index = getenv("SUBSPAWN_MST_APP_INDEX");
        if (index != nullptr) {
            // ReadNumber 使用 0 表示无效值，显式的索引 0 也是合法输入。
            identity_.appIndex = static_cast<uint32_t>(ReadNumber(index, UINT32_MAX));
            ASSERT_TRUE(identity_.appIndex != 0 || std::string(index) == "0") << "Invalid SUBSPAWN_MST_APP_INDEX";
        }
        socketName_.reset(GetSubSpawnSocketName(static_cast<int>(identity_.uid)));
        ASSERT_NE(socketName_, nullptr);
        ASSERT_TRUE(IsSocketGone(socketName_.get())) << "Use a dedicated HAP without an existing subspawn instance";
        AppSpawnClientHandle handle = nullptr;
        ASSERT_EQ(AppSpawnClientInit(APPSPAWN_SERVER_NAME, &handle), 0);
        appspawn_.reset(handle);
    }

    void TearDown() override
    {
        if (instancePid_ > 0) {
            StopInstance();
        }
    }

    void StartInstance()
    {
        AppSpawnResult result = {};
        ASSERT_EQ(SendRequest(appspawn_.get(), MSG_SUBSPAWN_CREATE, identity_, result), 0);
        ASSERT_EQ(result.result, 0);
        ASSERT_GT(result.pid, 0);
        instancePid_ = result.pid;
    }

    void StopInstance()
    {
        AppSpawnResult result = {};
        ASSERT_EQ(SendRequest(appspawn_.get(), MSG_SUBSPAWN_SHUTDOWN, identity_, result, instancePid_), 0);
        ASSERT_EQ(result.result, 0);
        const pid_t pid = instancePid_;
        instancePid_ = 0;
        EXPECT_TRUE(WaitForCleanup(pid, childPid_, socketName_.get()));
    }

    TestIdentity identity_;
    std::unique_ptr<char, decltype(&std::free)> socketName_ {nullptr, &std::free};
    std::unique_ptr<void, decltype(&AppSpawnClientDestroy)> appspawn_ {nullptr, &AppSpawnClientDestroy};
    pid_t instancePid_ = 0;
    pid_t childPid_ = 0;
};

/**
 * @tc.name: Subspawn_Mst_Create_001
 * @tc.desc: 验证实例与 socket 属主，并确认同一应用身份复用实例
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnMstTest, Subspawn_Mst_Create_001, TestSize.Level0)
{
    ASSERT_NO_FATAL_FAILURE(StartInstance());
    ASSERT_EQ(kill(instancePid_, 0), 0);

    struct stat info = {};
    ASSERT_EQ(lstat(socketName_.get(), &info), 0);
    EXPECT_TRUE(S_ISSOCK(info.st_mode));
    EXPECT_EQ(info.st_uid, identity_.uid);

    AppSpawnResult result = {};
    ASSERT_EQ(SendRequest(appspawn_.get(), MSG_SUBSPAWN_CREATE, identity_, result), 0);
    ASSERT_EQ(result.result, 0);
    EXPECT_EQ(result.pid, instancePid_);
}

/**
 * @tc.name: Subspawn_Mst_Fork_001
 * @tc.desc: 连接实例发起 FORK，验证服务端返回独立的子进程 PID
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnMstTest, Subspawn_Mst_Fork_001, TestSize.Level0)
{
    ASSERT_NO_FATAL_FAILURE(StartInstance());
    AppSpawnClientHandle handle = nullptr;
    ASSERT_EQ(AppSpawnClientInitSubspawn(socketName_.get(), &handle), 0);
    std::unique_ptr<void, decltype(&AppSpawnClientDestroy)> instance(handle, &AppSpawnClientDestroy);

    AppSpawnResult result = {};
    ASSERT_EQ(SendRequest(instance.get(), MSG_APP_SPAWN, identity_, result), 0);
    ASSERT_EQ(result.result, 0);
    ASSERT_GT(result.pid, 0);
    ASSERT_NE(result.pid, instancePid_);
    childPid_ = result.pid;
}

/**
 * @tc.name: Subspawn_Mst_Shutdown_001
 * @tc.desc: 经主 appspawn 关闭实例并验证进程与 socket 回收
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnMstTest, Subspawn_Mst_Shutdown_001, TestSize.Level0)
{
    ASSERT_NO_FATAL_FAILURE(StartInstance());
    ASSERT_EQ(kill(instancePid_, 0), 0);
    ASSERT_FALSE(IsSocketGone(socketName_.get()));
    StopInstance();
}
}  // namespace OHOS
