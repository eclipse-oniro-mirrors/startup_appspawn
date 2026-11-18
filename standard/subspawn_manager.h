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

#ifndef SUBSPAWN_MANAGER_H
#define SUBSPAWN_MANAGER_H

#include "appspawn_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse before StartSpawnService overwrites argv; only inherited descriptors are accepted. */
int SubspawnEarlyParse(int argc, char *const argv[]);
/* Restore the instance's long and short process names after service initialization. */
int SubspawnSetProcessName(AppSpawnContent *content);
const SubspawnStartInfo *GetSubspawnStartInfo(void);
void SubspawnNotifyReady(int result);
void SubspawnSetStartResult(int result);
int SubspawnGetStartResult(void);

#ifdef __cplusplus
}
#endif
#endif  // SUBSPAWN_MANAGER_H
