/*
 * Copyright (c) 2025-2026 Huawei Device Co., Ltd.
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

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include "securec.h"
#include "appspawn_utils.h"
#include "appspawn_msg.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "json_utils.h"
#include "appspawn_isolate.h"

typedef struct {
    const char *name;
    uint32_t value;
} FlagEntry;

static FlagEntry g_mountFlagMap[] = {
    {"ugc", SEC_UGC_PATH_TYPE},
    {"isolate", SEC_ISOLATE_PATH_TYPE},
    {"dec", SEC_DEC_PATH_TYPE},
    {"sandbox", SEC_SANDBOX_PATH_TYPE},
};

static FlagEntry g_pathModeMap[] = {
    {"ugc", UGC_CONSTRAINT_MODE},
};

APPSPAWN_STATIC SandboxPathMarkConfig *GetSandboxPathMarkConfig(const AppSpawnMgr *content)
{
    return (SandboxPathMarkConfig *)FindAppSpawnExtData(content, EXT_DATA_PATH_MARK);
}

static void ClearSandboxPathMarkConfig(SandboxPathMarkConfig *config)
{
    APPSPAWN_CHECK_ONLY_EXPER(config != NULL, return);
    if (config->mounts != NULL) {
        for (uint32_t i = 0; i < config->mountCount; i++) {
            free(config->mounts[i].path);
        }
        free(config->mounts);
        config->mounts = NULL;
        config->mountCount = 0;
    }
    if (config->paths != NULL) {
        for (uint32_t i = 0; i < config->pathCount; i++) {
            free(config->paths[i].path);
        }
        free(config->paths);
        config->paths = NULL;
        config->pathCount = 0;
    }
}

// freeNode只清理内部资源, 摘链与释放宿主内存由框架统一完成
static void FreeSandboxPathMarkConfig(struct TagAppSpawnExtData *data)
{
    APPSPAWN_CHECK_ONLY_EXPER(data != NULL, return);
    ClearSandboxPathMarkConfig((SandboxPathMarkConfig *)data);
}

static SandboxPathMarkConfig *CreateSandboxPathMarkConfig(AppSpawnMgr *content)
{
    return (SandboxPathMarkConfig *)CreateAppSpawnExtData(
        content, EXT_DATA_PATH_MARK, sizeof(SandboxPathMarkConfig),
        FreeSandboxPathMarkConfig, NULL);
}

static int32_t LookupValueByName(const char *name, const FlagEntry *map, size_t mapSize)
{
    APPSPAWN_CHECK_ONLY_EXPER(name != NULL, return -1);
    for (size_t i = 0; i < mapSize; i++) {
        if (strcmp(name, map[i].name) == 0) {
            return (int32_t)map[i].value;
        }
    }
    return -1;
}

static uint32_t ParseFlagsFromArray(const cJSON *item, const FlagEntry *map, size_t mapSize,
    const char *pathForLog)
{
    uint32_t result = 0;
    cJSON *flagsArray = cJSON_GetObjectItemCaseSensitive(item, "flags");
    if (flagsArray != NULL && cJSON_IsArray(flagsArray)) {
        int flagCount = cJSON_GetArraySize(flagsArray);
        for (int j = 0; j < flagCount; j++) {
            char *flagName = cJSON_GetStringValue(cJSON_GetArrayItem(flagsArray, j));
            int32_t val = LookupValueByName(flagName, map, mapSize);
            APPSPAWN_CHECK(val >= 0, continue, "Invalid flag %{public}s for path %{public}s", flagName, pathForLog);
            result |= (uint32_t)val;
        }
    }
    return result;
}

static cJSON *GetJsonArraySection(const cJSON *root, const char *key, int *outCount)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key);
    APPSPAWN_CHECK_ONLY_EXPER(arr != NULL && cJSON_IsArray(arr), return NULL);
    *outCount = cJSON_GetArraySize(arr);
    APPSPAWN_CHECK_ONLY_EXPER(*outCount > 0, return NULL);
    APPSPAWN_CHECK(*outCount <= MAX_PATH_RULE_NUM, return NULL,
        "%{public}s count %{public}d exceeds max %{public}d", key, *outCount, MAX_PATH_RULE_NUM);
    return arr;
}

static void ParseMountsSection(const cJSON *root, SandboxPathMarkConfig *config)
{
    int count = 0;
    cJSON *mounts = GetJsonArraySection(root, "mounts", &count);
    APPSPAWN_CHECK_ONLY_EXPER(mounts != NULL, return);
    config->mounts = (MountInfo *)calloc(count, sizeof(MountInfo));
    APPSPAWN_CHECK(config->mounts != NULL, return, "calloc path marks failed");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(mounts, i);
        char *path = GetStringFromJsonObj(item, "path");
        APPSPAWN_CHECK(path != NULL, continue, "mounts entry path missing, skip");
        uint32_t flags = ParseFlagsFromArray(item, g_mountFlagMap, ARRAY_LENGTH(g_mountFlagMap), path);
        APPSPAWN_CHECK(flags != 0, continue, "No valid flags for path %{public}s, skip", path);
        config->mounts[config->mountCount].path = strdup(path);
        APPSPAWN_CHECK(config->mounts[config->mountCount].path != NULL, continue,
            "strdup failed for path %{public}s, skip", path);
        config->mounts[config->mountCount].flags = flags;
        config->mounts[config->mountCount].recursive =
            GetBoolValueFromJsonObj(item, "recursive", false) ? MARK_ENABLE_RECURSIVE : 0;
        config->mountCount++;
    }
    APPSPAWN_LOGV("Parsed mounts: %{public}u entries", config->mountCount);
}

static void ParsePathsSection(const cJSON *root, SandboxPathMarkConfig *config)
{
    int count = 0;
    cJSON *paths = GetJsonArraySection(root, "paths", &count);
    APPSPAWN_CHECK_ONLY_EXPER(paths != NULL, return);
    config->paths = (PathInfo *)calloc(count, sizeof(PathInfo));
    APPSPAWN_CHECK(config->paths != NULL, return, "calloc paths failed");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(paths, i);
        char *path = GetStringFromJsonObj(item, "path");
        APPSPAWN_CHECK(path != NULL, continue, "paths entry path missing, skip");
        uint32_t mode = ParseFlagsFromArray(item, g_pathModeMap, ARRAY_LENGTH(g_pathModeMap), path);
        APPSPAWN_CHECK(mode != 0, continue, "No valid modes for path %{public}s, skip", path);
        config->paths[config->pathCount].path = strdup(path);
        APPSPAWN_CHECK(config->paths[config->pathCount].path != NULL, continue,
            "strdup failed for path %{public}s, skip", path);
        config->paths[config->pathCount].pathLen = (uint32_t)strlen(path);
        config->paths[config->pathCount].mode = mode;
        config->paths[config->pathCount].flag = false;
        config->pathCount++;
    }
    APPSPAWN_LOGV("Parsed paths: %{public}u entries", config->pathCount);
}

APPSPAWN_STATIC int ParsePathConfig(const cJSON *root, ParseJsonContext *context)
{
    APPSPAWN_CHECK(root != NULL && context != NULL && context->config != NULL,
        return APPSPAWN_SYSTEM_ERROR, "Invalid json root or context");
    SandboxPathMarkConfig *config = context->config;
    ClearSandboxPathMarkConfig(config);
    ParseMountsSection(root, config);
    ParsePathsSection(root, config);
    return APPSPAWN_OK;
}

void SetConstraintPaths(PathInfo *paths, uint32_t count)
{
    APPSPAWN_CHECK_ONLY_EXPER(paths != NULL && count != 0, return);
    int fd = open(DEV_DEC, O_RDWR);
    APPSPAWN_CHECK(fd >= 0, return, "Failed to open %{public}s for constraint, errno %{public}d", DEV_DEC, errno);
    uint32_t batches = (count + KERNEL_BATCH_SIZE - 1) / KERNEL_BATCH_SIZE;
    for (uint32_t batch = 0; batch < batches; batch++) {
        uint32_t start = batch * KERNEL_BATCH_SIZE;
        uint32_t end = start + KERNEL_BATCH_SIZE;
        APPSPAWN_ONLY_EXPER(end > count, end = count);
        IoctlDecPolicyBatch info = {0};
        info.pathNum = end - start;
        for (uint32_t i = start; i < end; i++) {
            info.path[i - start] = paths[i];
        }
        if (ioctl(fd, CONSTRAINT_DEC_POLICY_CMD, &info) < 0) {
            APPSPAWN_LOGE("CONSTRAINT_DEC_POLICY_CMD failed batch %{public}u errno %{public}d", batch, errno);
        }
    }
    APPSPAWN_LOGI("set CONSTRAINT_DEC_POLICY_CMD success");
    close(fd);
}

static void SetMark(MountInfo *mounts, uint32_t count)
{
    APPSPAWN_CHECK(mounts != NULL && count != 0, return, "Invalid parameters for SetMark");
    int fd = open(DEV_DEC, O_RDWR);
    APPSPAWN_CHECK(fd >= 0, return, "Failed to open %{public}s: %{public}s, skipping path mark",
        DEV_DEC, strerror(errno));
    for (uint32_t i = 0; i < count; i++) {
        if (ioctl(fd, ADD_PATH_MARK_CMD, &mounts[i]) < 0) {
            APPSPAWN_LOGE("Set %{public}s 0x%{public}x mark failed errno %{public}d",
                mounts[i].path, mounts[i].flags, errno);
        } else {
            APPSPAWN_LOGV("Set %{public}s 0x%{public}x mark success", mounts[i].path, mounts[i].flags);
        }
    }
    close(fd);
}

static int SpawnSetPathMark(AppSpawnMgr *content, AppSpawningCtx *property)
{
    if (IsNWebSpawnMode(content) || GetAppSpawnMsgType(property) == MSG_SPAWN_NATIVE_PROCESS) {
        return APPSPAWN_OK;
    }
    SandboxPathMarkConfig *config = GetSandboxPathMarkConfig(content);
    if (config != NULL) {
        SetMark(config->mounts, config->mountCount);
    }
    return APPSPAWN_OK;
}

int SpawnLoadPathConfig(AppSpawnMgr *content)
{
    APPSPAWN_CHECK(content != NULL, return APPSPAWN_SYSTEM_ERROR, "Invalid content");
    // 已加载过则直接返回, 不重复解析配置, 也不重复下发内核策略
    SandboxPathMarkConfig *config = GetSandboxPathMarkConfig(content);
    APPSPAWN_ONLY_EXPER(config != NULL, return APPSPAWN_OK);
    config = CreateSandboxPathMarkConfig(content);
    APPSPAWN_CHECK(config != NULL, return APPSPAWN_SYSTEM_ERROR, "Failed to create sandbox path mark config");

    ParseJsonContext ctx = { .config = config };
    (void)ParseJsonConfig("etc/sandbox", "/sandbox-path-mark-policy.json", ParsePathConfig, &ctx);
    APPSPAWN_LOGV("Config loaded: mounts=%{public}u paths=%{public}u", config->mountCount, config->pathCount);
    if (config->pathCount > 0) {
        SetConstraintPaths(config->paths, config->pathCount);
    }
    return APPSPAWN_OK;
}

MODULE_CONSTRUCTOR(void)
{
    AddPreloadHook(HOOK_PRIO_COMMON, SpawnLoadPathConfig);
    /* Path marking should be done between sandbox and setuid. */
    AddAppSpawnHook(STAGE_CHILD_EXECUTE, HOOK_PRIO_SANDBOX_MARK_PATH, SpawnSetPathMark);
}
