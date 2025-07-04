/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#ifndef SANDBOX_ADPATER_H
#define SANDBOX_ADPATER_H

#include "appspawn_sandbox.h"

#ifdef __cplusplus
extern "C" {
#endif

void MakeAtomicServiceDir(const SandboxContext *context, const char *originPath, const char *varPackageName);

#ifdef __cplusplus
}
#endif
#endif
