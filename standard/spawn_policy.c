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

#include "spawn_policy.h"

#include <string.h>

#include "appspawn.h"
#include "appspawn_utils.h"
#include "appspawn_manager.h"

#define DEFAULT_SUDO_CAPS_MASK 0x3fffffffff
#define DEFAULT_SUDO_SELINUX_CTX "u:r:sudo_execv_label:s0"

// γ paradigm: static const code profiles replace the JSON policy table.
// g_profiles[0] = app (values match the former LoadDefaultAppPolicy exactly).
// g_profiles[1] = daemon-owned (has owner ext → per-owner sandbox).
// No fopen/cJSON_Parse runtime path; app behavior is 100% compile-time fixed.
static const SpawnPolicy g_profiles[] = {
    [0] = {
        .name = "app",
        .uidFrom = POLICY_SRC_TLV_DAC,
        .gidFrom = POLICY_SRC_TLV_DAC,
        .capsFrom = POLICY_SRC_TLV_CAPS,
        .selinuxFrom = POLICY_SRC_TLV_SELINUX,
        .processNameFrom = POLICY_SRC_TLV_BUNDLE,
        .execFrom = POLICY_SRC_TLV_HAP_ENTRY,
        .setFileDescriptors = true,
        .xpmEnable = true,
        .silkEnable = true,
        .secCompEnable = true,
        .schedEnable = true,
        .envEnable = true,
        .asanEnvEnable = true,
    },
    // identical for now; differentiated when per-owner sandbox root + system sandbox diverge
    [1] = {
        .name = "daemon-owned",
        .uidFrom = POLICY_SRC_FIXED,
        .uid = 0,
        .gidFrom = POLICY_SRC_FIXED,
        .gid = 0,
        .capsFrom = POLICY_SRC_SUDO,
        .capsMask = DEFAULT_SUDO_CAPS_MASK,
        .selinuxFrom = POLICY_SRC_SELINUX_SUDO,
        .selinuxCtx = DEFAULT_SUDO_SELINUX_CTX,
        .processNameFrom = POLICY_SRC_EXT_BIN_PATH,
        .execFrom = POLICY_SRC_EXT_BIN_PATH,
        .setFileDescriptors = true,
        .xpmEnable = true,
        .silkEnable = false,
        .secCompEnable = false,
        .schedEnable = false,
        .envEnable = false,
        .asanEnvEnable = false,
    },
};

const SpawnPolicy *ResolvePolicy(const AppSpawningCtx *ctx)
{
    if (ctx == NULL || ctx->message == NULL) {
        APPSPAWN_LOGV("ResolvePolicy: NULL ctx, fallback app");
        return &g_profiles[0];  // app fallback — never NULL
    }
    const SpawnPolicy *pl = NULL;
    if (IsDaemonBinMsg(ctx->message)) {
        pl = &g_profiles[1];  // daemon-owned（MSG_SPAWN_SERVICE_BIN 且 APP_FLAGS_DAEMON_BIN）
    } else {
        pl = &g_profiles[0];
    }
    APPSPAWN_LOGV("ResolvePolicy: %{public}s", pl->name);
    return pl;
}

