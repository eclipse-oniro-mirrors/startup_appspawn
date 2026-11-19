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

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>

#include "appspawn.h"
#include "appspawn_client.h"
#include "appspawn_utils.h"
#include "app_spawn_test_helper.h"
#include "parameter.h"
#include "securec.h"

using namespace testing::ext;

// Keep C++ names in CamelCase while retaining the symbols required by the linker's --wrap options.
extern "C" int RealGetParameter(const char *key, const char *def, char *value, uint32_t len)
    __asm__("__real_GetParameter");
extern "C" int RealConnect(int socketFd, const struct sockaddr *address, socklen_t addressLen)
    __asm__("__real_connect");

namespace OHOS {
namespace {
constexpr char SUBSPAWN_PARAMETER[] = "const.startup.appspawn_support_subspawn.enable";
constexpr char TEST_SUBSPAWN_SOCKET_PREFIX[] = "/data/service/el1/startup/subspawn_";
constexpr pid_t RESPONSE_PID = 123;
thread_local bool g_mockParameter = false;
thread_local std::string g_parameterValue;
thread_local bool g_parameterReadFails = false;
thread_local uint32_t g_parameterReads = 0;
thread_local bool g_captureConnect = false;
thread_local bool g_redirectConnect = false;
thread_local std::vector<std::string> g_connectedPaths;
using ClientPtr = std::unique_ptr<void, decltype(&AppSpawnClientDestroy)>;
using SocketNamePtr = std::unique_ptr<char, decltype(&std::free)>;
}

extern "C" int WrapGetParameter(const char *key, const char *def, char *value, uint32_t len)
    __asm__("__wrap_GetParameter");
extern "C" int WrapConnect(int socketFd, const struct sockaddr *address, socklen_t addressLen)
    __asm__("__wrap_connect");

extern "C" int WrapGetParameter(const char *key, const char *def, char *value, uint32_t len)
{
    if (!g_mockParameter || key == nullptr || strcmp(key, SUBSPAWN_PARAMETER) != 0) {
        return RealGetParameter(key, def, value, len);
    }
    g_parameterReads++;
    if (g_parameterReadFails || strcpy_s(value, len, g_parameterValue.c_str()) != EOK) {
        return -1;
    }
    return static_cast<int>(g_parameterValue.size());
}

extern "C" int WrapConnect(int socketFd, const struct sockaddr *address, socklen_t addressLen)
{
    if (!g_captureConnect) {
        return RealConnect(socketFd, address, addressLen);
    }
    const auto *unixAddress = reinterpret_cast<const struct sockaddr_un *>(address);
    g_connectedPaths.emplace_back(unixAddress->sun_path);
    EXPECT_EQ(unixAddress->sun_family, AF_UNIX);
    EXPECT_EQ(addressLen, static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
        g_connectedPaths.back().size() + 1));
    if (!g_redirectConnect) {
        errno = ECONNREFUSED;
        return -1;
    }
    // Exercise real send/receive against the UT server without binding a production subspawn path.
    struct sockaddr_un testAddress = {};
    testAddress.sun_family = AF_UNIX;
    if (strcpy_s(testAddress.sun_path, sizeof(testAddress.sun_path),
        APPSPAWN_SOCKET_DIR APPSPAWN_SOCKET_NAME) != EOK) {
        errno = ENAMETOOLONG;
        return -1;
    }
    socklen_t testAddressLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
        strlen(testAddress.sun_path) + 1);
    return RealConnect(socketFd, reinterpret_cast<const struct sockaddr *>(&testAddress), testAddressLen);
}

class AppSpawnSubspawnClientTest : public testing::Test {
public:
    void SetUp() override
    {
        g_mockParameter = true;
        g_parameterValue.clear();
        g_parameterReadFails = false;
        g_parameterReads = 0;
        g_captureConnect = false;
        g_redirectConnect = false;
        g_connectedPaths.clear();
    }

    void TearDown() override
    {
        g_mockParameter = false;
        g_captureConnect = false;
        g_redirectConnect = false;
        g_connectedPaths.clear();
    }
};

/**
 * @tc.name: IsSupportSubSpawn_001
 * @tc.desc: Only the configured value 1 enables subspawn.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, IsSupportSubSpawn_001, TestSize.Level1)
{
    g_parameterValue = "1";
    EXPECT_TRUE(IsSupportSubSpawn());
    EXPECT_EQ(g_parameterReads, 1u);
}

/**
 * @tc.name: IsSupportSubSpawn_002
 * @tc.desc: Missing, disabled and noncanonical values do not enable subspawn.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, IsSupportSubSpawn_002, TestSize.Level2)
{
    for (const char *value : {"", "0", "true", "01", "1 "}) {
        SCOPED_TRACE(value);
        g_parameterValue = value;
        EXPECT_FALSE(IsSupportSubSpawn());
    }
    EXPECT_EQ(g_parameterReads, 5u);
}

/**
 * @tc.name: IsSupportSubSpawn_003
 * @tc.desc: A parameter read failure leaves subspawn disabled.
 * @tc.type: RELI
 */
