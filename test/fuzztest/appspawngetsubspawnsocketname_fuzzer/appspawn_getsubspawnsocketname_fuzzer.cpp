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

#include "appspawn_getsubspawnsocketname_fuzzer.h"
#include <cstdlib>
#include "appspawn.h"
#include "fuzzer/FuzzedDataProvider.h"

namespace OHOS {
    void FuzzGetSubSpawnSocketName(const uint8_t *data, size_t size)
    {
        FuzzedDataProvider provider(data, size);
        char *socketName = GetSubSpawnSocketName(provider.ConsumeIntegral<int>());
        if (socketName == nullptr) {
            return;
        }
        AppSpawnClientHandle handle = nullptr;
        (void)AppSpawnClientInitSubspawn(socketName, &handle);
        if (handle != nullptr) {
            (void)AppSpawnClientDestroy(handle);
        }
        free(socketName);
    }
}

/* Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    OHOS::FuzzGetSubSpawnSocketName(data, size);
    return 0;
}
