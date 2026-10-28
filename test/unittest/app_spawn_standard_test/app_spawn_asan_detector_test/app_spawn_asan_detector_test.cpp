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
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <unistd.h>

#include "appspawn_modulemgr.h"
#include "appspawn_server.h"
#include "appspawn_manager.h"
#include "appspawn.h"
#include "parameter.h"
#include "securec.h"
#include "spawning_test_helper.h"

#ifdef __cplusplus
extern "C" {
#endif

APPSPAWN_STATIC bool RegisterFfrtStackFunc();
APPSPAWN_STATIC bool IsInRenderProcess(const AppSpawningCtx *property);
APPSPAWN_STATIC int SetAsanEnabledEnv(const AppSpawnMgr *content, const AppSpawningCtx *property);
APPSPAWN_STATIC void SetGwpAsanEnabled(const AppSpawnMgr *content, const AppSpawningCtx *property);
APPSPAWN_STATIC int CheckSupportColdStart(const char *bundleName);
APPSPAWN_STATIC int AsanSpawnGetSpawningFlag(AppSpawnMgr *content, AppSpawningCtx *property);
APPSPAWN_STATIC int AsanSpawnInitSpawningEnv(AppSpawnMgr *content, AppSpawningCtx *property);

#ifdef __cplusplus
}
#endif

using namespace testing;
using namespace testing::ext;
using namespace OHOS;

namespace {
using FfrtCoroutineStackFn = bool (*)(void **stackAddr, size_t *size);
using RegisterFfrtStackFuncFn = void (*)(FfrtCoroutineStackFn fn);

constexpr const char *REGISTER_FUNC_SYMBOL = "libc_gwp_asan_register_ffrt_stack_func";
constexpr const char *FFRT_STACK_FUNC_SYMBOL = "ffrt_get_current_coroutine_stack";

void *g_mockRegisterFuncSymbol = nullptr;
void *g_mockFfrtFuncSymbol = nullptr;

int g_dlsymRegisterFuncCount = 0;
int g_dlsymFfrtFuncCount = 0;
int g_registerCalledCount = 0;

FfrtCoroutineStackFn g_registeredFfrtFunc = nullptr;

void ResetRegisterFfrtStackFuncMock()
{
    g_mockRegisterFuncSymbol = nullptr;
    g_mockFfrtFuncSymbol = nullptr;

    g_dlsymRegisterFuncCount = 0;
    g_dlsymFfrtFuncCount = 0;
    g_registerCalledCount = 0;

    g_registeredFfrtFunc = nullptr;
}

bool MockFfrtGetCurrentCoroutineStack(void **stackAddr, size_t *size)
{
    if (stackAddr != nullptr) {
        *stackAddr = nullptr;
    }

    if (size != nullptr) {
        *size = 0;
    }

    return true;
}

void MockLibcGwpAsanRegisterFfrtStackFunc(FfrtCoroutineStackFn fn)
{
    g_registerCalledCount++;
    g_registeredFfrtFunc = fn;
}

void ClearAsanEnv()
{
    unsetenv("LD_PRELOAD");
    unsetenv("ASAN_OPTIONS");
    unsetenv("TSAN_OPTIONS");
    unsetenv("UBSAN_OPTIONS");
    unsetenv("HWASAN_OPTIONS");
}
} // namespace

extern "C" void *__wrap_dlsym(void *handle, const char *symbol)
{
    (void)handle;

    if (symbol == nullptr) {
        return nullptr;
    }

    if (strcmp(symbol, REGISTER_FUNC_SYMBOL) == 0) {
        g_dlsymRegisterFuncCount++;
        return g_mockRegisterFuncSymbol;
    }

    if (strcmp(symbol, FFRT_STACK_FUNC_SYMBOL) == 0) {
        g_dlsymFfrtFuncCount++;
        return g_mockFfrtFuncSymbol;
    }

    return nullptr;
}

namespace OHOS {
class AppSpawnAsanDetectorTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
        ResetRegisterFfrtStackFuncMock();
        ClearAsanEnv();
    }
    void TearDown()
    {
        ResetRegisterFfrtStackFuncMock();
        ClearAsanEnv();
    }
};

