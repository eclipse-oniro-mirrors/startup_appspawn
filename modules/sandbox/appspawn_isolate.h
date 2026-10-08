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

#ifndef APPSPAWN_ISOLATE_H
#define APPSPAWN_ISOLATE_H
#include "appspawn_hook.h"
#include "sandbox_dec.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

/* Path-mark flag bits & markers */
#define SEC_UGC_PATH_TYPE     (1 << 0)
#define SEC_ISOLATE_PATH_TYPE (1 << 1)
#define SEC_DEC_PATH_TYPE     (1 << 2)
#define SEC_SANDBOX_PATH_TYPE (1 << 3)
#define UGC_CONSTRAINT_MODE  1
#define MARK_ENABLE_RECURSIVE 1

#define MARK_PATH_INFO_RESERVED_SIZE 7

typedef struct {
    char *path;
    uint32_t flags;
    uint32_t recursive;
    uint32_t reserved[MARK_PATH_INFO_RESERVED_SIZE];
} MountInfo;

typedef struct {
    AppSpawnExtData extData;
    MountInfo *mounts;
    uint32_t mountCount;
    PathInfo *paths;
    uint32_t pathCount;
} SandboxPathMarkConfig;

struct TagParseJsonContext {
    SandboxPathMarkConfig *config;
};

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif
