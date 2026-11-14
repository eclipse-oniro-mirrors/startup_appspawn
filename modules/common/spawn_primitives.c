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

#include "spawn_primitives.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/types.h>

#include "appspawn_utils.h"
#include "securec.h"

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
#endif

#define BITLEN32 32

/**
 * @brief 设置进程 capability 集合（纯机制原语）。
 *
 * 仅封装 capset 系统调用（inheritable/permitted/effective 三集合）。
 * 掩码由调用方（共享 Hook 读策略表）提供；app 侧的“flag→mask 计算”、
 * ext perm、isolated sandbox 等策略不在此原语，见 daemon_spawn_design §6.9。
 *
 * @param inheritable inheritable 集合位图
 * @param permitted permitted 集合位图
 * @param effective effective 集合位图
 * @return 0 成功；负数失败（-errno）
 */
int SetCapabilities(uint64_t inheritable, uint64_t permitted, uint64_t effective)
{
    struct __user_cap_header_struct capHeader;
    if (memset_s(&capHeader, sizeof(capHeader), 0, sizeof(capHeader)) != EOK) {
        APPSPAWN_LOGE("Failed to memset cap header");
        return -EINVAL;
    }
    capHeader.version = _LINUX_CAPABILITY_VERSION_3;
    capHeader.pid = 0;
    struct __user_cap_data_struct capData[2];
    if (memset_s(capData, sizeof(capData), 0, sizeof(capData)) != EOK) {
        APPSPAWN_LOGE("Failed to memset cap data");
        return -EINVAL;
    }
    capData[0].inheritable = (__u32)(inheritable);
    capData[1].inheritable = (__u32)(inheritable >> BITLEN32);
    capData[0].permitted = (__u32)(permitted);
    capData[1].permitted = (__u32)(permitted >> BITLEN32);
    capData[0].effective = (__u32)(effective);
    capData[1].effective = (__u32)(effective >> BITLEN32);
    if (capset(&capHeader, &capData[0]) != 0) {
        APPSPAWN_LOGE("Failed to capset errno: %{public}d", errno);
        return -errno;
    }
    return 0;
}

/**
 * @brief 设置 ambient capability 集合（纯机制原语）。
 *
 * 遍历掩码中置位的每个 cap，逐个 prctl(PR_CAP_AMBIENT, RAISE)。
 * 掩码由调用方提供；app 侧的 CAP_DAC_OVERRIDE/CAP_KILL/CAP_FOWNER
 * 条件计算不在此原语，见 daemon_spawn_design §6.9。
 *
 * @param mask ambient 能力位图
 * @return 0 成功；-1 失败
 */
int SetAmbientCapabilities(uint64_t mask)
{
    if (mask == 0) {
        return 0;
    }
    // 遍历 64 位掩码必须用整体右移判断位；CAP_TO_MASK 只取低 5 位，
    // 对 cap>=32 会回绕（raise 错位 cap 并对越界 cap 返回 EINVAL）。
    for (uint32_t cap = 0; cap < (uint32_t)(sizeof(uint64_t) * CHAR_BIT); cap++) {
        if (!SpawnCapMaskTest(mask, cap)) {
            continue;
        }
        if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) != 0) {
            APPSPAWN_LOGE("prctl PR_CAP_AMBIENT raise cap %{public}u failed: %{public}d", cap, errno);
            return -1;
        }
    }
    return 0;
}

int SetExecSelinuxCon(const char *context)
{
    if (context == NULL) {
        return 0;
    }
#ifdef WITH_SELINUX
    if (setexeccon(context) != 0) {
        APPSPAWN_LOGE("setexeccon(%{public}s) failed: %{public}d", context, errno);
        return -1;
    }
#else
    (void)context;
#endif
    return 0;
}
