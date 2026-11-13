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
#ifndef APPSPAWN_CAPABILITY_MOCK_H
#define APPSPAWN_CAPABILITY_MOCK_H

#include <stdint.h>
#include <sys/capability.h>

#include "appspawn_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Records attempted capability changes, including injected failures.
typedef struct {
    uint64_t effective;
    uint64_t permitted;
    uint64_t inheritable;
    uint64_t ambient;
    unsigned int capsetCalls;
    unsigned int ambientCalls;
    int capsetResult;
    int ambientResult;
} CapabilityTestState;
void SetCapabilityTestState(CapabilityTestState *state);

int CapabilityCapsetMock(cap_user_header_t hdrp, const cap_user_data_t datap);
int CapabilityPrctlMock(int option, ...);
int SetAppCapabilities(const AppSpawnMgr *content, const AppSpawningCtx *property);
int SetAppAmbientCapabilities(const AppSpawningCtx *property, uint64_t extAmbientCaps);

#ifdef __cplusplus
}
#endif
#endif // APPSPAWN_CAPABILITY_MOCK_H