HWTEST_F(AppSpawnSubspawnClientTest, IsSupportSubSpawn_003, TestSize.Level2)
{
    g_parameterValue = "1";
    g_parameterReadFails = true;
    EXPECT_FALSE(IsSupportSubSpawn());
    EXPECT_EQ(g_parameterReads, 1u);
}

/**
 * @tc.name: GetSubSpawnSocketName_001
 * @tc.desc: Zero, application UIDs and INT_MAX produce exact absolute socket paths.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, GetSubSpawnSocketName_001, TestSize.Level1)
{
    for (int uid : {0, 20010029, INT_MAX}) {
        SCOPED_TRACE(uid);
        SocketNamePtr socketName(GetSubSpawnSocketName(uid), &std::free);
        ASSERT_NE(socketName, nullptr);
        EXPECT_EQ(std::string(socketName.get()), std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + std::to_string(uid));
    }
}

/**
 * @tc.name: GetSubSpawnSocketName_002
 * @tc.desc: Negative UIDs are rejected, including INT_MIN.
 * @tc.type: SECU
 */
HWTEST_F(AppSpawnSubspawnClientTest, GetSubSpawnSocketName_002, TestSize.Level2)
{
    for (int uid : {-1, INT_MIN}) {
        SocketNamePtr socketName(GetSubSpawnSocketName(uid), &std::free);
        EXPECT_EQ(socketName, nullptr);
    }
}

