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

#ifndef SPAWN_POLICY_H
#define SPAWN_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "appspawn_hook.h"  // AppSpawningCtx forward decl

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

#define SPAWN_POLICY_NAME_LEN 32
#define SPAWN_POLICY_SELINUX_LEN 64

typedef enum {
    POLICY_SRC_INVALID = 0,
    POLICY_SRC_TLV_DAC,        // uid/gid 取自 TLV_DAC_INFO
    POLICY_SRC_FIXED,          // uid/gid 策略表固定值（daemon=0）
    POLICY_SRC_SUDO,           // caps = sudo 显式掩码
    POLICY_SRC_TLV_CAPS,       // caps 取自 TLV
    POLICY_SRC_SELINUX_SUDO,   // selinux = sudo 域（默认 u:r:sudo_execv_label:s0）
    POLICY_SRC_TLV_SELINUX,    // selinux 取自 TLV
    POLICY_SRC_TLV_BUNDLE,     // processName / 沙箱 root 取自 TLV_BUNDLE_INFO
    POLICY_SRC_EXT_OWNER,      // reserved for future per-owner sandbox root
    POLICY_SRC_TLV_HAP_ENTRY,  // exec 目标取自 TLV hap 入口
    POLICY_SRC_EXT_BIN_PATH,   // exec 目标 / processName 取自 ext:daemon_bin_path
} PolicySrc;

/**
 * @brief 单条孵化策略（策略表条目）。
 *
 */
typedef struct {
    char name[SPAWN_POLICY_NAME_LEN];   // "app" / "daemon-owned"
    PolicySrc uidFrom;
    PolicySrc gidFrom;
    uid_t uid;                          // POLICY_SRC_FIXED 时的固定 uid（daemon=0）
    gid_t gid;                          // POLICY_SRC_FIXED 时的固定 gid
    PolicySrc capsFrom;                 // POLICY_SRC_SUDO / POLICY_SRC_TLV_CAPS
    uint64_t capsMask;                  // POLICY_SRC_SUDO 时的掩码
    PolicySrc selinuxFrom;              // POLICY_SRC_SELINUX_SUDO / POLICY_SRC_TLV_SELINUX
    char selinuxCtx[SPAWN_POLICY_SELINUX_LEN]; // POLICY_SRC_SELINUX_SUDO 时的 ctx
    PolicySrc processNameFrom;          // TLV_BUNDLE (app) / EXT_BIN_PATH (daemon basename)
    PolicySrc execFrom;                 // TLV_HAP_ENTRY (app) / EXT_BIN_PATH (daemon)
    bool setFileDescriptors;           // app=true / daemon=true（重定向 /dev/null）
    bool xpmEnable;                     // app=true / daemon=true
    bool silkEnable;                    // app=true / daemon=false
    bool secCompEnable;                // app=true / daemon=false
    bool schedEnable;                   // app=true / daemon=false
    bool envEnable;                    // app=true / daemon=false
    bool asanEnvEnable;                // app=true / daemon=false
} SpawnPolicy;

/**
 * @brief 按请求数据解析孵化策略（纯函数，零分配/零文件 I/O）。
 *
 * 分类信号 = 专用消息类型 MSG_SPAWN_SERVICE_BIN与报文标记 APP_FLAGS_DAEMON_BIN 必须同时成立。
 * 非 daemon 恒返 &g_profiles[0]（app），永不为 NULL。
 *
 * @param ctx 孵化上下文（含消息类型/TLV/ext）
 * @return 命中策略指针（永不为 NULL，未命中回退 app）
 */
const SpawnPolicy *ResolvePolicy(const AppSpawningCtx *ctx);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif  // SPAWN_POLICY_H
