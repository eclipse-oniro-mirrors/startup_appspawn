/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
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

#ifndef HNP_INSTALL_TEST_H
#define HNP_INSTALL_TEST_H

#include "hnp_installer.h"

#ifdef __cplusplus
extern "C" {
#endif

void ClearSoftLink(int uid);
int BssInstall(HnpSignMapInfo *hnpSignMapInfos, int count, HapInstallInfo *installInfo, int execFilesCount);
int HandleSignParameter(HapInstallInfo *installInfo, const char *signHnp);
bool CheckIsSignHnp(const char *hnpFile, HapInstallInfo *installInfo);
int HnpInstall(const char *hnpFile, HnpInstallInfo *hnpInfo, HnpCfgInfo *hnpCfg,
    HnpSignMapInfo *hnpSignMapInfos, int *count);

#ifdef __cplusplus
}
#endif
#endif
