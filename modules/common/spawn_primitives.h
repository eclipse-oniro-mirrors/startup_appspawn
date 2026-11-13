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

#ifndef SPAWN_PRIMITIVES_H
#define SPAWN_PRIMITIVES_H

#ifdef __cplusplus
#include <climits>
#include <cstdint>
#else
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#endif
#include <sys/types.h>

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

/**
 * @brief 设置进程 capability 集合（纯机制原语）。
 *
 * 仅封装 capset 系统调用（inheritable/permitted/effective 三集合）。
 * 掩码由调用方（共享 Hook 读策略表）提供；app 侧的“flag→mask 计算”、
 * ext perm、isolated sandbox 等策略不在此原语。
 *
 * @param inheritable inheritable 集合位图
 * @param permitted permitted 集合位图
 * @param effective effective 集合位图
 * @return 0 成功；负数失败（-errno）
 */
int SetCapabilities(uint64_t inheritable, uint64_t permitted, uint64_t effective);

/**
 * @brief 判断 64 位 capability 掩码中第 cap 位是否置位（纯函数，无系统调用）。
 *
 * `CAP_TO_MASK(x)` 只取低 5 位（`1U << (x & 31)`），不能用于遍历 64 位掩码：
 * 直接对 cap>=32 使用会回绕到低位，导致重复 raise 低位 capability 并对
 * 不存在的 cap 调用 prctl 返回 EINVAL。掩码位判断必须用整体右移，
 *
 * @param mask capability 位图
 * @param cap capability 序号（0~63；超出 63 一律返回 false）
 * @return true 该位置位；false 未置位或 cap 越界
 */
static inline bool SpawnCapMaskTest(uint64_t mask, uint32_t cap)
{
    if (cap >= sizeof(uint64_t) * CHAR_BIT) {
        return false;
    }
    return ((mask >> cap) & 1ULL) != 0;
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
int SetAmbientCapabilities(uint64_t mask);

/**
 * @brief 设置子进程 exec 时的 SELinux 上下文（纯机制原语）。
 *
 * 仅封装 setexeccon()：上下文只记录到内核的 exec 上下文，域切换发生在
 * 随后的 exec 执行目标二进制时。
 * app 侧的 HapDomain 解析（provision type/APL→域）不在此原语，
 * 由 app Hook 算出 context 后调本原语，见 daemon_spawn_design §6.9。
 * 编译需 WITH_SELINUX；未启用时为空实现返回 0。
 *
 * @param context 目标 SELinux 上下文串，如 "u:r:sudo_execv_label:s0"
 * @return 0 成功；-1 失败（或未启用 SELinux 时 0）
 */
int SetExecSelinuxCon(const char *context);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif  // SPAWN_PRIMITIVES_H
