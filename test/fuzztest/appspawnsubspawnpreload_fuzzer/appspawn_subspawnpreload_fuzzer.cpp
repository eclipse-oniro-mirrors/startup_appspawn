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

#include "appspawn_subspawnpreload_fuzzer.h"
#include <dlfcn.h>
#include <string>
#include <vector>
#include "module_manager/native_module_manager.h"
#include "subspawn_preload.h"

namespace {
constexpr uint8_t MODULE_MANAGER_FAILURE_FLAG = 1;
constexpr uint8_t LD_NAMESPACE_NAME_FAILURE_FLAG = 2;
#ifdef __MUSL__
constexpr uint8_t DLNS_GET_FAILURE_FLAG = 4;
constexpr uint8_t DLOPEN_NS_FAILURE_FLAG = 8;
#endif
constexpr uint8_t SYSTEM_APP_FLAG = 16;
uint8_t g_failureFlags = 0;
}

// 沿用 subspawn UT 的替换方式，保留 subspawn_napi.cpp 的实际路径转换和校验。
class SubspawnModuleManagerMock {
public:
    static SubspawnModuleManagerMock *GetInstance()
    {
        static SubspawnModuleManagerMock manager;
        return (g_failureFlags & MODULE_MANAGER_FAILURE_FLAG) != 0 ? nullptr : &manager;
    }

    void SetAppLibPath(const std::string &module, const std::vector<std::string> &paths, bool isSystemApp)
    {
        // 不注册真实 namespace，避免跨 fuzz 迭代积累进程状态。
    }

    bool GetLdNamespaceName(const std::string &module, std::string &name)
    {
        name = "moduleNs_" + module;
        return (g_failureFlags & LD_NAMESPACE_NAME_FAILURE_FLAG) == 0;
    }
};

#define NativeModuleManager SubspawnModuleManagerMock
#include "subspawn_napi.cpp"
#undef NativeModuleManager

#ifdef __MUSL__
extern "C" int __wrap_dlns_get(const char *name, Dl_namespace *handle)
{
    return (g_failureFlags & DLNS_GET_FAILURE_FLAG) != 0 ? -1 : 0;
}

extern "C" void *__wrap_dlopen_ns(Dl_namespace *handle, const char *name, int flags)
{
    return (g_failureFlags & DLOPEN_NS_FAILURE_FLAG) != 0 ? nullptr : &g_failureFlags;
}
#endif

namespace OHOS {
    int FuzzSubspawnPreloadLibraries(const uint8_t *data, size_t size)
    {
        if (size == 0) {
            return 0;
        }
        // 首字节控制依赖失败及系统应用标志，其余字节为 JSON；每轮重置，避免输入间相互影响。
        g_failureFlags = data[0];
        std::string json(reinterpret_cast<const char *>(data + 1), size - 1);
        cJSON *config = cJSON_Parse(json.c_str());
        int ret = SubspawnPreloadLibraries(config, (g_failureFlags & SYSTEM_APP_FLAG) != 0);
        cJSON_Delete(config);
        return ret;
    }
}

/* Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    OHOS::FuzzSubspawnPreloadLibraries(data, size);
    return 0;
}