/**
 * @tc.name: RegisterFfrtStackFunc01
 * @tc.desc: libc_gwp_asan_register_ffrt_stack_func symbol not found
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, RegisterFfrtStackFunc01, TestSize.Level1)
{
    g_mockRegisterFuncSymbol = nullptr;
    g_mockFfrtFuncSymbol = reinterpret_cast<void *>(MockFfrtGetCurrentCoroutineStack);

    bool ret = RegisterFfrtStackFunc();

    EXPECT_FALSE(ret);
    EXPECT_EQ(g_dlsymRegisterFuncCount, 1);
    EXPECT_EQ(g_dlsymFfrtFuncCount, 0);
    EXPECT_EQ(g_registerCalledCount, 0);
    EXPECT_EQ(g_registeredFfrtFunc, nullptr);
}

/**
 * @tc.name: RegisterFfrtStackFunc02
 * @tc.desc: ffrt_get_current_coroutine_stack symbol not found
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, RegisterFfrtStackFunc02, TestSize.Level1)
{
    g_mockRegisterFuncSymbol = reinterpret_cast<void *>(MockLibcGwpAsanRegisterFfrtStackFunc);
    g_mockFfrtFuncSymbol = nullptr;

    bool ret = RegisterFfrtStackFunc();

    EXPECT_FALSE(ret);
    EXPECT_EQ(g_dlsymRegisterFuncCount, 1);
    EXPECT_EQ(g_dlsymFfrtFuncCount, 1);
    EXPECT_EQ(g_registerCalledCount, 0);
    EXPECT_EQ(g_registeredFfrtFunc, nullptr);
}

/**
 * @tc.name: RegisterFfrtStackFunc03
 * @tc.desc: register ffrt_get_current_coroutine_stack successfully
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, RegisterFfrtStackFunc03, TestSize.Level1)
{
    g_mockRegisterFuncSymbol = reinterpret_cast<void *>(MockLibcGwpAsanRegisterFfrtStackFunc);
    g_mockFfrtFuncSymbol = reinterpret_cast<void *>(MockFfrtGetCurrentCoroutineStack);

    bool ret = RegisterFfrtStackFunc();

    EXPECT_TRUE(ret);
    EXPECT_EQ(g_dlsymRegisterFuncCount, 1);
    EXPECT_EQ(g_dlsymFfrtFuncCount, 1);
    EXPECT_EQ(g_registerCalledCount, 1);
    EXPECT_EQ(g_registeredFfrtFunc, MockFfrtGetCurrentCoroutineStack);
}

/**
 * @tc.name: SetAsanEnabledEnv01
 * @tc.desc: APP_FLAGS_HWASAN_ENABLED flag set, set HWASAN env
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, SetAsanEnabledEnv01, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");
        AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_HWASAN_ENABLED);

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        EXPECT_NE(CheckAppMsgFlagsSet(property, APP_FLAGS_HWASAN_ENABLED), 0);

        ret = SetAsanEnabledEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_STREQ(getenv("LD_PRELOAD"), "/system/lib64/libclang_rt.hwasan.so");
        EXPECT_STREQ(getenv("HWASAN_OPTIONS"), "include=/system/etc/asan.options");
        EXPECT_EQ(getenv("ASAN_OPTIONS"), nullptr);
        EXPECT_EQ(getenv("TSAN_OPTIONS"), nullptr);
        EXPECT_EQ(getenv("UBSAN_OPTIONS"), nullptr);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: SetAsanEnabledEnv02
 * @tc.desc: APP_FLAGS_ASANENABLED flag set, set ASAN env
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, SetAsanEnabledEnv02, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");
        AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_ASANENABLED);

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        EXPECT_NE(CheckAppMsgFlagsSet(property, APP_FLAGS_ASANENABLED), 0);

        ret = SetAsanEnabledEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_STREQ(getenv("LD_PRELOAD"), "/system/lib64/libclang_rt.asan.so");
        EXPECT_STREQ(getenv("ASAN_OPTIONS"), "include=/system/etc/asan.options");
        EXPECT_EQ(getenv("HWASAN_OPTIONS"), nullptr);
        EXPECT_EQ(getenv("TSAN_OPTIONS"), nullptr);
        EXPECT_EQ(getenv("UBSAN_OPTIONS"), nullptr);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: SetAsanEnabledEnv03
 * @tc.desc: APP_FLAGS_TSAN_ENABLED with render process type, skip tsan setting
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, SetAsanEnabledEnv03, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");
        AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_TSAN_ENABLED);
        int rc = AppSpawnReqMsgAddStringInfo(reqHandle, MSG_EXT_NAME_PROCESS_TYPE, "render");
        APPSPAWN_CHECK(rc == 0, break, "Failed to add process type");

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        EXPECT_NE(CheckAppMsgFlagsSet(property, APP_FLAGS_TSAN_ENABLED), 0);

        ret = SetAsanEnabledEnv(mgr, property);
        EXPECT_EQ(ret, -1);
        EXPECT_EQ(getenv("LD_PRELOAD"), nullptr);
        EXPECT_EQ(getenv("TSAN_OPTIONS"), nullptr);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: SetAsanEnabledEnv04
 * @tc.desc: APP_FLAGS_UBSAN_ENABLED flag set, set UBSAN env
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, SetAsanEnabledEnv04, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");
        AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_UBSAN_ENABLED);

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        EXPECT_NE(CheckAppMsgFlagsSet(property, APP_FLAGS_UBSAN_ENABLED), 0);

        ret = SetAsanEnabledEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_EQ(getenv("LD_PRELOAD"), nullptr);
        EXPECT_EQ(getenv("ASAN_OPTIONS"), nullptr);
        EXPECT_EQ(getenv("TSAN_OPTIONS"), nullptr);
        EXPECT_STREQ(getenv("UBSAN_OPTIONS"), "print_stacktrace=1:print_module_map=2:log_exe_name=1");
        EXPECT_EQ(getenv("HWASAN_OPTIONS"), nullptr);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: SetAsanEnabledEnv05
 * @tc.desc: no asan flag set, return -1
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, SetAsanEnabledEnv05, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");

        ret = SetAsanEnabledEnv(mgr, property);
        EXPECT_EQ(ret, -1);
        EXPECT_EQ(getenv("LD_PRELOAD"), nullptr);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: SetGwpAsanEnabled01
 * @tc.desc: no GWP_ENABLED_FORCE flag, enforce is 0
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, SetGwpAsanEnabled01, TestSize.Level1)
{
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        int ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        EXPECT_EQ(CheckAppMsgFlagsSet(property, APP_FLAGS_GWP_ENABLED_FORCE), 0);

        g_mockRegisterFuncSymbol = reinterpret_cast<void *>(MockLibcGwpAsanRegisterFfrtStackFunc);
        g_mockFfrtFuncSymbol = reinterpret_cast<void *>(MockFfrtGetCurrentCoroutineStack);

        SetGwpAsanEnabled(mgr, property);
        EXPECT_EQ(g_dlsymRegisterFuncCount, 1);
        EXPECT_EQ(g_dlsymFfrtFuncCount, 1);
        EXPECT_EQ(g_registerCalledCount, 1);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: SetGwpAsanEnabled02
 * @tc.desc: GWP_ENABLED_FORCE flag set, enforce is non-zero
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, SetGwpAsanEnabled02, TestSize.Level1)
{
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        int ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");
        AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_GWP_ENABLED_FORCE);

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        EXPECT_NE(CheckAppMsgFlagsSet(property, APP_FLAGS_GWP_ENABLED_FORCE), 0);

        g_mockRegisterFuncSymbol = nullptr;
        g_mockFfrtFuncSymbol = nullptr;

        SetGwpAsanEnabled(mgr, property);
        EXPECT_EQ(g_dlsymRegisterFuncCount, 1);
        EXPECT_EQ(g_registerCalledCount, 0);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: CheckSupportColdStart01
 * @tc.desc: wrap parameter set to asan_wrapper, return 0
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, CheckSupportColdStart01, TestSize.Level1)
{
    const char *bundleName = "com.example.myapplication";
    char key[128] = {0};
    snprintf_s(key, sizeof(key), sizeof(key) - 1, "wrap.%s", bundleName);
    SetParameter(key, "asan_wrapper");

    int ret = CheckSupportColdStart(bundleName);
    EXPECT_EQ(ret, 0);

    SetParameter(key, "");
}

/**
 * @tc.name: CheckSupportColdStart02
 * @tc.desc: wrap parameter set to other value, return -1
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, CheckSupportColdStart02, TestSize.Level1)
{
    const char *bundleName = "com.example.myapplication2";
    char key[128] = {0};
    snprintf_s(key, sizeof(key), sizeof(key) - 1, "wrap.%s", bundleName);
    SetParameter(key, "other_value");

    int ret = CheckSupportColdStart(bundleName);
    EXPECT_EQ(ret, -1);

    SetParameter(key, "");
}

/**
 * @tc.name: CheckSupportColdStart03
 * @tc.desc: wrap parameter not set, return -1
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, CheckSupportColdStart03, TestSize.Level1)
{
    const char *bundleName = "com.example.notexist.bundle";

    int ret = CheckSupportColdStart(bundleName);
    EXPECT_EQ(ret, -1);
}

/**
 * @tc.name: CheckSupportColdStart04
 * @tc.desc: bundleName too long, sprintf_s fail, return -1
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, CheckSupportColdStart04, TestSize.Level1)
{
    char longBundleName[256] = {0};
    memset_s(longBundleName, sizeof(longBundleName), 'a', sizeof(longBundleName) - 1);

    int ret = CheckSupportColdStart(longBundleName);
    EXPECT_EQ(ret, -1);
}

/**
 * @tc.name: AsanSpawnGetSpawningFlag01
 * @tc.desc: default build (no ASAN_DETECTOR), return 0 and flags unchanged
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, AsanSpawnGetSpawningFlag01, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        int rc = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(rc == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        uint32_t flagsBefore = property->client.flags;

        ret = AsanSpawnGetSpawningFlag(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_EQ(property->client.flags, flagsBefore);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: AsanSpawnInitSpawningEnv01
 * @tc.desc: MSG_SPAWN_NATIVE_PROCESS, return 0 early
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, AsanSpawnInitSpawningEnv01, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_SPAWN_NATIVE_PROCESS, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");

        ret = AsanSpawnInitSpawningEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_EQ(getenv("LD_PRELOAD"), nullptr);
        EXPECT_EQ(property->client.flags & APP_COLD_START, 0);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: AsanSpawnInitSpawningEnv02
 * @tc.desc: MSG_APP_SPAWN with ASANENABLED flag, set asan env and cold start
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, AsanSpawnInitSpawningEnv02, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");
        AppSpawnReqMsgSetAppFlag(reqHandle, APP_FLAGS_ASANENABLED);

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");

        ret = AsanSpawnInitSpawningEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_NE(property->client.flags & APP_COLD_START, 0);
        EXPECT_STREQ(getenv("LD_PRELOAD"), "/system/lib64/libclang_rt.asan.so");
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: AsanSpawnInitSpawningEnv03
 * @tc.desc: MSG_APP_SPAWN with wrap parameter set, set hwasan env and cold start
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, AsanSpawnInitSpawningEnv03, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        const char *bundleName = "com.example.myapplication";
        char key[128] = {0};
        snprintf_s(key, sizeof(key), sizeof(key) - 1, "wrap.%s", bundleName);
        SetParameter(key, "asan_wrapper");

        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");

        ret = AsanSpawnInitSpawningEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_NE(property->client.flags & APP_COLD_START, 0);
        EXPECT_STREQ(getenv("LD_PRELOAD"), "/system/lib64/libclang_rt.hwasan.so");

        SetParameter(key, "");
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}

/**
 * @tc.name: AsanSpawnInitSpawningEnv04
 * @tc.desc: MSG_APP_SPAWN with no flag and no wrap, SetAsanEnabledEnv returns -1
 * @tc.type: FUNC
 */
