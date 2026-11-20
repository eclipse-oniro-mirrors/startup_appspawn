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

#include <dlfcn.h>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include "appspawn_error.h"
#include "appspawn.h"
#include "appspawn_msg.h"
#include "module_manager/native_module_manager.h"
#include "subspawn_preload.h"

namespace {
struct PreloadMock {
    bool managerAvailable = true;
    bool registered = true;
    bool namespaceAvailable = true;
    bool loadFail = true;
    bool isSystemApp = true;
    std::string failLibrary;
    std::vector<std::string> modules;
    std::vector<std::string> paths;
    std::vector<std::string> namespaces;
    std::vector<std::string> libraries;
    std::vector<int> loadFlags;
    std::vector<bool> systemAppFlags;
};

PreloadMock g_preloadMock;
} // namespace

// 仅替换 NAPI 依赖，以下测试仍执行 subspawn_napi.cpp 的实际路径转换及校验代码。
class SubspawnModuleManagerMock {
public:
    static SubspawnModuleManagerMock *GetInstance()
    {
        static SubspawnModuleManagerMock manager;
        return g_preloadMock.managerAvailable ? &manager : nullptr;
    }

    void SetAppLibPath(const std::string &module, const std::vector<std::string> &paths, bool isSystemApp)
    {
        g_preloadMock.modules.push_back(module);
        g_preloadMock.paths = paths;
        g_preloadMock.isSystemApp = isSystemApp;
        g_preloadMock.systemAppFlags.push_back(isSystemApp);
    }

    bool GetLdNamespaceName(const std::string &module, std::string &name)
    {
        name = "moduleNs_" + module;
        return g_preloadMock.registered;
    }
};

#define NativeModuleManager SubspawnModuleManagerMock
#include "subspawn_napi.cpp"
#undef NativeModuleManager

void ResetSubspawnPreloadMock()
{
    g_preloadMock = PreloadMock();
}

bool GetSubspawnPreloadSystemAppForTest()
{
    return g_preloadMock.isSystemApp;
}

#ifdef __MUSL__
// __wrap_* names are fixed by the linker --wrap contract, so the
// reserved-identifier and naming checks do not apply to them.
// NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming)
extern "C" int __wrap_dlns_get(const char *name, Dl_namespace *handle)
{
    g_preloadMock.namespaces.emplace_back(name);
    return g_preloadMock.namespaceAvailable ? 0 : -1;
}

extern "C" void *__wrap_dlopen_ns(Dl_namespace *handle, const char *name, int flags)
{
    g_preloadMock.libraries.emplace_back(name);
    g_preloadMock.loadFlags.push_back(flags);
    if (g_preloadMock.loadFail || g_preloadMock.failLibrary == name) {
        return nullptr;
    }
    return &g_preloadMock;
}
// NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming)
#endif

namespace OHOS {
using namespace testing;
using namespace testing::ext;
using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

class SubspawnPreloadTest : public testing::Test {
public:
    void SetUp() override
    {
        ResetSubspawnPreloadMock();
        g_preloadMock.loadFail = false;
    }

    void TearDown() override
    {
        ResetSubspawnPreloadMock();
    }

