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

#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>

#include "appspawn_error.h"
#include "appspawn_msg.h"
#include "appspawn_utils.h"
#include "securec.h"

#ifdef __MUSL__
static int LoadGroup(const cJSON *group, bool isSystemApp)
{
    const cJSON *libPath = cJSON_GetObjectItemCaseSensitive(group, "libPath");
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(group, "so");
    const char *ns = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(group, "ns"));
    APPSPAWN_CHECK(cJSON_IsArray(libPath) && cJSON_IsArray(libraries) && ns != NULL && ns[0] != '\0',
        return APPSPAWN_SUBSPAWN_PRELOAD_FAILED, "Invalid subspawn preload group");
    int result = SubspawnSetAppLibPath(ns, libPath, isSystemApp);
    APPSPAWN_CHECK_ONLY_EXPER(result == 0, return result);
    char name[APP_LEN_PROC_NAME];
    Dl_namespace handle = {0};
    APPSPAWN_CHECK(snprintf_s(name, sizeof(name), sizeof(name) - 1, "moduleNs_%s", ns) > 0,
        return APPSPAWN_SUBSPAWN_PRELOAD_FAILED, "Invalid subspawn namespace name");
    APPSPAWN_CHECK(dlns_get(name, &handle) == 0,
        return APPSPAWN_SUBSPAWN_PRELOAD_FAILED, "Subspawn NAPI namespace lookup failed");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, libraries) {
        const char *library = cJSON_GetStringValue(item);
        APPSPAWN_CHECK(library != NULL, return APPSPAWN_SUBSPAWN_PRELOAD_FAILED,
            "Invalid subspawn so for %{public}s", ns);
        void *libHandle = dlopen_ns(&handle, library, RTLD_NOW | RTLD_GLOBAL);
        APPSPAWN_CHECK(libHandle != NULL, return APPSPAWN_SUBSPAWN_PRELOAD_FAILED,
            "Failed to preload subspawn library %{public}s: %{public}s", library, dlerror());
        // Do not dlclose: the instance and its forked children retain the mapping.
        APPSPAWN_LOGV("Subspawn preload %{public}s module %{public}s", library, ns);
    }
    return 0;
}

static bool IsDuplicateNamespace(const cJSON *config, const cJSON *current)
{
    const char *ns = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(current, "ns"));
    if (ns == NULL) {
        return false;
    }
    const cJSON *group = NULL;
    cJSON_ArrayForEach(group, config) {
        if (group == current) {
            return false;
        }
        const char *prev = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(group, "ns"));
        if (prev != NULL && strcmp(prev, ns) == 0) {
            APPSPAWN_LOGE("Duplicate subspawn preload namespace %{public}s", ns);
            return true;
        }
    }
    return false;
}
#endif

int SubspawnPreloadLibraries(const cJSON *config, bool isSystemApp)
{
#ifdef __MUSL__
    APPSPAWN_CHECK(cJSON_IsArray(config), return APPSPAWN_SUBSPAWN_PRELOAD_FAILED,
        "Invalid subspawn preload schema");
    const cJSON *group = NULL;
    cJSON_ArrayForEach(group, config) {
        APPSPAWN_CHECK_ONLY_EXPER(!IsDuplicateNamespace(config, group), return APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
        int result = LoadGroup(group, isSystemApp);
        APPSPAWN_CHECK_ONLY_EXPER(result == 0, return result);
    }
    return 0;
#else
    (void)config;
    (void)isSystemApp;
    APPSPAWN_LOGE("Subspawn preload requires the namespace loader");
    return APPSPAWN_SUBSPAWN_PRELOAD_FAILED;
#endif
}