HWTEST_F(AppSpawnAsanDetectorTest, AsanSpawnInitSpawningEnv04, TestSize.Level1)
{
    int ret = -1;
    AppSpawnMgr *mgr = nullptr;
    AppSpawnClientHandle clientHandle = nullptr;
    AppSpawningCtx *property = nullptr;
    do {
        mgr = CreateAppSpawnMgr(MODE_FOR_APP_SPAWN);
        ASSERT_NE(mgr, nullptr);

        ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
        APPSPAWN_CHECK(ret == 0, break, "Failed to create client");

        SpawningTestHelper helper;
        AppSpawnReqMsgHandle reqHandle = helper.SpawningTestCreateSendMsg(clientHandle, MSG_APP_SPAWN, 0);
        APPSPAWN_CHECK(reqHandle != INVALID_REQ_HANDLE, break, "Failed to create msg");

        property = helper.SpawningTestGetAppProperty(clientHandle, reqHandle);
        APPSPAWN_CHECK(property != nullptr, break, "Failed to get app property");
        uint32_t flagsBefore = property->client.flags;

        ret = AsanSpawnInitSpawningEnv(mgr, property);
        EXPECT_EQ(ret, 0);
        EXPECT_EQ(property->client.flags, flagsBefore);
        EXPECT_EQ(getenv("LD_PRELOAD"), nullptr);
    } while (0);
    DeleteAppSpawningCtx(property);
    AppSpawnClientDestroy(clientHandle);
    DeleteAppSpawnMgr(mgr);
}
}   // namespace OHOS
