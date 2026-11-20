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

#include "subspawn_preload.h"

#include <string>
#include <vector>

#include "appspawn_error.h"
#include "appspawn_utils.h"
#include "module_manager/native_module_manager.h"

int SubspawnSetAppLibPath(const char *moduleName, const cJSON *libPaths, bool isSystemApp)
{
    APPSPAWN_CHECK(moduleName != nullptr && moduleName[0] != '\0' && cJSON_IsArray(libPaths),
        return APPSPAWN_SUBSPAWN_PRELOAD_FAILED, "Invalid subspawn NAPI library paths");
    std::vector<std::string> libraryPaths;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, libPaths) {
        const char *path = cJSON_GetStringValue(item);
        APPSPAWN_CHECK(path != nullptr, return APPSPAWN_SUBSPAWN_PRELOAD_FAILED,
            "Invalid subspawn libPath for %{public}s", moduleName);
        if (path[0] != '\0') {
            libraryPaths.emplace_back(path);
        }
    }
    APPSPAWN_CHECK(!libraryPaths.empty(), return APPSPAWN_SUBSPAWN_PRELOAD_FAILED,
        "Invalid subspawn NAPI library paths");
    NativeModuleManager *manager = NativeModuleManager::GetInstance();
    APPSPAWN_CHECK(manager != nullptr, return APPSPAWN_SUBSPAWN_PRELOAD_FAILED,
        "Subspawn NAPI module manager unavailable");
    manager->SetAppLibPath(moduleName, libraryPaths, isSystemApp);
    std::string namespaceName;
    APPSPAWN_CHECK(manager->GetLdNamespaceName(moduleName, namespaceName),
        return APPSPAWN_SUBSPAWN_PRELOAD_FAILED, "Subspawn NAPI module path registration failed");
    return 0;
}