/**
 * @tc.name: GetSubSpawnSocketName_003
 * @tc.desc: Returned strings have independent caller-owned storage.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, GetSubSpawnSocketName_003, TestSize.Level1)
{
    SocketNamePtr first(GetSubSpawnSocketName(100), &std::free);
    SocketNamePtr second(GetSubSpawnSocketName(100), &std::free);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first.get(), second.get());
    first.reset();
    EXPECT_EQ(std::string(second.get()), std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + "100");
}

/**
 * @tc.name: AppSpawnClientInitSubspawn_001
 * @tc.desc: Valid UID boundaries initialize a lazy client that copies the caller's path.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientInitSubspawn_001, TestSize.Level1)
{
    for (const char *uid : {"0", "20010029", "2147483647", "4294967295"}) {
        std::string path = std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + uid;
        std::string expectedPath = path;
        AppSpawnClientHandle handle = nullptr;
        int ret = AppSpawnClientInitSubspawn(path.c_str(), &handle);
        ClientPtr client(handle, &AppSpawnClientDestroy);
        ASSERT_EQ(ret, 0);
        ASSERT_NE(client, nullptr);
        path.assign("changed");
        auto *manager = static_cast<AppSpawnReqMsgMgr *>(client.get());
        EXPECT_EQ(manager->type, CLIENT_FOR_SUBSPAWN);
        EXPECT_STREQ(manager->socketName, expectedPath.c_str());
        EXPECT_EQ(manager->socketId, -1);
        ASSERT_EQ(pthread_mutex_trylock(&manager->mutex), 0);
        EXPECT_EQ(pthread_mutex_unlock(&manager->mutex), 0);
    }
}

/**
 * @tc.name: AppSpawnClientInitSubspawn_002
 * @tc.desc: Null arguments are rejected and a failed initialization clears the output handle.
 * @tc.type: RELI
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientInitSubspawn_002, TestSize.Level2)
{
    EXPECT_EQ(AppSpawnClientInitSubspawn(nullptr, nullptr), APPSPAWN_ARG_INVALID);
    std::string path = std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + "100";
    EXPECT_EQ(AppSpawnClientInitSubspawn(path.c_str(), nullptr), APPSPAWN_ARG_INVALID);
    int sentinel = 0;
    AppSpawnClientHandle handle = &sentinel;
    EXPECT_EQ(AppSpawnClientInitSubspawn(nullptr, &handle), APPSPAWN_ARG_INVALID);
    EXPECT_EQ(handle, nullptr);
}

/**
 * @tc.name: AppSpawnClientInitSubspawn_003
 * @tc.desc: Reject noncanonical, overflowing and traversing socket paths.
 * @tc.type: SECU
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientInitSubspawn_003, TestSize.Level2)
{
    std::vector<std::string> paths = {"", "subspawn_100", "/tmp/subspawn_100"};
    for (const char *suffix : {"", "00", "01", "-1", "+1", "1 ", " 1", "1x", "1/../2",
        "4294967296", "999999999999999999999"}) {
        paths.emplace_back(std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + suffix);
    }
    for (const auto &path : paths) {
        SCOPED_TRACE(path);
        AppSpawnClientHandle handle = nullptr;
        int ret = AppSpawnClientInitSubspawn(path.c_str(), &handle);
        ClientPtr client(handle, &AppSpawnClientDestroy);
        EXPECT_EQ(ret, APPSPAWN_ARG_INVALID);
        EXPECT_EQ(client, nullptr);
    }
}

/**
 * @tc.name: AppSpawnClientInitSubspawn_004
 * @tc.desc: Reject overlength and nonterminated input without reading beyond the length limit.
 * @tc.type: SECU
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientInitSubspawn_004, TestSize.Level2)
{
    std::array<char, APP_SUBSPAWN_SOCKET_NAME_LEN> path;
    path.fill('1');
    ASSERT_EQ(memcpy_s(path.data(), path.size(), TEST_SUBSPAWN_SOCKET_PREFIX,
        sizeof(TEST_SUBSPAWN_SOCKET_PREFIX) - 1), EOK);
    AppSpawnClientHandle handle = nullptr;
    EXPECT_EQ(AppSpawnClientInitSubspawn(path.data(), &handle), APPSPAWN_ARG_INVALID);
    EXPECT_EQ(handle, nullptr);
    std::string longPath = std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + std::string(APP_SUBSPAWN_SOCKET_NAME_LEN, '1');
    EXPECT_EQ(AppSpawnClientInitSubspawn(longPath.c_str(), &handle), APPSPAWN_ARG_INVALID);
    EXPECT_EQ(handle, nullptr);
}

/**
 * @tc.name: AppSpawnClientInitSubspawn_005
 * @tc.desc: Clients with identical or different UIDs have independent lifetimes and timeout state.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientInitSubspawn_005, TestSize.Level1)
{
    std::vector<ClientPtr> clients;
    for (const char *uid : {"100", "100", "101"}) {
        std::string path = std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + uid;
        AppSpawnClientHandle handle = nullptr;
        int ret = AppSpawnClientInitSubspawn(path.c_str(), &handle);
        clients.emplace_back(handle, &AppSpawnClientDestroy);
        ASSERT_EQ(ret, 0);
        ASSERT_NE(handle, nullptr);
    }
    EXPECT_NE(clients[0].get(), clients[1].get());
    EXPECT_NE(clients[1].get(), clients[2].get());
    auto *third = static_cast<AppSpawnReqMsgMgr *>(clients[2].get());
    uint32_t thirdTimeout = third->timeout;
    AppSpawnClientHandle standardHandle = nullptr;
    int ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &standardHandle);
    ClientPtr standardClient(standardHandle, &AppSpawnClientDestroy);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(standardClient, nullptr);
    static_cast<AppSpawnReqMsgMgr *>(clients[1].get())->timeout = APPSPAWN_CLIENT_TIMEOUT_MAX;
    clients[0].reset();
    EXPECT_EQ(static_cast<AppSpawnReqMsgMgr *>(clients[1].get())->timeout, APPSPAWN_CLIENT_TIMEOUT_MAX);
    EXPECT_EQ(third->timeout, thirdTimeout);
    ASSERT_EQ(pthread_mutex_trylock(&third->mutex), 0);
    EXPECT_EQ(pthread_mutex_unlock(&third->mutex), 0);
    AppSpawnClientHandle cachedHandle = nullptr;
    EXPECT_EQ(AppSpawnClientInit(APPSPAWN_SERVER_NAME, &cachedHandle), 0);
    EXPECT_EQ(cachedHandle, standardClient.get());
}

/**
 * @tc.name: AppSpawnClientInit_001
 * @tc.desc: The generic initializer routes valid subspawn paths and rejects invalid subspawn suffixes.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientInit_001, TestSize.Level1)
{
    std::string path = std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + "20010029";
    AppSpawnClientHandle handle = nullptr;
    int ret = AppSpawnClientInit(path.c_str(), &handle);
    ClientPtr client(handle, &AppSpawnClientDestroy);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(client, nullptr);
    auto *manager = static_cast<AppSpawnReqMsgMgr *>(client.get());
    EXPECT_EQ(manager->type, CLIENT_FOR_SUBSPAWN);
    EXPECT_STREQ(manager->socketName, path.c_str());
    path += "x";
    handle = nullptr;
    EXPECT_EQ(AppSpawnClientInit(path.c_str(), &handle), APPSPAWN_ARG_INVALID);
    EXPECT_EQ(handle, nullptr);
}

/**
 * @tc.name: AppSpawnClientSendMsg_001
 * @tc.desc: Send and receive over the absolute subspawn route, then reuse the same connection.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientSendMsg_001, TestSize.Level1)
{
    AppSpawnClientHandle handle = nullptr;
    std::string path = std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + "20010029";
    int ret = AppSpawnClientInitSubspawn(path.c_str(), &handle);
    ClientPtr client(handle, &AppSpawnClientDestroy);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(client, nullptr);
    AppSpawnTestServer server("appspawn -mode appspawn", false);
    server.Start([](TestConnection *connection, const uint8_t *buffer, uint32_t bufferLen) {
        connection->SendResponse(reinterpret_cast<AppSpawnMsg *>(const_cast<uint8_t *>(buffer)), 0, RESPONSE_PID);
    });
    g_captureConnect = true;
    g_redirectConnect = true;
    for (int index = 0; index < 2; index++) {
        AppSpawnReqMsgHandle request = nullptr;
        ret = AppSpawnReqMsgCreate(MSG_DUMP, "subspawn-client-ut", &request);
        EXPECT_EQ(ret, 0);
        AppSpawnResult result = {};
        EXPECT_EQ(AppSpawnClientSendMsg(client.get(), request, &result), 0);
        EXPECT_EQ(result.result, 0);
        EXPECT_EQ(result.pid, RESPONSE_PID);
    }
    client.reset();
    server.Stop();
    ASSERT_EQ(g_connectedPaths.size(), 1u);
    EXPECT_EQ(g_connectedPaths[0], path);
}

/**
 * @tc.name: AppSpawnClientSendMsg_002
 * @tc.desc: Connection refusal retries the same absolute path and keeps the handle reusable.
 * @tc.type: RELI
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientSendMsg_002, TestSize.Level2)
{
    std::string path = std::string(TEST_SUBSPAWN_SOCKET_PREFIX) + "20010029";
    AppSpawnClientHandle handle = nullptr;
    int ret = AppSpawnClientInitSubspawn(path.c_str(), &handle);
    ClientPtr client(handle, &AppSpawnClientDestroy);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(client, nullptr);
    auto *manager = static_cast<AppSpawnReqMsgMgr *>(client.get());
    constexpr uint32_t retryCount = 2;
    manager->maxRetryCount = retryCount;
    g_captureConnect = true;
    for (int index = 0; index < 2; index++) {
        AppSpawnReqMsgHandle request = nullptr;
        ASSERT_EQ(AppSpawnReqMsgCreate(MSG_DUMP, "subspawn-client-ut", &request), 0);
        AppSpawnResult result = {};
        EXPECT_EQ(AppSpawnClientSendMsg(client.get(), request, &result), APPSPAWN_TIMEOUT);
        EXPECT_EQ(result.result, APPSPAWN_TIMEOUT);
        EXPECT_EQ(manager->socketId, -1);
    }
    ASSERT_EQ(g_connectedPaths.size(), 2 * retryCount * retryCount);
    for (const auto &connectedPath : g_connectedPaths) {
        EXPECT_EQ(connectedPath, path);
    }
}

/**
 * @tc.name: AppSpawnClientSendMsg_003
 * @tc.desc: Existing client variants retain the normal socket directory when connections fail.
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnSubspawnClientTest, AppSpawnClientSendMsg_003, TestSize.Level1)
{
    const char *services[] = {APPSPAWN_SERVER_NAME, NWEBSPAWN_SERVER_NAME, NATIVESPAWN_SERVER_NAME,
        CJAPPSPAWN_SERVER_NAME, HYBRIDSPAWN_SERVER_NAME};
    const char *sockets[] = {APPSPAWN_SOCKET_NAME, NWEBSPAWN_SOCKET_NAME, NATIVESPAWN_SOCKET_NAME,
        CJAPPSPAWN_SOCKET_NAME, HYBRIDSPAWN_SOCKET_NAME};
    g_captureConnect = true;
    for (size_t index = 0; index < sizeof(services) / sizeof(services[0]); index++) {
        SCOPED_TRACE(services[index]);
        AppSpawnClientHandle handle = nullptr;
        int ret = AppSpawnClientInit(services[index], &handle);
        ClientPtr client(handle, &AppSpawnClientDestroy);
        ASSERT_EQ(ret, 0);
        ASSERT_NE(client, nullptr);
        static_cast<AppSpawnReqMsgMgr *>(client.get())->maxRetryCount = 1;
        AppSpawnReqMsgHandle request = nullptr;
        ASSERT_EQ(AppSpawnReqMsgCreate(MSG_DUMP, "subspawn-client-ut", &request), 0);
        AppSpawnResult result = {};
        EXPECT_EQ(AppSpawnClientSendMsg(client.get(), request, &result), APPSPAWN_TIMEOUT);
        ASSERT_EQ(g_connectedPaths.size(), index + 1);
        EXPECT_EQ(g_connectedPaths.back(), std::string(APPSPAWN_SOCKET_DIR) + sockets[index]);
    }
}
} // namespace OHOS
