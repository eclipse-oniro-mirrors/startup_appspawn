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

#include "appspawnclient_initsubspawn_fuzzer.h"
#include <string>
#include "appspawn.h"

namespace {
constexpr uint8_t INIT_CASE_COUNT = 5;
}

namespace OHOS {
    int FuzzAppSpawnClientInitSubspawn(const uint8_t *data, size_t size)
    {
        if (size == 0) {
            return 0;
        }
        // 首字节选择入口，其余字节作为 socket 路径，保留内嵌 NUL 和长度边界。
        std::string socketName(reinterpret_cast<const char *>(data + 1), size - 1);
        AppSpawnClientHandle handle = nullptr;
        int ret;
        switch (data[0] % INIT_CASE_COUNT) {
            case 0:
                ret = AppSpawnClientInitSubspawn(socketName.c_str(), &handle);
                break;
            case 1:
                socketName.insert(0, "/data/service/el1/startup/subspawn_");
                ret = AppSpawnClientInitSubspawn(socketName.c_str(), &handle);
                break;
            case 2:
                ret = AppSpawnClientInitSubspawn(nullptr, &handle);
                break;
            case 3:
                ret = AppSpawnClientInitSubspawn(socketName.c_str(), nullptr);
                break;
            default:
                socketName.insert(0, "/data/service/el1/startup/subspawn_");
                ret = AppSpawnClientInit(socketName.c_str(), &handle);
                break;
        }
        if (handle != nullptr) {
            (void)AppSpawnClientDestroy(handle);
        }
        return ret;
    }
}

/* Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    OHOS::FuzzAppSpawnClientInitSubspawn(data, size);
    return 0;
}