    int Preload(const char *json, bool isSystemApp = false)
    {
        JsonPtr config(cJSON_Parse(json), cJSON_Delete);
        return SubspawnPreloadLibraries(config.get(), isSystemApp);
    }
};

HWTEST_F(SubspawnPreloadTest, NapiRegistersPathsInOrder, TestSize.Level0)
{
    JsonPtr paths(cJSON_Parse("[\"/first\",\"\",\"/second\"]"), cJSON_Delete);
    ASSERT_EQ(SubspawnSetAppLibPath("sample", paths.get(), false), 0);
    EXPECT_EQ(g_preloadMock.modules, std::vector<std::string>({"sample"}));
    EXPECT_EQ(g_preloadMock.paths, std::vector<std::string>({"/first", "/second"}));
    EXPECT_FALSE(g_preloadMock.isSystemApp);
}

HWTEST_F(SubspawnPreloadTest, NapiPreservesSystemAppFlag, TestSize.Level0)
{
    JsonPtr paths(cJSON_Parse("[\"/lib\"]"), cJSON_Delete);
    ASSERT_EQ(SubspawnSetAppLibPath("sample", paths.get(), true), 0);
    EXPECT_TRUE(g_preloadMock.isSystemApp);
}

HWTEST_F(SubspawnPreloadTest, NapiRejectsNonArrayAndInvalidElement, TestSize.Level0)
{
    for (const char *json : {"{}", "[null]", "[\"/valid\",false]"}) {
        SCOPED_TRACE(json);
        JsonPtr paths(cJSON_Parse(json), cJSON_Delete);
        EXPECT_EQ(SubspawnSetAppLibPath("sample", paths.get(), false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    }
    EXPECT_TRUE(g_preloadMock.modules.empty());
}

HWTEST_F(SubspawnPreloadTest, NapiRejectsMissingManager, TestSize.Level0)
{
    JsonPtr paths(cJSON_Parse("[\"/lib\"]"), cJSON_Delete);
    g_preloadMock.managerAvailable = false;
    EXPECT_EQ(SubspawnSetAppLibPath("sample", paths.get(), false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_TRUE(g_preloadMock.modules.empty());
}

HWTEST_F(SubspawnPreloadTest, NapiRejectsRegistrationFailure, TestSize.Level0)
{
    JsonPtr paths(cJSON_Parse("[\"/lib\"]"), cJSON_Delete);
    g_preloadMock.registered = false;
    EXPECT_EQ(SubspawnSetAppLibPath("sample", paths.get(), false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(g_preloadMock.modules.size(), 1U);
}

#ifdef __MUSL__
HWTEST_F(SubspawnPreloadTest, RejectsNonArraySchema, TestSize.Level0)
{
    for (const char *json : {"null", "{}", "1"}) {
        SCOPED_TRACE(json);
        EXPECT_EQ(Preload(json), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    }
    EXPECT_EQ(SubspawnPreloadLibraries(nullptr, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_TRUE(g_preloadMock.modules.empty());
}

HWTEST_F(SubspawnPreloadTest, EmptyConfigurationLoadsNothing, TestSize.Level0)
{
    EXPECT_EQ(Preload("[]"), 0);
    EXPECT_TRUE(g_preloadMock.modules.empty());
    EXPECT_TRUE(g_preloadMock.libraries.empty());
}

HWTEST_F(SubspawnPreloadTest, RejectsInvalidGroupBeforeLoading, TestSize.Level0)
{
    const char *cases[] = {
        "[1]",
        "[{\"so\":[],\"ns\":\"one\"}]",
        "[{\"libPath\":[],\"ns\":\"one\"}]",
        "[{\"libPath\":[],\"so\":[]}]",
        "[{\"libPath\":[],\"so\":[],\"ns\":1}]",
        "[{\"libPath\":[],\"so\":[],\"ns\":\"\"}]",
        "[{\"libPath\":1,\"so\":[],\"ns\":\"one\"}]",
        "[{\"libPath\":[],\"so\":1,\"ns\":\"one\"}]",
        "[{\"libPath\":[],\"so\":[],\"ns\":\"one\"}]",
    };
    for (const char *json : cases) {
        SCOPED_TRACE(json);
        EXPECT_EQ(Preload(json), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    }
    EXPECT_TRUE(g_preloadMock.libraries.empty());
}

HWTEST_F(SubspawnPreloadTest, LoadsMultipleNamespacesAndLibraries, TestSize.Level0)
{
    ASSERT_EQ(Preload("[{\"libPath\":[\"/one\"],\"so\":[\"first.so\",\"second.so\"],\"ns\":\"one\"},"
        "{\"libPath\":[\"/two\"],\"so\":[\"third.so\"],\"ns\":\"two\"}]"), 0);
    EXPECT_EQ(g_preloadMock.modules, std::vector<std::string>({"one", "two"}));
    EXPECT_EQ(g_preloadMock.namespaces, std::vector<std::string>({"moduleNs_one", "moduleNs_two"}));
    EXPECT_EQ(g_preloadMock.libraries, std::vector<std::string>({"first.so", "second.so", "third.so"}));
    for (int flags : g_preloadMock.loadFlags) {
        EXPECT_EQ(flags, RTLD_NOW | RTLD_GLOBAL);
    }
}

HWTEST_F(SubspawnPreloadTest, SystemAppFlagReachesEveryNamespace, TestSize.Level0)
{
    ASSERT_EQ(Preload("[{\"libPath\":[\"/one\"],\"so\":[],\"ns\":\"one\"},"
        "{\"libPath\":[\"/two\"],\"so\":[],\"ns\":\"two\"}]", true), 0);
    EXPECT_EQ(g_preloadMock.systemAppFlags, std::vector<bool>({true, true}));
}

HWTEST_F(SubspawnPreloadTest, RejectsDuplicateNamespaceBeforeSecondRegistration, TestSize.Level0)
{
    EXPECT_EQ(Preload("[{\"libPath\":[\"/one\"],\"so\":[\"first.so\"],\"ns\":\"same\"},"
        "{\"libPath\":[\"/two\"],\"so\":[\"second.so\"],\"ns\":\"same\"}]"),
        APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(g_preloadMock.modules.size(), 1U);
    EXPECT_EQ(g_preloadMock.libraries, std::vector<std::string>({"first.so"}));
}

HWTEST_F(SubspawnPreloadTest, StopsWhenNamespaceLookupFails, TestSize.Level0)
{
    g_preloadMock.namespaceAvailable = false;
    EXPECT_EQ(Preload("[{\"libPath\":[\"/lib\"],\"so\":[\"first.so\"],\"ns\":\"one\"}]"),
        APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(g_preloadMock.namespaces.size(), 1U);
    EXPECT_TRUE(g_preloadMock.libraries.empty());
}

HWTEST_F(SubspawnPreloadTest, StopsAtFirstLibraryFailure, TestSize.Level0)
{
    g_preloadMock.failLibrary = "second.so";
    EXPECT_EQ(Preload("[{\"libPath\":[\"/lib\"],\"so\":[\"first.so\",\"second.so\",\"third.so\"],"
        "\"ns\":\"one\"}]"), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(g_preloadMock.libraries, std::vector<std::string>({"first.so", "second.so"}));
}

HWTEST_F(SubspawnPreloadTest, RejectsInvalidLibraryElement, TestSize.Level0)
{
    EXPECT_EQ(Preload("[{\"libPath\":[\"/lib\"],\"so\":[\"first.so\",1,\"third.so\"],\"ns\":\"one\"}]"),
        APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(g_preloadMock.libraries, std::vector<std::string>({"first.so"}));
}

HWTEST_F(SubspawnPreloadTest, EmptyLibraryListRegistersNamespaceOnly, TestSize.Level0)
{
    EXPECT_EQ(Preload("[{\"libPath\":[\"/lib\"],\"so\":[],\"ns\":\"one\"}]"), 0);
    EXPECT_EQ(g_preloadMock.namespaces.size(), 1U);
    EXPECT_TRUE(g_preloadMock.libraries.empty());
}

HWTEST_F(SubspawnPreloadTest, RejectsNamespaceNameOverflow, TestSize.Level0)
{
    std::string json = "[{\"libPath\":[\"/lib\"],\"so\":[],\"ns\":\"";
    json.append(APP_LEN_PROC_NAME, 'x');
    json += "\"}]";
    EXPECT_EQ(Preload(json.c_str()), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_TRUE(g_preloadMock.namespaces.empty());
}
#endif
} // namespace OHOS
