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
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "appspawn.h"
#include "appspawn_utils.h"

#include "devicedebug_base.h"
#include "devicedebug_kill.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICEDEBUG_KILL_CMD_NUM 4
#define DEVICEDEBUG_KILL_CMD_SIGNAL_INDEX 2
#define DEVICEDEBUG_KILL_CMD_PID_INDEX 3

APPSPAWN_STATIC void DeviceDebugShowKillHelp(void)
{
    printf("\r\nusage: devicedebug kill [options] <pid>"
        "\r\noptions list:"
        "\r\n         -h, --help                list available commands"
        "\r\n         kill -<signal> <pid>      send a signal(1-64) to a process\r\n");
}

APPSPAWN_STATIC char* DeviceDebugJsonStringGeneral(int pid, const char *op, cJSON *args)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(args);
        DEVICEDEBUG_LOGE("devicedebug json write create root object unsuccess");
        return NULL;
    }

    cJSON_AddNumberToObject(root, "app", pid);
    cJSON_AddStringToObject(root, "op", op);
    cJSON_AddItemToObject(root, "args", args);

    char *jsonString = cJSON_Print(root);
    cJSON_Delete(root);
    return jsonString;
}

APPSPAWN_STATIC void DevicedebugKillRetDeal(int result, int pid)
{
    switch (result) {
        case APPSPAWN_DEVICEDEBUG_ERROR_APP_NOT_EXIST:
            printf("devicedebug: kill: %d: No such app process\r\n", pid);
            break;
        case APPSPAWN_DEVICEDEBUG_ERROR_APP_NOT_DEBUGGABLE:
            printf("devicedebug: kill: process: %d is not debuggable app\r\n", pid);
            break;
        default:
            printf("devicedebug: process: %d kill unsuccess ret=%d, please check the hilog for the cause\r\n",
                pid, result);
            break;
    }
}

APPSPAWN_STATIC int DeviceDebugKill(int pid, int signal)
{
    cJSON *args = cJSON_CreateObject();
    if (args == NULL) {
        DEVICEDEBUG_LOGE("devicedebug json write create args object unsuccess");
        return DEVICEDEBUG_ERRNO_JSON_CREATED_FAILED;
    }
    cJSON_AddNumberToObject(args, "signal", signal);
    char *jsonString = DeviceDebugJsonStringGeneral(pid, "kill", args);
    if (jsonString == NULL) {
        return DEVICEDEBUG_ERRNO_JSON_CREATED_FAILED;
    }

    AppSpawnReqMsgHandle reqHandle;
    int ret = AppSpawnReqMsgCreate(MSG_DEVICE_DEBUG, "devicedebug", &reqHandle);
    if (ret != 0) {
        free(jsonString);
        DEVICEDEBUG_LOGE("devicedebug appspawn message create unsuccess, ret=%{public}d", ret);
        return ret;
    }

    ret = AppSpawnReqMsgAddExtInfo(reqHandle, "devicedebug", (uint8_t *)jsonString, strlen(jsonString) + 1);
    if (ret != 0) {
        DEVICEDEBUG_LOGE("devicedebug appspawn message add devicedebug[%{public}s] unsuccess, ret=%{public}d",
            jsonString, ret);
        free(jsonString);
        AppSpawnReqMsgFree(reqHandle);
        return ret;
    }

    AppSpawnClientHandle clientHandle;
    ret = AppSpawnClientInit(APPSPAWN_SERVER_NAME, &clientHandle);
    if (ret != 0) {
        free(jsonString);
        AppSpawnReqMsgFree(reqHandle);
        DEVICEDEBUG_LOGE("devicedebug appspawn client init unsuccess, ret=%{public}d", ret);
        return ret;
    }

    AppSpawnResult result = {0};
    ret = AppSpawnClientSendMsg(clientHandle, reqHandle, &result);
    AppSpawnClientDestroy(clientHandle);
    free(jsonString);
    if (ret != 0) {
        DEVICEDEBUG_LOGE("devicedebug appspawn send msg unsuccess, ret=%{public}d", ret);
        return ret;
    }

    if (result.result != 0) {
        return result.result;
    }

    return 0;
}

int DeviceDebugCmdKill(int argc, char *argv[])
{
    if (!IsDeveloperModeOpen()) {
        return DEVICEDEBUG_ERRNO_NOT_IN_DEVELOPER_MODE;
    }

    if (argc <= DEVICEDEBUG_NUM_2 || strcmp(argv[DEVICEDEBUG_NUM_2], "-h") == 0 ||
        strcmp(argv[DEVICEDEBUG_NUM_2], "help") == 0) {
        DeviceDebugShowKillHelp();
        return 0;
    }

    if (argc < DEVICEDEBUG_KILL_CMD_NUM) {
        DEVICEDEBUG_LOGE("devicedebug cmd operator num is %{public}d < %{public}d", argc, DEVICEDEBUG_KILL_CMD_NUM);
        DeviceDebugShowKillHelp();
        return DEVICEDEBUG_ERRNO_OPERATOR_ARGV_MISS;
    }

    int signal = atoi(argv[DEVICEDEBUG_KILL_CMD_SIGNAL_INDEX] + 1);
    if (signal > SIGRTMAX || signal <= 0) {
        DEVICEDEBUG_LOGE("signal is %{public}d not in [1, %{public}d]", signal, SIGRTMAX);
        DeviceDebugShowKillHelp();
        return DEVICEDEBUG_ERRNO_PARAM_INVALID;
    }

    int pid = atoi(argv[DEVICEDEBUG_KILL_CMD_PID_INDEX]);
    DEVICEDEBUG_LOGI("devicedebug cmd kill start signal[%{public}d], pid[%{public}d]", signal, pid);

    int ret = DeviceDebugKill(pid, signal);
    if (ret != 0) {
        DevicedebugKillRetDeal(ret, pid);
    }

    return ret;
}

#ifdef __cplusplus
}
#endif
