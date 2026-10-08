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

#ifndef APP_SPAWN_ISOLATE_TEST_H
#define APP_SPAWN_ISOLATE_TEST_H

#include "appspawn_isolate.h"
#include "json_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

SandboxPathMarkConfig *GetSandboxPathMarkConfig(const AppSpawnMgr *content);
int ParsePathConfig(const cJSON *root, ParseJsonContext *context);
void SetConstraintPaths(PathInfo *paths, uint32_t count);
int SpawnLoadPathConfig(AppSpawnMgr *content);

#ifdef __cplusplus
}
#endif

#endif /* APP_SPAWN_ISOLATE_TEST_H */
