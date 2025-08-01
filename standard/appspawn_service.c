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

#include "appspawn_service.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/mount.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sched.h>
#include "appspawn.h"
#include "appspawn_hook.h"
#include "appspawn_modulemgr.h"
#include "appspawn_manager.h"
#include "appspawn_msg.h"
#include "appspawn_server.h"
#include "appspawn_trace.h"
#include "appspawn_utils.h"
#include "init_socket.h"
#include "init_utils.h"
#include "parameter.h"
#include "appspawn_adapter.h"
#include "securec.h"
#include "cJSON.h"
#ifdef APPSPAWN_HISYSEVENT
#include "appspawn_hisysevent.h"
#include "hisysevent_adapter.h"
#endif
#define PARAM_BUFFER_SIZE 10
#define PATH_SIZE 256
#define FD_PATH_SIZE 128

#define PREFORK_PROCESS "apppool"
#define APPSPAWN_MSG_USER_CHECK_COUNT 4
#define USER_ID_MIN_VALUE 100
#define USER_ID_MAX_VALUE 10736
#define LOCK_STATUS_PARAM_SIZE 64
#ifndef PIDFD_NONBLOCK
#define PIDFD_NONBLOCK O_NONBLOCK
#endif

static void WaitChildTimeout(const TimerHandle taskHandle, void *context);
static void ProcessChildResponse(const WatcherHandle taskHandle, int fd, uint32_t *events, const void *context);
static void WaitChildDied(pid_t pid);
static void OnReceiveRequest(const TaskHandle taskHandle, const uint8_t *buffer, uint32_t buffLen);
static void ProcessRecvMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message);

// FD_CLOEXEC
static inline void SetFdCtrl(int fd, int opt)
{
    int option = fcntl(fd, F_GETFD);
    APPSPAWN_CHECK(option >= 0, return, "SetFdCtrl fcntl failed %{public}d, %{public}d", option, errno);
    int ret = fcntl(fd, F_SETFD, (unsigned int)option | (unsigned int)opt);
    if (ret < 0) {
        APPSPAWN_LOGI("Set fd %{public}d option %{public}d %{public}d result: %{public}d", fd, option, opt, errno);
    }
}

static void AppQueueDestroyProc(const AppSpawnMgr *mgr, AppSpawnedProcess *appInfo, void *data)
{
    pid_t pid = appInfo->pid;
    APPSPAWN_LOGI("kill %{public}s pid: %{public}d", appInfo->name, appInfo->pid);
    // notify child proess died,clean sandbox info
    ProcessMgrHookExecute(STAGE_SERVER_APP_DIED, GetAppSpawnContent(), appInfo);
    OH_ListRemove(&appInfo->node);
    OH_ListInit(&appInfo->node);
    free(appInfo);
    if (pid > 0 && kill(pid, SIGKILL) != 0) {
        APPSPAWN_LOGE("unable to kill process, pid: %{public}d errno: %{public}d", pid, errno);
    }
}

static void StopAppSpawn(void)
{
    // delete nwespawn, and wait exit. Otherwise, the process of nwebspawn spawning will become zombie
    AppSpawnedProcess *appInfo = GetSpawnedProcessByName(NWEBSPAWN_SERVER_NAME);
    if (appInfo != NULL) {
        APPSPAWN_LOGI("kill %{public}s pid: %{public}d", appInfo->name, appInfo->pid);
        int exitStatus = 0;
        KillAndWaitStatus(appInfo->pid, SIGTERM, &exitStatus);
        OH_ListRemove(&appInfo->node);
        OH_ListInit(&appInfo->node);
        free(appInfo);
    }
    // delete nativespawn, and wait exit. Otherwise, the process of nativespawn spawning will become zombie
    appInfo = GetSpawnedProcessByName(NATIVESPAWN_SERVER_NAME);
    if (appInfo != NULL) {
        APPSPAWN_LOGI("kill %{public}s pid: %{public}d", appInfo->name, appInfo->pid);
        int exitStatus = 0;
        KillAndWaitStatus(appInfo->pid, SIGTERM, &exitStatus);
        OH_ListRemove(&appInfo->node);
        OH_ListInit(&appInfo->node);
        free(appInfo);
    }

    AppSpawnContent *content = GetAppSpawnContent();
    if (content != NULL && content->reservedPid > 0) {
        int ret = kill(content->reservedPid, SIGKILL);
        APPSPAWN_CHECK_ONLY_LOG(ret == 0, "kill reserved pid %{public}d failed %{public}d %{public}d",
            content->reservedPid, ret, errno);
        content->reservedPid = 0;
    }
    TraversalSpawnedProcess(AppQueueDestroyProc, NULL);
    APPSPAWN_LOGI("StopAppSpawn ");
#ifdef APPSPAWN_HISYSEVENT
    AppSpawnHiSysEventWrite();
#endif
    LE_StopLoop(LE_GetDefaultLoop());
}

static inline void DumpStatus(const char *appName, pid_t pid, int status, int *signal)
{
    if (WIFSIGNALED(status)) {
        *signal = WTERMSIG(status);
        APPSPAWN_LOGW("%{public}s with pid %{public}d exit with signal:%{public}d", appName, pid, *signal);
    }
    if (WIFEXITED(status)) {
        *signal = WEXITSTATUS(status);
        APPSPAWN_LOGW("%{public}s with pid %{public}d exit with code:%{public}d", appName, pid, *signal);
    }
}

APPSPAWN_STATIC void WriteSignalInfoToFd(AppSpawnedProcess *appInfo, AppSpawnContent *content, int signal)
{
    APPSPAWN_CHECK(content->signalFd > 0, return, "Invalid signal fd[%{public}d]", content->signalFd);
    APPSPAWN_CHECK(appInfo->pid > 0, return, "Invalid pid[%{public}d]", appInfo->pid);
    APPSPAWN_CHECK(appInfo->uid > 0, return, "Invalid uid[%{public}d]", appInfo->uid);
    APPSPAWN_CHECK(appInfo->name != NULL, return, "Invalid name");

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        APPSPAWN_LOGE("signal json write create root object unsuccess");
        return;
    }
    cJSON_AddNumberToObject(root, "pid", appInfo->pid);
    cJSON_AddNumberToObject(root, "uid", appInfo->uid);
    cJSON_AddNumberToObject(root, "signal", signal);
    cJSON_AddStringToObject(root, "bundleName", appInfo->name);
    char *jsonString = cJSON_Print(root);
    cJSON_Delete(root);

    int ret = write(content->signalFd, jsonString, strlen(jsonString) + 1);
    if (ret < 0) {
        free(jsonString);
        APPSPAWN_LOGE("Spawn Listen failed to write signal info to fd errno %{public}d", errno);
        return;
    }
    APPSPAWN_LOGV("Spawn Listen write signal info %{public}s to fd %{public}d success", jsonString, content->signalFd);
    free(jsonString);
}

static void HandleDiedPid(pid_t pid, uid_t uid, int status)
{
    AppSpawnContent *content = GetAppSpawnContent();
    APPSPAWN_CHECK(content != NULL, return, "Invalid content");

    if (pid == content->reservedPid) {
        APPSPAWN_LOGW("HandleDiedPid with reservedPid %{public}d", pid);
        content->reservedPid = 0;
    }
    int signal = 0;
    AppSpawnedProcess *appInfo = GetSpawnedProcess(pid);
    if (appInfo == NULL) { // If an exception occurs during app spawning, kill pid, return failed
        WaitChildDied(pid);
        DumpStatus("unknown", pid, status, &signal);
        return;
    }

    appInfo->exitStatus = status;
    APPSPAWN_CHECK_ONLY_LOG(appInfo->uid == uid, "Invalid uid %{public}u %{public}u", appInfo->uid, uid);
    DumpStatus(appInfo->name, pid, status, &signal);
    WriteSignalInfoToFd(appInfo, content, signal);
    ProcessMgrHookExecute(STAGE_SERVER_APP_DIED, GetAppSpawnContent(), appInfo);
    ProcessMgrHookExecute(STAGE_SERVER_APP_UMOUNT, GetAppSpawnContent(), appInfo);

    // move app info to died queue in NWEBSPAWN, or delete appinfo
    TerminateSpawnedProcess(appInfo);
}

APPSPAWN_STATIC void ProcessSignal(const struct signalfd_siginfo *siginfo)
{
    APPSPAWN_LOGI("ProcessSignal signum %{public}d %{public}d", siginfo->ssi_signo, siginfo->ssi_pid);
    switch (siginfo->ssi_signo) {
        case SIGCHLD: { // delete pid from app map
            pid_t pid;
            int status;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                APPSPAWN_CHECK(WIFSIGNALED(status) || WIFEXITED(status), return,
                    "ProcessSignal with wrong status:%{public}d", status);
                HandleDiedPid(pid, siginfo->ssi_uid, status);
            }
#if (defined(CJAPP_SPAWN) || defined(NATIVE_SPAWN))
            if (OH_ListGetCnt(&GetAppSpawnMgr()->appQueue) == 0) {
                LE_StopLoop(LE_GetDefaultLoop());
            }
#endif
            break;
        }
        case SIGTERM: { // appswapn killed, use kill without parameter
            StopAppSpawn();
            break;
        }
        default:
            APPSPAWN_LOGI("SigHandler, unsupported signal %{public}d.", siginfo->ssi_signo);
            break;
    }
}

static void AppSpawningCtxOnClose(const AppSpawnMgr *mgr, AppSpawningCtx *ctx, void *data)
{
    if (ctx == NULL || ctx->message == NULL || ctx->message->connection != data) {
        return;
    }
    APPSPAWN_LOGI("Kill process, pid: %{public}d app: %{public}s", ctx->pid, GetProcessName(ctx));
    if (ctx->pid > 0 && kill(ctx->pid, SIGKILL) != 0) {
        APPSPAWN_LOGE("unable to kill process, pid: %{public}d errno: %{public}d", ctx->pid, errno);
    }
    DeleteAppSpawningCtx(ctx);
}

static void OnClose(const TaskHandle taskHandle)
{
    if (!IsSpawnServer(GetAppSpawnMgr())) {
        return;
    }
    AppSpawnConnection *connection = (AppSpawnConnection *)LE_GetUserData(taskHandle);
    APPSPAWN_CHECK(connection != NULL, return, "Invalid connection");
    if (connection->receiverCtx.timer) {
        LE_StopTimer(LE_GetDefaultLoop(), connection->receiverCtx.timer);
        connection->receiverCtx.timer = NULL;
    }
    APPSPAWN_LOGI("OnClose connectionId: %{public}u socket %{public}d",
        connection->connectionId, LE_GetSocketFd(taskHandle));
    DeleteAppSpawnMsg(&connection->receiverCtx.incompleteMsg);
    connection->receiverCtx.incompleteMsg = NULL;
    // connect close, to close spawning app
    AppSpawningCtxTraversal(AppSpawningCtxOnClose, connection);
}

static void OnDisConnect(const TaskHandle taskHandle)
{
    AppSpawnConnection *connection = (AppSpawnConnection *)LE_GetUserData(taskHandle);
    APPSPAWN_CHECK(connection != NULL, return, "Invalid connection");
    APPSPAWN_LOGI("OnDisConnect connectionId: %{public}u socket %{public}d",
        connection->connectionId, LE_GetSocketFd(taskHandle));
    OnClose(taskHandle);
}

static void SendMessageComplete(const TaskHandle taskHandle, BufferHandle handle)
{
    AppSpawnConnection *connection = (AppSpawnConnection *)LE_GetUserData(taskHandle);
    APPSPAWN_CHECK(connection != NULL, return, "Invalid connection");
    uint32_t bufferSize = sizeof(AppSpawnResponseMsg);
    AppSpawnResponseMsg *msg = (AppSpawnResponseMsg *)LE_GetBufferInfo(handle, NULL, &bufferSize);
    if (msg == NULL) {
        return;
    }
    AppSpawnedProcess *appInfo = GetSpawnedProcess(msg->result.pid);
    if (appInfo == NULL) {
        return;
    }
    APPSPAWN_LOGI("SendMessageComplete connectionId: %{public}u result %{public}d app %{public}s pid %{public}d",
        connection->connectionId, LE_GetSendResult(handle), appInfo->name, msg->result.pid);
    if (LE_GetSendResult(handle) != 0 && msg->result.pid > 0) {
        kill(msg->result.pid, SIGKILL);
    }
}

static int SendResponse(const AppSpawnConnection *connection, const AppSpawnMsg *msg, int result, pid_t pid)
{
    StartAppspawnTrace("SendResponse");
    APPSPAWN_LOGV("SendResponse connectionId: %{public}u result: 0x%{public}x pid: %{public}d",
        connection->connectionId, result, pid);
    uint32_t bufferSize = sizeof(AppSpawnResponseMsg);
    BufferHandle handle = LE_CreateBuffer(LE_GetDefaultLoop(), bufferSize);
    AppSpawnResponseMsg *buffer = (AppSpawnResponseMsg *)LE_GetBufferInfo(handle, NULL, &bufferSize);
    APPSPAWN_CHECK(buffer != NULL, return APPSPAWN_ERROR_UTILS_MEM_FAIL, "buffer is null");
    int ret = memcpy_s(buffer, bufferSize, msg, sizeof(AppSpawnMsg));
    APPSPAWN_CHECK(ret == 0, LE_FreeBuffer(LE_GetDefaultLoop(), NULL, handle);
        return -1, "Failed to memcpy_s bufferSize");
    buffer->result.result = result;
    buffer->result.pid = pid;
    ret = LE_Send(LE_GetDefaultLoop(), connection->stream, handle, bufferSize);
    FinishAppspawnTrace();
    return ret;
}

static void WaitMsgCompleteTimeOut(const TimerHandle taskHandle, void *context)
{
    AppSpawnConnection *connection = (AppSpawnConnection *)context;
    APPSPAWN_LOGE("Long time no msg complete so close connectionId: %{public}u", connection->connectionId);
    DeleteAppSpawnMsg(&connection->receiverCtx.incompleteMsg);
    connection->receiverCtx.incompleteMsg = NULL;
    LE_CloseStreamTask(LE_GetDefaultLoop(), connection->stream);
}

static inline int StartTimerForCheckMsg(AppSpawnConnection *connection)
{
    if (connection->receiverCtx.timer != NULL) {
        return 0;
    }
    int ret = LE_CreateTimer(LE_GetDefaultLoop(), &connection->receiverCtx.timer, WaitMsgCompleteTimeOut, connection);
    if (ret == 0) {
        ret = LE_StartTimer(LE_GetDefaultLoop(), connection->receiverCtx.timer, MAX_WAIT_MSG_COMPLETE, 1);
    }
    return ret;
}

static int HandleRecvMessage(const TaskHandle taskHandle, uint8_t * buffer, int bufferSize, int flags)
{
    int socketFd = LE_GetSocketFd(taskHandle);
    struct iovec iov = {
        .iov_base = buffer,
        .iov_len = bufferSize,
    };
    char ctrlBuffer[CMSG_SPACE(APP_MAX_FD_COUNT * sizeof(int))];
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrlBuffer,
        .msg_controllen = sizeof(ctrlBuffer),
    };

    AppSpawnConnection *connection = (AppSpawnConnection *) LE_GetUserData(taskHandle);
    APPSPAWN_CHECK(connection != NULL, return -1, "Invalid connection");
    errno = 0;
    int recvLen = recvmsg(socketFd, &msg, flags);
    APPSPAWN_CHECK_ONLY_LOG(errno == 0, "recvmsg with errno %{public}d", errno);
    struct cmsghdr *cmsg = NULL;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fdCount = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fd = (int *) CMSG_DATA(cmsg);
            APPSPAWN_CHECK(fdCount <= APP_MAX_FD_COUNT, return -1,
                "failed to recv fd %{public}d %{public}d", connection->receiverCtx.fdCount, fdCount);
            int ret = memcpy_s(connection->receiverCtx.fds,
                fdCount * sizeof(int), fd, fdCount * sizeof(int));
            APPSPAWN_CHECK(ret == 0, return -1, "memcpy_s fd ret %{public}d", ret);
            connection->receiverCtx.fdCount = fdCount;
        }
    }

    return recvLen;
}

APPSPAWN_STATIC bool OnConnectionUserCheck(uid_t uid)
{
    const uid_t uids[APPSPAWN_MSG_USER_CHECK_COUNT] = {
        0, // root 0
        3350, // app_fwk_update 3350
        5523, // foundation 5523
        1090, //storage_manager 1090
    };

    for (int i = 0; i < APPSPAWN_MSG_USER_CHECK_COUNT; i++) {
        if (uid == uids[i]) {
            return true;
        }
    }

    // shell 2000
    if (uid == 2000 && IsDeveloperModeOpen()) {
        return true;
    }

    return false;
}

static int OnConnection(const LoopHandle loopHandle, const TaskHandle server)
{
    APPSPAWN_CHECK(server != NULL && loopHandle != NULL, return -1, "Error server");
    static uint32_t connectionId = 0;
    TaskHandle stream;
    LE_StreamInfo info = {};
    info.baseInfo.flags = TASK_STREAM | TASK_PIPE | TASK_CONNECT;
    info.baseInfo.close = OnClose;
    info.baseInfo.userDataSize = sizeof(AppSpawnConnection);
    info.disConnectComplete = OnDisConnect;
    info.sendMessageComplete = SendMessageComplete;
    info.recvMessage = OnReceiveRequest;
    info.handleRecvMsg = HandleRecvMessage;
    LE_STATUS ret = LE_AcceptStreamClient(loopHandle, server, &stream, &info);
    APPSPAWN_CHECK(ret == 0, return -1, "Failed to alloc stream");

    AppSpawnConnection *connection = (AppSpawnConnection *)LE_GetUserData(stream);
    APPSPAWN_CHECK(connection != NULL, return -1, "Failed to alloc stream");
    struct ucred cred = {-1, -1, -1};
    socklen_t credSize = sizeof(struct ucred);
    if ((getsockopt(LE_GetSocketFd(stream), SOL_SOCKET, SO_PEERCRED, &cred, &credSize) < 0) ||
        !OnConnectionUserCheck(cred.uid)) {
        APPSPAWN_LOGE("Invalid uid %{public}d from client", cred.uid);
        LE_CloseStreamTask(LE_GetDefaultLoop(), stream);
        return -1;
    }
    SetFdCtrl(LE_GetSocketFd(stream), FD_CLOEXEC);

    connection->connectionId = ++connectionId;
    connection->stream = stream;
    connection->receiverCtx.fdCount = 0;
    connection->receiverCtx.incompleteMsg = NULL;
    connection->receiverCtx.timer = NULL;
    connection->receiverCtx.msgRecvLen = 0;
    connection->receiverCtx.nextMsgId = 1;
    APPSPAWN_LOGI("OnConnection connectionId: %{public}u fd %{public}d ",
        connection->connectionId, LE_GetSocketFd(stream));
    return 0;
}

APPSPAWN_STATIC bool MsgDevicedebugCheck(TaskHandle stream, AppSpawnMsgNode *message)
{
    struct ucred cred = {0, 0, 0};
    socklen_t credSize = sizeof(cred);
    if (getsockopt(LE_GetSocketFd(stream), SOL_SOCKET, SO_PEERCRED, &cred, &credSize) < 0) {
        return false;
    }

    if (cred.uid != DecodeUid("shell")) {
        return true;
    }

    AppSpawnMsg *msg = &message->msgHeader;
    APPSPAWN_CHECK(msg->msgType == MSG_DEVICE_DEBUG, return false,
        "appspawn devicedebug msg type is not devicedebug [%{public}d]", msg->msgType);

    return true;
}

static void OnReceiveRequest(const TaskHandle taskHandle, const uint8_t *buffer, uint32_t buffLen)
{
    AppSpawnConnection *connection = (AppSpawnConnection *)LE_GetUserData(taskHandle);
    APPSPAWN_CHECK(connection != NULL, LE_CloseTask(LE_GetDefaultLoop(), taskHandle);
        return, "Failed to get client form socket");
    APPSPAWN_CHECK(buffLen < MAX_MSG_TOTAL_LENGTH, LE_CloseTask(LE_GetDefaultLoop(), taskHandle);
        return, "Message too long %{public}u", buffLen);

    uint32_t reminder = 0;
    uint32_t currLen = 0;
    AppSpawnMsgNode *message = connection->receiverCtx.incompleteMsg; // incomplete msg
    connection->receiverCtx.incompleteMsg = NULL;
    int ret = 0;
    do {
        APPSPAWN_LOGI("connectionId:%{public}u buffLen:%{public}d",
            connection->connectionId, buffLen - currLen);

        ret = GetAppSpawnMsgFromBuffer(buffer + currLen, buffLen - currLen,
            &message, &connection->receiverCtx.msgRecvLen, &reminder);
        APPSPAWN_CHECK_ONLY_EXPER(ret == 0, break);

        if (connection->receiverCtx.msgRecvLen != message->msgHeader.msgLen) {  // recv complete msg
            connection->receiverCtx.incompleteMsg = message;
            message = NULL;
            break;
        }
        connection->receiverCtx.msgRecvLen = 0;
        if (connection->receiverCtx.timer) {
            LE_StopTimer(LE_GetDefaultLoop(), connection->receiverCtx.timer);
            connection->receiverCtx.timer = NULL;
        }

        APPSPAWN_CHECK_ONLY_EXPER(MsgDevicedebugCheck(connection->stream, message),
            LE_CloseTask(LE_GetDefaultLoop(), taskHandle); return);

        // decode msg
        ret = DecodeAppSpawnMsg(message);
        APPSPAWN_CHECK_ONLY_EXPER(ret == 0, break);
        StartAppspawnTrace("ProcessRecvMsg");
        (void)ProcessRecvMsg(connection, message);
        FinishAppspawnTrace();
        message = NULL;
        currLen += buffLen - reminder;
    } while (reminder > 0);

    if (message) {
        DeleteAppSpawnMsg(&message);
    }
    if (ret != 0) {
        LE_CloseTask(LE_GetDefaultLoop(), taskHandle);
        return;
    }
    if (connection->receiverCtx.incompleteMsg != NULL) { // Start the detection timer
        ret = StartTimerForCheckMsg(connection);
        APPSPAWN_CHECK(ret == 0, LE_CloseStreamTask(LE_GetDefaultLoop(), taskHandle);
            return, "Failed to create time for connection");
    }
}

static char *GetMapMem(uint32_t clientId, const char *processName, uint32_t size, bool readOnly, bool isNweb)
{
    char path[PATH_MAX] = {};
    int len = sprintf_s(path, sizeof(path), APPSPAWN_MSG_DIR "%s/%s_%u",
        isNweb ? "nwebspawn" : "appspawn", processName, clientId);
    APPSPAWN_CHECK(len > 0, return NULL, "Failed to format path %{public}s", processName);
    APPSPAWN_LOGV("GetMapMem for child %{public}s memSize %{public}u", path, size);
    int prot = PROT_READ;
    int mode = O_RDONLY;
    if (!readOnly) {
        mode = O_CREAT | O_RDWR | O_TRUNC;
        prot = PROT_READ | PROT_WRITE;
    }
    int fd = open(path, mode, S_IRWXU);
    APPSPAWN_CHECK(fd >= 0, return NULL, "Failed to open errno %{public}d path %{public}s", errno, path);
    if (!readOnly) {
        ftruncate(fd, size);
    }
    void *areaAddr = (void *)mmap(NULL, size, prot, MAP_SHARED, fd, 0);
    close(fd);
    APPSPAWN_CHECK(areaAddr != MAP_FAILED && areaAddr != NULL,
        return NULL, "Failed to map memory error %{public}d fileName %{public}s ", errno, path);
    return (char *)areaAddr;
}

APPSPAWN_STATIC int WriteMsgToChild(AppSpawningCtx *property, bool isNweb)
{
    APPSPAWN_CHECK(property != NULL && property->message != NULL, return APPSPAWN_MSG_INVALID,
        "Failed to WriteMsgToChild property invalid");
    const uint32_t memSize = (property->message->msgHeader.msgLen / 4096 + 1) * 4096; // 4096 4K
    char *buffer = GetMapMem(property->client.id, GetProcessName(property), memSize, false, isNweb);
    APPSPAWN_CHECK(buffer != NULL, return APPSPAWN_SYSTEM_ERROR,
        "Failed to map memory error %{public}d fileName %{public}s ", errno, GetProcessName(property));
    // copy msg header
    int ret = memcpy_s(buffer, memSize, &property->message->msgHeader, sizeof(AppSpawnMsg));
    if (ret == 0) {
        ret = memcpy_s((char *)buffer + sizeof(AppSpawnMsg), memSize - sizeof(AppSpawnMsg),
            property->message->buffer, property->message->msgHeader.msgLen - sizeof(AppSpawnMsg));
    }
    if (ret != 0) {
        APPSPAWN_LOGE("Failed to copy msg fileName %{public}s ", GetProcessName(property));
        munmap((char *)buffer, memSize);
        return APPSPAWN_SYSTEM_ERROR;
    }
    property->forkCtx.msgSize = memSize;
    property->forkCtx.childMsg = buffer;
    APPSPAWN_LOGV("Write msg to child: %{public}u success", property->client.id);
    return 0;
}

static int InitForkContext(AppSpawningCtx *property)
{
    if (pipe(property->forkCtx.fd) == -1) {
        APPSPAWN_LOGE("create pipe fail, errno: %{public}d", errno);
        return errno;
    }
    int option = fcntl(property->forkCtx.fd[0], F_GETFD);
    if (option > 0) {
        (void)fcntl(property->forkCtx.fd[0], F_SETFD, (unsigned int)option | O_NONBLOCK);
    }
    return 0;
}

static void ClosePidfdWatcher(const TaskHandle taskHandle)
{
    int fd = LE_GetSocketFd(taskHandle);
    if (fd >= 0) {
        close(fd);
    }
    void *p = LE_GetUserData(taskHandle);
    if (p != NULL) {
        free(*(void **)p);
    }
}

static void ProcessChildProcessFd(const WatcherHandle taskHandle, int fd, uint32_t *events, const void *context)
{
    APPSPAWN_CHECK_ONLY_EXPER(context != NULL, return);
    pid_t pid = *(pid_t *)context;
    APPSPAWN_LOGI("Clear process group with pid %{public}d, pidFd %{public}d", pid, fd);
    AppSpawnedProcess *appInfo = GetSpawnedProcess(pid);
    if (appInfo == NULL) {
        APPSPAWN_LOGW("Cannot get app info by bundle name: %{public}d", pid);
        return;
    }
    ProcessMgrHookExecute(STAGE_SERVER_APP_DIED, GetAppSpawnContent(), appInfo);
    LE_CloseTask(LE_GetDefaultLoop(), taskHandle);
}

static int OpenPidFd(pid_t pid, unsigned int flags)
{
    return syscall(SYS_pidfd_open, pid, flags);
}

static void WatchChildProcessFd(AppSpawningCtx *property)
{
    if (property->pid <= 0) {
        APPSPAWN_LOGW("Invalid child process pid, skip watch");
        return;
    }
    if (IsNWebSpawnMode((AppSpawnMgr *)GetAppSpawnContent())) {
        APPSPAWN_LOGV("Nwebspawn don't need add pidfd");
        return;
    }
    AppSpawnedProcess *appInfo = GetSpawnedProcess(property->pid);
    if (appInfo == NULL) {
        APPSPAWN_LOGW("Cannot get app info of pid %{public}d", property->pid);
        return;
    }
    int fd = OpenPidFd(property->pid, PIDFD_NONBLOCK); // PIDFD_NONBLOCK  since Linux kernel 5.10
    if (fd < 0) {
        APPSPAWN_LOGW("Failed to open pid fd for app: %{public}s, err = %{public}d",
            GetBundleName(property), errno);
        return;
    }
    APPSPAWN_LOGI("watch app process pid %{public}d, pidFd %{public}d", property->pid, fd);
    LE_WatchInfo watchInfo = {};
    watchInfo.fd = fd;
    watchInfo.flags = WATCHER_ONCE;
    watchInfo.events = EVENT_READ;
    watchInfo.close = ClosePidfdWatcher;
    watchInfo.processEvent = ProcessChildProcessFd;

    pid_t *appPid = (pid_t *)malloc(sizeof(pid_t));
    APPSPAWN_CHECK_ONLY_EXPER(appPid != NULL, return);
    *appPid = property->pid;
    LE_STATUS status = LE_StartWatcher(LE_GetDefaultLoop(), &property->forkCtx.pidFdWatcherHandle, &watchInfo, appPid);
    if (status != LE_SUCCESS) {
#ifndef APPSPAWN_TEST
        close(fd);
#endif
        APPSPAWN_LOGW("Failed to watch child pid fd, pid is %{public}d", property->pid);
    }
}

static int IsChildColdRun(AppSpawningCtx *property)
{
    return CheckAppMsgFlagsSet(property, APP_FLAGS_UBSAN_ENABLED)
        || CheckAppMsgFlagsSet(property, APP_FLAGS_ASANENABLED)
        || CheckAppMsgFlagsSet(property, APP_FLAGS_TSAN_ENABLED)
        || CheckAppMsgFlagsSet(property, APP_FLAGS_HWASAN_ENABLED)
        || (property->client.flags & APP_COLD_START);
}

static int AddChildWatcher(AppSpawningCtx *property)
{
    uint32_t defTimeout = IsChildColdRun(property) ? COLD_CHILD_RESPONSE_TIMEOUT : WAIT_CHILD_RESPONSE_TIMEOUT;
    uint32_t timeout = GetSpawnTimeout(defTimeout);

    LE_WatchInfo watchInfo = {};
    watchInfo.fd = property->forkCtx.fd[0];
    watchInfo.flags = WATCHER_ONCE;
    watchInfo.events = EVENT_READ;
    watchInfo.processEvent = ProcessChildResponse;
    LE_STATUS status = LE_StartWatcher(LE_GetDefaultLoop(), &property->forkCtx.watcherHandle, &watchInfo, property);
    APPSPAWN_LOGV("AddChildWatcher with timeout %{public}u fd %{public}d", timeout, watchInfo.fd);
    APPSPAWN_CHECK(status == LE_SUCCESS,
        return APPSPAWN_SYSTEM_ERROR, "Failed to watch child %{public}d", property->pid);
    status = LE_CreateTimer(LE_GetDefaultLoop(), &property->forkCtx.timer, WaitChildTimeout, property);
    if (status == LE_SUCCESS) {
        status = LE_StartTimer(LE_GetDefaultLoop(), property->forkCtx.timer, timeout * 1000, 0); // 1000 1s
    }
    if (status != LE_SUCCESS) {
        if (property->forkCtx.timer != NULL) {
            LE_StopTimer(LE_GetDefaultLoop(), property->forkCtx.timer);
        }
        property->forkCtx.timer = NULL;
        LE_RemoveWatcher(LE_GetDefaultLoop(), property->forkCtx.watcherHandle);
        property->forkCtx.watcherHandle = NULL;
        APPSPAWN_LOGE("Failed to watch child %{public}d", property->pid);
        return APPSPAWN_SYSTEM_ERROR;
    }
    return 0;
}

static bool IsSupportRunHnp()
{
    char buffer[PARAM_BUFFER_SIZE] = {0};
    int ret = GetParameter("const.startup.hnp.execute.enable", "false", buffer, PARAM_BUFFER_SIZE);
    if (ret <= 0) {
        APPSPAWN_LOGE("Get hnp execute enable param unsuccess! ret =%{public}d", ret);
        return false;
    }

    if (strcmp(buffer, "true") == 0) {
        return true;
    }

    return false;
}

static void ClearMMAP(int clientId, uint32_t memSize)
{
    char path[PATH_MAX] = {0};
    int ret = snprintf_s(path, sizeof(path), sizeof(path) - 1, APPSPAWN_MSG_DIR "appspawn/prefork_%d", clientId);
    APPSPAWN_CHECK_ONLY_LOG(ret > 0, "snprintf failed with %{public}d %{public}d", ret, errno);
    if (ret > 0 && access(path, F_OK) == 0) {
        ret =  unlink(path);
        APPSPAWN_CHECK_ONLY_LOG(ret == 0, "prefork unlink result %{public}d %{public}d", ret, errno);
    }

    AppSpawnContent *content = GetAppSpawnContent();
    if (content != NULL && content->propertyBuffer != NULL) {
        ret = munmap(content->propertyBuffer, memSize);
        APPSPAWN_CHECK_ONLY_LOG(ret == 0, "munmap failed %{public}d %{public}d", ret, errno);
        content->propertyBuffer = NULL;
    }
}

static int WritePreforkMsg(AppSpawningCtx *property, uint32_t memSize)
{
    AppSpawnContent *content = GetAppSpawnContent();
    if (content == NULL || content->propertyBuffer == NULL) {
        APPSPAWN_LOGE("buffer is null can not write propery");
        return  -1;
    }

    int ret = memcpy_s(content->propertyBuffer, memSize, &property->message->msgHeader, sizeof(AppSpawnMsg));
    if (ret != 0) {
        APPSPAWN_LOGE("memcpys_s msgHeader failed");
        ClearMMAP(property->client.id, memSize);
        return ret;
    }

    ret = memcpy_s((char *)content->propertyBuffer + sizeof(AppSpawnMsg), memSize - sizeof(AppSpawnMsg),
        property->message->buffer, property->message->msgHeader.msgLen - sizeof(AppSpawnMsg));
    if (ret != 0) {
        APPSPAWN_LOGE("memcpys_s AppSpawnMsg failed");
        ClearMMAP(property->client.id, memSize);
        return ret;
    }
    ret = munmap((char *)content->propertyBuffer, memSize);
    APPSPAWN_CHECK_ONLY_LOG(ret == 0, "munmap failed %{public}d,%{public}d", ret, errno);
    content->propertyBuffer = NULL;
    return ret;
}

static int GetAppSpawnMsg(AppSpawningCtx *property, uint32_t memSize)
{
    uint8_t *buffer = (uint8_t *)GetMapMem(property->client.id, "prefork", memSize, true, false);
    APPSPAWN_CHECK(buffer != NULL, return -1, "prefork buffer is null can not write propery");

    uint32_t msgRecvLen = 0;
    uint32_t remainLen = 0;
    property->forkCtx.childMsg = (char *)buffer;
    property->forkCtx.msgSize = memSize;
    AppSpawnMsgNode *message = NULL;
    int ret = GetAppSpawnMsgFromBuffer(buffer, ((AppSpawnMsg *)buffer)->msgLen, &message, &msgRecvLen, &remainLen);
    // release map
    APPSPAWN_LOGV("prefork GetAppSpawnMsg ret:%{public}d", ret);
    if (ret == 0 && DecodeAppSpawnMsg(message) == 0 && CheckAppSpawnMsg(message) == 0) {
        property->message = message;
        message = NULL;
        return 0;
    }
    return -1;
}

static int SetPreforkProcessName(AppSpawnContent *content)
{
    int ret = prctl(PR_SET_NAME, PREFORK_PROCESS);
    if (ret == -1) {
        return errno;
    }

    ret = memset_s(content->longProcName,
        (size_t)content->longProcNameLen, 0, (size_t)content->longProcNameLen);
    if (ret != EOK) {
        return EINVAL;
    }

    ret = strncpy_s(content->longProcName, content->longProcNameLen,
        PREFORK_PROCESS, strlen(PREFORK_PROCESS));
    if (ret != EOK) {
        return EINVAL;
    }

    return 0;
}

static void ClearPipeFd(int pipe[], int length)
{
    for (int i = 0; i < length; i++) {
        if (pipe[i] > 0) {
            close(pipe[i]);
            pipe[i] = -1;
        }
    }
}

static void ProcessPreFork(AppSpawnContent *content, AppSpawningCtx *property)
{
    APPSPAWN_CHECK(pipe(content->preforkFd) == 0, return, "prefork with prefork pipe failed %{public}d", errno);
    ClearPipeFd(content->parentToChildFd, PIPE_FD_LENGTH);
    APPSPAWN_LOGV("clear pipe fd finish %{public}d %{public}d",
        content->parentToChildFd[0], content->parentToChildFd[1]);
    APPSPAWN_CHECK(pipe(content->parentToChildFd) == 0, ClearPipeFd(content->preforkFd, PIPE_FD_LENGTH);
        return, "prefork with prefork pipe failed %{public}d", errno);
    enum fdsan_error_level errorLevel = fdsan_get_error_level();
    StartAppspawnTrace("AppspawnPreFork");
    content->reservedPid = fork();
    APPSPAWN_LOGV("prefork fork finish %{public}d,%{public}d,%{public}d,%{public}d,%{public}d",
        content->reservedPid, content->preforkFd[0], content->preforkFd[1], content->parentToChildFd[0],
        content->parentToChildFd[1]);
    if (content->reservedPid == 0) {
        ClearPipeFd(property->forkCtx.fd, PIPE_FD_LENGTH);
        int isRet = SetPreforkProcessName(content);
        APPSPAWN_LOGV("prefork process start wait read msg with set processname %{public}d", isRet);
        AppSpawnPreforkMsg preforkMsg = {0};
        int infoSize = read(content->parentToChildFd[0], &preforkMsg, sizeof(AppSpawnPreforkMsg));
        if (infoSize != sizeof(AppSpawnPreforkMsg) || preforkMsg.msgLen > MAX_MSG_TOTAL_LENGTH ||
            preforkMsg.msgLen < sizeof(AppSpawnMsg)) {
            APPSPAWN_LOGE("prefork process read msg failed %{public}d,%{public}d", infoSize, errno);
            ProcessExit(0);
            return;
        }

        property->client.id = preforkMsg.id;
        property->client.flags = preforkMsg.flags;
        property->isPrefork = true;
        property->forkCtx.fd[0] = content->preforkFd[0];
        property->forkCtx.fd[1] = content->preforkFd[1];
        property->state = APP_STATE_SPAWNING;
        const uint32_t memSize = (preforkMsg.msgLen / MAX_MSG_BLOCK_LEN + 1) * MAX_MSG_BLOCK_LEN;
        if (GetAppSpawnMsg(property, memSize) == -1) {
            APPSPAWN_LOGE("prefork child read GetAppSpawnMsg failed");
            ClearMMAP(property->client.id, memSize);
            content->notifyResToParent(content, &property->client, APPSPAWN_MSG_INVALID);
            ProcessExit(0);
            return;
        }
        ClearMMAP(property->client.id, memSize);
        // Inherit the error level of the original process
        (void)fdsan_set_error_level(errorLevel);
        ProcessExit(AppSpawnChild(content, &property->client));
    } else {
        FinishAppspawnTrace();
        if (content->reservedPid < 0) {
            ClearPipeFd(content->preforkFd, PIPE_FD_LENGTH);
            APPSPAWN_LOGE("prefork fork child process failed %{public}d", content->reservedPid);
        }
    }
}

static int NormalSpawnChild(AppSpawnContent *content, AppSpawnClient *client, pid_t *childPid)
{
    APPSPAWN_CHECK(client != NULL, return APPSPAWN_ARG_INVALID, "client is null");
    int ret = InitForkContext((AppSpawningCtx *)client);
    APPSPAWN_CHECK(ret == 0, return ret, "init fork context failed");
    return AppSpawnProcessMsg(content, client, childPid);
}

static int AppSpawnProcessMsgForPrefork(AppSpawnContent *content, AppSpawnClient *client, pid_t *childPid)
{
    int ret = 0;
    AppSpawningCtx *property = (AppSpawningCtx *)client;
    if (content->reservedPid <= 0) {
        ret = NormalSpawnChild(content, client, childPid);
    } else {
        const uint32_t memSize = (property->message->msgHeader.msgLen / MAX_MSG_BLOCK_LEN + 1) *
            MAX_MSG_BLOCK_LEN;
        content->propertyBuffer = GetMapMem(property->client.id, "prefork", memSize, false, false);
        if (content->propertyBuffer == NULL) {
            ClearMMAP(property->client.id, memSize);
            return NormalSpawnChild(content, client, childPid);
        }
        ret = WritePreforkMsg(property, memSize);
        if (ret != 0) {
            ClearMMAP(property->client.id, memSize);
            return NormalSpawnChild(content, client, childPid);
        }
        *childPid = content->reservedPid;
        property->forkCtx.fd[0] = content->preforkFd[0];
        property->forkCtx.fd[1] = content->preforkFd[1];
        int option = fcntl(property->forkCtx.fd[0], F_GETFD);
        if (option > 0) {
            ret = fcntl(property->forkCtx.fd[0], F_SETFD, (unsigned int)option | O_NONBLOCK);
            APPSPAWN_CHECK_ONLY_LOG(ret == 0, "fcntl failed %{public}d,%{public}d", ret, errno);
        }
        AppSpawnPreforkMsg *preforkMsg = (AppSpawnPreforkMsg *)calloc(1, sizeof(AppSpawnPreforkMsg));
        if (preforkMsg == NULL) {
            APPSPAWN_LOGE("calloc failed");
            ClearMMAP(property->client.id, memSize);
            return NormalSpawnChild(content, client, childPid);
        }

        preforkMsg->id = client->id;
        preforkMsg->flags = client->flags;
        preforkMsg->msgLen = property->message->msgHeader.msgLen;
        ssize_t writesize = write(content->parentToChildFd[1], preforkMsg, sizeof(AppSpawnPreforkMsg)) ;
        APPSPAWN_CHECK(writesize == sizeof(AppSpawnPreforkMsg), kill(*childPid, SIGKILL);
            *childPid = 0;
            ret = -1,
            "write msg to child failed %{public}d", errno);
        free(preforkMsg);
    }
    ProcessPreFork(content, property);
    return ret;
}

static bool IsSupportPrefork(AppSpawnContent *content, AppSpawnClient *client)
{
#ifdef APPSPAWN_SUPPORT_PREFORK
    if (client == NULL || content == NULL) {
        return false;
    }
    if (!content->enablePerfork) {
        APPSPAWN_LOGV("g_enablePrefork %{public}d", content->enablePerfork);
        return false;
    }
    AppSpawningCtx *property = (AppSpawningCtx *)client;

    if (content->mode == MODE_FOR_APP_SPAWN && !IsChildColdRun(property)
        && !CheckAppMsgFlagsSet(property, APP_FLAGS_CHILDPROCESS)) {
        return true;
    }
#endif
    return false;
}

static bool IsBootFinished(void)
{
    char buffer[32] = {0};  // 32 max
    int ret = GetParameter("bootevent.boot.completed", "false", buffer, sizeof(buffer));
    bool isBootCompleted = (ret > 0 && strcmp(buffer, "true") == 0);
    return isBootCompleted;
}


static int RunAppSpawnProcessMsg(AppSpawnContent *content, AppSpawnClient *client, pid_t *childPid)
{
    int ret = 0;

    if (IsBootFinished() && IsSupportPrefork(content, client)) {
        ret = AppSpawnProcessMsgForPrefork(content, client, childPid);
    } else {
        ret = NormalSpawnChild(content, client, childPid);
    }
    return ret;
}

static void ProcessSpawnReqMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    int ret = CheckAppSpawnMsg(message);
    if (ret != 0) {
        SendResponse(connection, &message->msgHeader, ret, 0);
        DeleteAppSpawnMsg(&message);
        return;
    }

    if (IsDeveloperModeOpen()) {
        if (IsSupportRunHnp()) {
            SetAppSpawnMsgFlag(message, TLV_MSG_FLAGS, APP_FLAGS_DEVELOPER_MODE);
        } else {
            APPSPAWN_LOGV("Not support execute hnp file!");
        }
    }

    AppSpawningCtx *property = CreateAppSpawningCtx();
    if (property == NULL) {
        SendResponse(connection, &message->msgHeader, APPSPAWN_SYSTEM_ERROR, 0);
        DeleteAppSpawnMsg(&message);
        return;
    }

    property->state = APP_STATE_SPAWNING;
    property->message = message;
    message->connection = connection;
    // mount el2 dir
    // getWrapBundleNameValue
    AppSpawnHookExecute(STAGE_PARENT_PRE_FORK, 0, GetAppSpawnContent(), &property->client);
    DumpAppSpawnMsg(property->message);

    clock_gettime(CLOCK_MONOTONIC, &property->spawnStart);
    ret = RunAppSpawnProcessMsg(GetAppSpawnContent(), &property->client, &property->pid);
    AppSpawnHookExecute(STAGE_PARENT_POST_FORK, 0, GetAppSpawnContent(), &property->client);
    if (ret != 0) { // wait child process result
        SendResponse(connection, &message->msgHeader, ret, 0);
        DeleteAppSpawningCtx(property);
        return;
    }
    if (AddChildWatcher(property) != 0) { // wait child process result
        kill(property->pid, SIGKILL);
        SendResponse(connection, &message->msgHeader, ret, 0);
        DeleteAppSpawningCtx(property);
        return;
    }
}

static uint32_t g_lastDiedAppId = 0;
static uint32_t g_crashTimes = 0;
#define MAX_CRASH_TIME 5
static void WaitChildDied(pid_t pid)
{
    AppSpawningCtx *property = GetAppSpawningCtxByPid(pid);
    if (property != NULL && property->message != NULL && property->state == APP_STATE_SPAWNING) {
        const char *processName = GetProcessName(property);
        APPSPAWN_LOGI("Child process %{public}s fail \'child crash \'pid %{public}d appId: %{public}d",
            processName, property->pid, property->client.id);
#ifdef APPSPAWN_HISYSEVENT
        ReportSpawnChildProcessFail(processName, ERR_APPSPAWN_CHILD_CRASH, APPSPAWN_CHILD_CRASH);
#endif
        if (property->client.id == g_lastDiedAppId + 1) {
            g_crashTimes++;
        } else {
            g_crashTimes = 1;
        }
        g_lastDiedAppId = property->client.id;

        SendResponse(property->message->connection, &property->message->msgHeader, APPSPAWN_CHILD_CRASH, 0);
        DeleteAppSpawningCtx(property);

        if (g_crashTimes >= MAX_CRASH_TIME) {
            APPSPAWN_LOGW("Continuous failures in spawning the app, restart appspawn");
#ifdef APPSPAWN_HISYSEVENT
            ReportKeyEvent(APPSPAWN_MAX_FAILURES_EXCEEDED);
#endif
            StopAppSpawn();
        }
    }
}

static void WaitChildTimeout(const TimerHandle taskHandle, void *context)
{
    AppSpawningCtx *property = (AppSpawningCtx *)context;
    APPSPAWN_LOGI("Child process %{public}s fail \'wait child timeout \'pid %{public}d appId: %{public}d",
        GetProcessName(property), property->pid, property->client.id);
    if (property->pid > 0) {
#if (!defined(CJAPP_SPAWN) && !defined(NATIVE_SPAWN))
        DumpSpawnStack(property->pid);
#endif
        kill(property->pid, SIGKILL);
    }
#ifdef APPSPAWN_HISYSEVENT
    ReportSpawnChildProcessFail(GetProcessName(property), ERR_APPSPAWN_SPAWN_TIMEOUT, APPSPAWN_SPAWN_TIMEOUT);
#endif
    SendResponse(property->message->connection, &property->message->msgHeader, APPSPAWN_SPAWN_TIMEOUT, 0);
    DeleteAppSpawningCtx(property);
}

static int ProcessChildFdCheck(int fd, AppSpawningCtx *property)
{
    int result = 0;
    (void)read(fd, &result, sizeof(result));
    APPSPAWN_LOGI("Child process %{public}s success pid %{public}d appId: %{public}d result: %{public}d",
        GetProcessName(property), property->pid, property->client.id, result);
    APPSPAWN_CHECK(property->message != NULL, return -1, "Invalid message in ctx %{public}d", property->client.id);

    if (result != 0) {
#ifdef APPSPAWN_HISYSEVENT
        ReportSpawnChildProcessFail(GetProcessName(property), ERR_APPSPAWN_SPAWN_FAIL, result);
#endif
        SendResponse(property->message->connection, &property->message->msgHeader, result, property->pid);
        DeleteAppSpawningCtx(property);
        return -1;
    }

    return 0;
}

static void ProcessChildResponse(const WatcherHandle taskHandle, int fd, uint32_t *events, const void *context)
{
    AppSpawningCtx *property = (AppSpawningCtx *)context;
    property->forkCtx.watcherHandle = NULL;  // delete watcher
    LE_RemoveWatcher(LE_GetDefaultLoop(), (WatcherHandle)taskHandle);

    if (ProcessChildFdCheck(fd, property) != 0) {
        return;
    }

    // success
    bool isDebuggable = CheckAppMsgFlagsSet(property, APP_FLAGS_DEBUGGABLE);
    uint32_t appIndex = 0;
    if (CheckAppMsgFlagsSet(property, APP_FLAGS_CLONE_ENABLE)) {
        AppSpawnMsgBundleInfo *bundleInfo = (AppSpawnMsgBundleInfo *)GetAppProperty(property, TLV_BUNDLE_INFO);
        if (bundleInfo != NULL) {
            appIndex = bundleInfo->bundleIndex;
        }
    }
    AppSpawnedProcess *appInfo = AddSpawnedProcess(property->pid, GetBundleName(property), appIndex, isDebuggable);
    if (appInfo) {
        AppSpawnMsgDacInfo *dacInfo = GetAppProperty(property, TLV_DAC_INFO);
        appInfo->uid = dacInfo != NULL ? dacInfo->uid : 0;
        appInfo->spawnStart.tv_sec = property->spawnStart.tv_sec;
        appInfo->spawnStart.tv_nsec = property->spawnStart.tv_nsec;
#ifdef DEBUG_BEGETCTL_BOOT
        if (IsDeveloperModeOpen()) {
            appInfo->message = property->message;
        }
#endif
        clock_gettime(CLOCK_MONOTONIC, &appInfo->spawnEnd);

#ifdef APPSPAWN_HISYSEVENT
        //add process spawn duration into hisysevent,(ms)
        uint32_t spawnProcessDuration = (uint32_t)((appInfo->spawnEnd.tv_sec - appInfo->spawnStart.tv_sec) *
            (APPSPAWN_USEC_TO_NSEC)) +(uint32_t)((appInfo->spawnEnd.tv_nsec - appInfo->spawnStart.tv_nsec) /
            (APPSPAWN_MSEC_TO_NSEC));
        AppSpawnMgr *appspawnMgr = GetAppSpawnMgr();
        if (appspawnMgr != NULL) {
            AddStatisticEventInfo(appspawnMgr->hisyseventInfo, spawnProcessDuration, IsBootFinished());
        }
#ifndef ASAN_DETECTOR
        uint64_t diff = DiffTime(&appInfo->spawnStart, &appInfo->spawnEnd);
        APPSPAWN_CHECK_ONLY_EXPER(diff < (IsChildColdRun(property) ? SPAWN_COLDRUN_DURATION : SPAWN_DURATION),
            ReportAbnormalDuration("SPAWNCHILD", diff));
#endif
#endif
    }

    WatchChildProcessFd(property);
    ProcessMgrHookExecute(STAGE_SERVER_APP_ADD, GetAppSpawnContent(), appInfo);
    // response
    AppSpawnHookExecute(STAGE_PARENT_PRE_RELY, 0, GetAppSpawnContent(), &property->client);
    SendResponse(property->message->connection, &property->message->msgHeader, 0, property->pid);
    AppSpawnHookExecute(STAGE_PARENT_POST_RELY, 0, GetAppSpawnContent(), &property->client);
#ifdef DEBUG_BEGETCTL_BOOT
    if (IsDeveloperModeOpen()) {
        property->message = NULL;
    }
#endif
    DeleteAppSpawningCtx(property);
}

static void NotifyResToParent(AppSpawnContent *content, AppSpawnClient *client, int result)
{
    AppSpawningCtx *property = (AppSpawningCtx *)client;
    int fd = property->forkCtx.fd[1];
    if (fd >= 0) {
        (void)write(fd, &result, sizeof(result));
        (void)close(fd);
        property->forkCtx.fd[1] = -1;
    }
    APPSPAWN_LOGV("NotifyResToParent client id: %{public}u result: 0x%{public}x", client->id, result);
}

static int CreateAppSpawnServer(TaskHandle *server, const char *socketName)
{
    char path[128] = {0};  // 128 max path
    int ret = snprintf_s(path, sizeof(path), sizeof(path) - 1, "%s%s", APPSPAWN_SOCKET_DIR, socketName);
    APPSPAWN_CHECK(ret > 0, return -1, "Failed to snprintf_s %{public}d", ret);
    int socketId = GetControlSocket(socketName);
    APPSPAWN_LOGI("get socket form env %{public}s socketId %{public}d", socketName, socketId);

    LE_StreamServerInfo info = {};
    info.baseInfo.flags = TASK_STREAM | TASK_PIPE | TASK_SERVER;
    info.socketId = socketId;
    info.server = path;
    info.baseInfo.close = NULL;
    info.incommingConnect = OnConnection;

    MakeDirRec(path, 0711, 0);  // 0711 default mask
    ret = LE_CreateStreamServer(LE_GetDefaultLoop(), server, &info);
    APPSPAWN_CHECK(ret == 0, return -1, "Failed to create socket for %{public}s errno: %{public}d", path, errno);
    SetFdCtrl(LE_GetSocketFd(*server), FD_CLOEXEC);

    APPSPAWN_LOGI("CreateAppSpawnServer path %{public}s fd %{public}d", path, LE_GetSocketFd(*server));
    return 0;
}

void AppSpawnDestroyContent(AppSpawnContent *content)
{
    if (content == NULL) {
        return;
    }
    if (content->parentToChildFd[0] > 0) {
        close(content->parentToChildFd[0]);
        content->parentToChildFd[0] = -1;
    }
    if (content->parentToChildFd[1] > 0) {
        close(content->parentToChildFd[1]);
        content->parentToChildFd[1] = -1;
    }
    AppSpawnMgr *appSpawnContent = (AppSpawnMgr *)content;
    if (appSpawnContent->sigHandler != NULL && appSpawnContent->servicePid == getpid()) {
        LE_CloseSignalTask(LE_GetDefaultLoop(), appSpawnContent->sigHandler);
    }

    if (appSpawnContent->server != NULL && appSpawnContent->servicePid == getpid()) { // childProcess can't deal socket
        LE_CloseStreamTask(LE_GetDefaultLoop(), appSpawnContent->server);
        appSpawnContent->server = NULL;
    }
    DeleteAppSpawnMgr(appSpawnContent);
    LE_StopLoop(LE_GetDefaultLoop());
    LE_CloseLoop(LE_GetDefaultLoop());
}

APPSPAWN_STATIC int AppSpawnColdStartApp(struct AppSpawnContent *content, AppSpawnClient *client)
{
    AppSpawningCtx *property = (AppSpawningCtx *)client;
#ifdef CJAPP_SPAWN
    char *path = property->forkCtx.coldRunPath != NULL ? property->forkCtx.coldRunPath : "/system/bin/cjappspawn";
#elif NATIVE_SPAWN
    char *path = property->forkCtx.coldRunPath != NULL ? property->forkCtx.coldRunPath : "/system/bin/nativespawn";
#elif NWEB_SPAWN
    char *path = property->forkCtx.coldRunPath != NULL ? property->forkCtx.coldRunPath : "/system/bin/nwebspawn";
#else
    char *path = property->forkCtx.coldRunPath != NULL ? property->forkCtx.coldRunPath : "/system/bin/appspawn";
#endif
    APPSPAWN_LOGI("ColdStartApp::processName: %{public}s path: %{public}s", GetProcessName(property), path);

    // for cold run, use shared memory to exchange message
    APPSPAWN_LOGV("Write msg to child %{public}s", GetProcessName(property));
    int ret = WriteMsgToChild(property, IsNWebSpawnMode((AppSpawnMgr *)content));
    APPSPAWN_CHECK(ret == 0, return APPSPAWN_SYSTEM_ERROR, "Failed to write msg to child");

    char buffer[4][32] = {0};  // 4 32 buffer for fd
    int len = sprintf_s(buffer[0], sizeof(buffer[0]), " %d ", property->forkCtx.fd[1]);
    APPSPAWN_CHECK(len > 0, return APPSPAWN_SYSTEM_ERROR, "Invalid to format fd");
    len = sprintf_s(buffer[1], sizeof(buffer[1]), " %u ", property->client.flags);
    APPSPAWN_CHECK(len > 0, return APPSPAWN_SYSTEM_ERROR, "Invalid to format flags");
    len = sprintf_s(buffer[2], sizeof(buffer[2]), " %u ", property->forkCtx.msgSize); // 2 2 index for dest path
    APPSPAWN_CHECK(len > 0, return APPSPAWN_SYSTEM_ERROR, "Invalid to format shmId ");
    len = sprintf_s(buffer[3], sizeof(buffer[3]), " %u ", property->client.id); // 3 3 index for client id
    APPSPAWN_CHECK(len > 0, return APPSPAWN_SYSTEM_ERROR, "Invalid to format shmId ");
#ifndef APPSPAWN_TEST
    char *mode = IsNWebSpawnMode((AppSpawnMgr *)content) ? "nweb_cold" : "app_cold";
    // 2 2 index for dest path
    const char *const formatCmds[] = {
        path, "-mode", mode, "-fd", buffer[0], buffer[1], buffer[2],
        "-param", GetProcessName(property), buffer[3], NULL
    };

    ret = execv(path, (char **)formatCmds);
    if (ret != 0) {
        APPSPAWN_LOGE("Failed to execv, errno: %{public}d", errno);
    }
#endif
    APPSPAWN_LOGV("ColdStartApp::processName: %{public}s end", GetProcessName(property));
    return 0;
}

static AppSpawningCtx *GetAppSpawningCtxFromArg(AppSpawnMgr *content, int argc, char *const argv[])
{
    AppSpawningCtx *property = CreateAppSpawningCtx();
    APPSPAWN_CHECK(property != NULL, return NULL, "Create app spawning ctx fail");
    property->forkCtx.fd[1] = atoi(argv[FD_VALUE_INDEX]);
    property->client.flags = (uint32_t)atoi(argv[FLAGS_VALUE_INDEX]);
    property->client.flags &= ~APP_COLD_START;

    int isNweb = IsNWebSpawnMode(content);
    uint32_t size = (uint32_t)atoi(argv[SHM_SIZE_INDEX]);
    property->client.id = (uint32_t)atoi(argv[CLIENT_ID_INDEX]);
    uint8_t *buffer = (uint8_t *)GetMapMem(property->client.id,
        argv[PARAM_VALUE_INDEX], size, true, isNweb);
    if (buffer == NULL) {
        APPSPAWN_LOGE("Failed to map errno %{public}d %{public}s", property->client.id, argv[PARAM_VALUE_INDEX]);
        NotifyResToParent(&content->content, &property->client, APPSPAWN_SYSTEM_ERROR);
        DeleteAppSpawningCtx(property);
        return NULL;
    }

    uint32_t msgRecvLen = 0;
    uint32_t remainLen = 0;
    AppSpawnMsgNode *message = NULL;
    int ret = GetAppSpawnMsgFromBuffer(buffer, ((AppSpawnMsg *)buffer)->msgLen, &message, &msgRecvLen, &remainLen);
    // release map
    munmap((char *)buffer, size);
    //unlink
    char path[PATH_MAX] = {0};
    int len = sprintf_s(path, sizeof(path), APPSPAWN_MSG_DIR "%s/%s_%u",
        isNweb ? "nwebspawn" : "appspawn", argv[PARAM_VALUE_INDEX], property->client.id);
    if (len > 0) {
        unlink(path);
    }

    if (ret == 0 && DecodeAppSpawnMsg(message) == 0 && CheckAppSpawnMsg(message) == 0) {
        property->message = message;
        message = NULL;
        return property;
    }
    NotifyResToParent(&content->content, &property->client, APPSPAWN_MSG_INVALID);
    DeleteAppSpawnMsg(&message);
    DeleteAppSpawningCtx(property);
    return NULL;
}

static void AppSpawnColdRun(AppSpawnContent *content, int argc, char *const argv[])
{
    APPSPAWN_CHECK(argc >= ARG_NULL, return, "Invalid arg for cold start %{public}d", argc);
    AppSpawnMgr *appSpawnContent = (AppSpawnMgr *)content;
    APPSPAWN_CHECK(appSpawnContent != NULL, return, "Invalid appspawn content");

    AppSpawningCtx *property = GetAppSpawningCtxFromArg(appSpawnContent, argc, argv);
    if (property == NULL) {
        APPSPAWN_LOGE("Failed to get property from arg");
        return;
    }
    DumpAppSpawnMsg(property->message);

    int ret = AppSpawnExecuteSpawningHook(content, &property->client);
    if (ret == 0) {
        ret = AppSpawnExecutePreReplyHook(content, &property->client);
        // success
        NotifyResToParent(content, &property->client, ret);

        (void)AppSpawnExecutePostReplyHook(content, &property->client);

        ret = APPSPAWN_SYSTEM_ERROR;
        if (content->runChildProcessor != NULL) {
            ret = content->runChildProcessor(content, &property->client);
        }
    } else {
        NotifyResToParent(content, &property->client, ret);
    }

    if (ret != 0) {
        AppSpawnEnvClear(content, &property->client);
    }
    APPSPAWN_LOGI("AppSpawnColdRun exit %{public}d.", getpid());
}

static void AppSpawnRun(AppSpawnContent *content, int argc, char *const argv[])
{
    APPSPAWN_LOGI("AppSpawnRun");
    AppSpawnMgr *appSpawnContent = (AppSpawnMgr *)content;
    APPSPAWN_CHECK(appSpawnContent != NULL, return, "Invalid appspawn content");

    LE_STATUS status = LE_CreateSignalTask(LE_GetDefaultLoop(), &appSpawnContent->sigHandler, ProcessSignal);
    if (status == 0) {
        (void)LE_AddSignal(LE_GetDefaultLoop(), appSpawnContent->sigHandler, SIGCHLD);
        (void)LE_AddSignal(LE_GetDefaultLoop(), appSpawnContent->sigHandler, SIGTERM);
    }

    if (IsAppSpawnMode(appSpawnContent)) {
        struct sched_param param = { 0 };
        param.sched_priority = 1;
        int ret = sched_setscheduler(0, SCHED_FIFO, &param);
        APPSPAWN_CHECK_ONLY_LOG(ret == 0, "UpdateSchedPrio failed ret: %{public}d, %{public}d", ret, errno);
    }
    LE_RunLoop(LE_GetDefaultLoop());
    APPSPAWN_LOGI("AppSpawnRun exit mode: %{public}d ", content->mode);

    (void)ServerStageHookExecute(STAGE_SERVER_EXIT, content); // service exit,plugin can deal task
    AppSpawnDestroyContent(content);
}

APPSPAWN_STATIC int AppSpawnClearEnv(AppSpawnMgr *content, AppSpawningCtx *property)
{
    StartAppspawnTrace("AppSpawnClearEnv");
    APPSPAWN_CHECK(content != NULL, return 0, "Invalid appspawn content");
    DeleteAppSpawningCtx(property);
    AppSpawnDestroyContent(&content->content);
    APPSPAWN_LOGV("clear %{public}d end", getpid());
    FinishAppspawnTrace();
    return 0;
}

static int IsEnablePrefork(void)
{
    char buffer[32] = {0};
    int ret = GetParameter("persist.sys.prefork.enable", "true", buffer, sizeof(buffer));
    APPSPAWN_LOGV("IsEnablePrefork result %{public}d, %{public}s", ret, buffer);
    return strcmp(buffer, "true") == 0;
}

AppSpawnContent *AppSpawnCreateContent(const char *socketName, char *longProcName, uint32_t nameLen, int mode)
{
    APPSPAWN_CHECK(socketName != NULL && longProcName != NULL, return NULL, "Invalid name");
    APPSPAWN_LOGI("AppSpawnCreateContent %{public}s %{public}u mode %{public}d", socketName, nameLen, mode);

    AppSpawnMgr *appSpawnContent = CreateAppSpawnMgr(mode);
    APPSPAWN_CHECK(appSpawnContent != NULL, return NULL, "Failed to alloc memory for appspawn");
#ifdef APPSPAWN_HISYSEVENT
    appSpawnContent->hisyseventInfo = InitHisyseventTimer();
#endif
    appSpawnContent->content.longProcName = longProcName;
    appSpawnContent->content.longProcNameLen = nameLen;
    appSpawnContent->content.notifyResToParent = NotifyResToParent;
    if (IsColdRunMode(appSpawnContent)) {
        appSpawnContent->content.runAppSpawn = AppSpawnColdRun;
    } else {
        appSpawnContent->content.runAppSpawn = AppSpawnRun;
        appSpawnContent->content.coldStartApp = AppSpawnColdStartApp;

        int ret = CreateAppSpawnServer(&appSpawnContent->server, socketName);
        APPSPAWN_CHECK(ret == 0, AppSpawnDestroyContent(&appSpawnContent->content);
            return NULL, "Failed to create server");
    }
    appSpawnContent->content.enablePerfork = IsEnablePrefork();
    return &appSpawnContent->content;
}

AppSpawnContent *StartSpawnService(const AppSpawnStartArg *startArg, uint32_t argvSize, int argc, char *const argv[])
{
    APPSPAWN_CHECK(startArg != NULL && argv != NULL, return NULL, "Invalid start arg");
    pid_t pid = 0;
    AppSpawnStartArg *arg = (AppSpawnStartArg *)startArg;
    APPSPAWN_LOGV("Start appspawn argvSize %{public}d mode %{public}d service %{public}s",
        argvSize, arg->mode, arg->serviceName);

    if (arg->initArg) {
        int ret = memset_s(argv[0], argvSize, 0, (size_t)argvSize);
        APPSPAWN_CHECK(ret == EOK, return NULL, "Failed to memset argv[0]");
        ret = strncpy_s(argv[0], argvSize, arg->serviceName, strlen(arg->serviceName));
        APPSPAWN_CHECK(ret == EOK, return NULL, "Failed to copy service name %{public}s", arg->serviceName);
    }

    // load module appspawn/common
    AppSpawnLoadAutoRunModules(MODULE_COMMON);
    AppSpawnModuleMgrInstall(ASAN_MODULE_PATH);

    APPSPAWN_CHECK(LE_GetDefaultLoop() != NULL, return NULL, "Invalid default loop");
    AppSpawnContent *content = AppSpawnCreateContent(arg->socketName, argv[0], argvSize, arg->mode);
    APPSPAWN_CHECK(content != NULL, return NULL, "Failed to create content for %{public}s", arg->socketName);

    AppSpawnLoadAutoRunModules(arg->moduleType);  // load corresponding plugin according to startup mode
    int ret = ServerStageHookExecute(STAGE_SERVER_PRELOAD, content);   // Preload, prase the sandbox
    APPSPAWN_CHECK(ret == 0, AppSpawnDestroyContent(content);
        return NULL, "Failed to prepare load %{public}s result: %{public}d", arg->serviceName, ret);
#ifndef APPSPAWN_TEST
    APPSPAWN_CHECK(content->runChildProcessor != NULL, AppSpawnDestroyContent(content);
        return NULL, "No child processor %{public}s result: %{public}d", arg->serviceName, ret);
#endif
    AddAppSpawnHook(STAGE_CHILD_PRE_RUN, HOOK_PRIO_LOWEST, AppSpawnClearEnv);
    if (arg->mode == MODE_FOR_APP_SPAWN) {
        SetParameter("bootevent.appspawn.started", "true");
    }
    return content;
}

static AppSpawnMsgNode *ProcessSpawnBegetctlMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    uint32_t len = 0;
    const char *cmdMsg = (const char *)GetAppSpawnMsgExtInfo(message, MSG_EXT_NAME_BEGET_PID, &len);
    APPSPAWN_CHECK(cmdMsg != NULL, return NULL, "Failed to get extInfo");
    AppSpawnedProcess *appInfo = GetSpawnedProcess(atoi(cmdMsg));
    APPSPAWN_CHECK(appInfo != NULL, return NULL, "Failed to get app info");
    AppSpawnMsgNode *msgNode = RebuildAppSpawnMsgNode(message, appInfo);
    APPSPAWN_CHECK(msgNode != NULL, return NULL, "Failed to rebuild app message node");
    int ret = DecodeAppSpawnMsg(msgNode);
    if (ret != 0) {
        DeleteAppSpawnMsg(&msgNode);
        return NULL;
    }
    return msgNode;
}

static void ProcessBegetCmdMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    AppSpawnMsg *msg = &message->msgHeader;
    if (!IsDeveloperModeOpen()) {
        SendResponse(connection, msg, APPSPAWN_DEBUG_MODE_NOT_SUPPORT, 0);
        DeleteAppSpawnMsg(&message);
        return;
    }
    AppSpawnMsgNode *msgNode = ProcessSpawnBegetctlMsg(connection, message);
    if (msgNode == NULL) {
        SendResponse(connection, msg, APPSPAWN_DEBUG_MODE_NOT_SUPPORT, 0);
        DeleteAppSpawnMsg(&message);
        return;
    }
    ProcessSpawnReqMsg(connection, msgNode);
    DeleteAppSpawnMsg(&message);
    DeleteAppSpawnMsg(&msgNode);
}

static int GetArkWebInstallPath(const char *key, char *value)
{
    int len = GetParameter(key, "", value, PATH_SIZE);
    APPSPAWN_CHECK(len > 0, return -1, "Failed to get arkwebcore install path from param, len %{public}d", len);
    for (int i = len - 1; i >= 0; i--) {
        if (value[i] == '/') {
            value[i] = '\0';
            return i;
        }
        value[i] = '\0';
    }
    return -1;
}

static bool CheckAllDigit(char *userId)
{
    for (int i = 0; userId[i] != '\0'; i++) {
        if (!isdigit(userId[i])) {
            return false;
        }
    }
    return true;
}

#ifdef APPSPAWN_SANDBOX_NEW
static int ProcessSpawnRemountMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    APPSPAWN_LOGI("ProcessSpawnRemountMsg in new sandbox, do not handle it");
    return 0;
}

static void ProcessSpawnRestartMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    APPSPAWN_LOGI("ProcessSpawnRestartMsg in new sandbox, do not handle it");
    SendResponse(connection, &message->msgHeader, 0, 0);
}
#else
static int ProcessSpawnRemountMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    char srcPath[PATH_SIZE] = {0};
    int len = GetArkWebInstallPath("persist.arkwebcore.install_path", srcPath);
    APPSPAWN_CHECK(len > 0, return -1, "Failed to get arkwebcore install path");

    char *rootPath = "/mnt/sandbox";
    DIR *rootDir = opendir(rootPath);
    APPSPAWN_CHECK(rootDir != NULL, return -1, "Failed to opendir %{public}s, errno %{public}d", rootPath, errno);

    struct dirent *ent;
    while ((ent = readdir(rootDir)) != NULL) {
        char *userId = ent->d_name;
        if (strcmp(userId, ".") == 0 || strcmp(userId, "..") == 0 || !CheckAllDigit(userId)) {
            continue;
        }

        char userIdPath[PATH_SIZE] = {0};
        int ret = snprintf_s(userIdPath, sizeof(userIdPath), sizeof(userIdPath) - 1, "%s/%s", rootPath, userId);
        APPSPAWN_CHECK(ret > 0, continue, "Failed to snprintf_s, errno %{public}d", errno);

        DIR *userIdDir = opendir(userIdPath);
        APPSPAWN_CHECK(userIdDir != NULL, continue, "Failed to open %{public}s, errno %{public}d", userIdPath, errno);

        while ((ent = readdir(userIdDir)) != NULL) {
            char *bundleName = ent->d_name;
            if (strcmp(bundleName, ".") == 0 || strcmp(bundleName, "..") == 0) {
                continue;
            }
            char destPath[PATH_SIZE] = {0};
            ret = snprintf_s(destPath, sizeof(destPath), sizeof(destPath) - 1,
                "%s/%s/data/storage/el1/bundle/arkwebcore", userIdPath, bundleName);
            APPSPAWN_CHECK(ret > 0, continue, "Failed to snprintf_s, errno %{public}d", errno);

            umount2(destPath, MNT_DETACH);
            ret = mount(srcPath, destPath, NULL, MS_BIND | MS_REC, NULL);
            if (ret != 0 && errno == EBUSY) {
                ret = mount(srcPath, destPath, NULL, MS_BIND | MS_REC, NULL);
                APPSPAWN_LOGV("mount again %{public}s to %{public}s, ret %{public}d", srcPath, destPath, ret);
            }
            APPSPAWN_CHECK(ret == 0, continue,
                "Failed to bind mount %{public}s to %{public}s, errno %{public}d", srcPath, destPath, errno);

            ret = mount(NULL, destPath, NULL, MS_SHARED, NULL);
            APPSPAWN_CHECK(ret == 0, continue,
                "Failed to shared mount %{public}s to %{public}s, errno %{public}d", srcPath, destPath, errno);

            APPSPAWN_LOGV("Remount %{public}s to %{public}s success", srcPath, destPath);
        }
        closedir(userIdDir);
    }
    closedir(rootDir);
    return 0;
}

static void ProcessSpawnRestartMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    AppSpawnContent *content = GetAppSpawnContent();
    if (!IsNWebSpawnMode((AppSpawnMgr *)content)) {
        SendResponse(connection, &message->msgHeader, APPSPAWN_MSG_INVALID, 0);
        DeleteAppSpawnMsg(&message);
        APPSPAWN_LOGE("Restart msg only support nwebspawn");
        return;
    }

    TraversalSpawnedProcess(AppQueueDestroyProc, NULL);
    SendResponse(connection, &message->msgHeader, 0, 0);
    DeleteAppSpawnMsg(&message);
    (void) ServerStageHookExecute(STAGE_SERVER_EXIT, content);

    errno = 0;
    int fd = GetControlSocket(NWEBSPAWN_SOCKET_NAME);
    APPSPAWN_CHECK(fd >= 0, return, "Get fd failed %{public}d, errno %{public}d", fd, errno);

    int ret = 0;
    int op = fcntl(fd, F_GETFD);
    APPSPAWN_CHECK_ONLY_LOG(op >= 0, "fcntl failed %{public}d", op);
    if (op > 0) {
        ret = fcntl(fd, F_SETFD, (unsigned int)op & ~FD_CLOEXEC);
        if (ret < 0) {
            APPSPAWN_LOGE("Set fd failed %{public}d, %{public}d, ret %{public}d, errno %{public}d", fd, op, ret, errno);
        }
    }

    char *path = "/system/bin/nwebspawn";
    char *mode = NWEBSPAWN_RESTART;
    const char *const formatCmds[] = {path, "-mode", mode, NULL};
    ret = execv(path, (char **)formatCmds);
    APPSPAWN_LOGE("Failed to execv, ret %{public}d, errno %{public}d", ret, errno);
}
#endif

APPSPAWN_STATIC void ProcessUninstallDebugHap(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    APPSPAWN_LOGI("ProcessUninstallDebugHap start");
    AppSpawningCtx *property = CreateAppSpawningCtx();
    if (property == NULL) {
        SendResponse(connection, &message->msgHeader, APPSPAWN_SYSTEM_ERROR, 0);
        DeleteAppSpawnMsg(&message);
        return;
    }
    
    property->message = message;
    property->message->connection = connection;
    int ret = AppSpawnHookExecute(STAGE_PARENT_UNINSTALL, 0, GetAppSpawnContent(), &property->client);
    SendResponse(connection, &message->msgHeader, ret, 0);
    DeleteAppSpawningCtx(property);
}

APPSPAWN_STATIC int AppspawpnDevicedebugKill(int pid, cJSON *args)
{
    cJSON *signal = cJSON_GetObjectItem(args, "signal");
    APPSPAWN_CHECK(cJSON_IsNumber(signal), return -1, "appspawn devicedebug json get signal fail");

    AppSpawnedProcess *appInfo = GetSpawnedProcess(pid);
    APPSPAWN_CHECK(appInfo != NULL, return APPSPAWN_DEVICEDEBUG_ERROR_APP_NOT_EXIST,
        "appspawn devicedebug get app info unsuccess, pid=%{public}d", pid);
    APPSPAWN_CHECK(appInfo->isDebuggable, return APPSPAWN_DEVICEDEBUG_ERROR_APP_NOT_DEBUGGABLE,
        "appspawn devicedebug process is not debuggable, pid=%{public}d", pid);

    APPSPAWN_LOGI("appspawn devicedebug debugable=%{public}d, pid=%{public}d, signal=%{public}d",
        appInfo->isDebuggable, pid, (int)signal->valueint);

    if (kill(pid, signal->valueint) != 0) {
        APPSPAWN_LOGE("appspawn devicedebug unable to kill process, pid: %{public}d ret %{public}d", pid, errno);
        return -1;
    }

    return 0;
}

APPSPAWN_STATIC int AppspawnDevicedebugDeal(const char* op, int pid, cJSON *args)
{
    if (strcmp(op, "kill") == 0) {
        return AppspawpnDevicedebugKill(pid, args);
    }
    APPSPAWN_LOGE("appspawn devicedebug op:%{public}s invaild", op);

    return -1;
}

APPSPAWN_STATIC int ProcessAppSpawnDeviceDebugMsg(AppSpawnMsgNode *message)
{
    APPSPAWN_CHECK_ONLY_EXPER(message != NULL, return -1);
    uint32_t len = 0;

    const char* jsonString = (char *)GetAppSpawnMsgExtInfo(message, "devicedebug", &len);
    if (jsonString == NULL || len == 0) {
        APPSPAWN_LOGE("appspawn devicedebug get devicedebug fail");
        return -1;
    }

    cJSON *json = cJSON_Parse(jsonString);
    APPSPAWN_CHECK(json != NULL, return -1, "appspawn devicedebug json parse fail");

    cJSON *app = cJSON_GetObjectItem(json, "app");
    APPSPAWN_CHECK(cJSON_IsNumber(app), cJSON_Delete(json);
        return -1, "appspawn devicedebug json get app fail");

    cJSON *op = cJSON_GetObjectItem(json, "op");
    APPSPAWN_CHECK(cJSON_IsString(op) && op->valuestring != NULL, cJSON_Delete(json);
        return -1, "appspawn devicedebug json get op fail");

    cJSON *args = cJSON_GetObjectItem(json, "args");
    APPSPAWN_CHECK(cJSON_IsObject(args), cJSON_Delete(json);
        return -1, "appspawn devicedebug json get args fail");

    int result = AppspawnDevicedebugDeal(op->valuestring, app->valueint, args);
    cJSON_Delete(json);
    return result;
}

static void ProcessAppSpawnLockStatusMsg(AppSpawnMsgNode *message)
{
    APPSPAWN_CHECK_ONLY_EXPER(message != NULL, return);
    uint32_t len = 0;
    char *lockstatus = (char *)GetAppSpawnMsgExtInfo(message, "lockstatus", &len);
    APPSPAWN_CHECK(lockstatus != NULL, return, "failed to get lockstatus");
    APPSPAWN_LOGI("appspawn get lockstatus %{public}s from storage_manager", lockstatus);
    char *userLockStatus = NULL;
    // userLockStatus format example:  100:0  100 for userid 0:unlock 1:lock
    char *userIdStr = strtok_r(lockstatus, ":", &userLockStatus);
    APPSPAWN_CHECK(userIdStr != NULL && userLockStatus != NULL, return,
        "lockstatus not satisfied format, failed to get userLockStatus");
    int userId = atoi(userIdStr);
    if (userId < USER_ID_MIN_VALUE || userId > USER_ID_MAX_VALUE) {
        APPSPAWN_LOGE("userId err %{public}s", userIdStr);
        return;
    }
    if (strcmp(userLockStatus, "0") != 0 && strcmp(userLockStatus, "1") != 0) {
        APPSPAWN_LOGE("userLockStatus err %{public}s", userLockStatus);
        return;
    }
    char lockStatusParam[LOCK_STATUS_PARAM_SIZE] = {0};
    int ret = snprintf_s(lockStatusParam, sizeof(lockStatusParam), sizeof(lockStatusParam) - 1,
        "startup.appspawn.lockstatus_%d", userId);
    APPSPAWN_CHECK(ret > 0, return, "get lock status param failed, errno %{public}d", errno);
    ret = SetParameter(lockStatusParam, userLockStatus);
    APPSPAWN_CHECK(ret == 0, return, "failed to set lockstatus param value ret %{public}d", ret);
#ifdef APPSPAWN_HISYSEVENT
    ReportKeyEvent(strcmp(userLockStatus, "0") == 0 ? UNLOCK_SUCCESS : LOCK_SUCCESS);
#endif
    if (strcmp(userLockStatus, "0") == 0) {
        ServerStageHookExecute(STAGE_SERVER_LOCK, GetAppSpawnContent());
    }
}

APPSPAWN_STATIC int AppSpawnReqMsgFdGet(AppSpawnConnection *connection, AppSpawnMsgNode *message,
    const char *fdName, int *fd)
{
    APPSPAWN_CHECK_ONLY_EXPER(message != NULL && message->buffer != NULL && connection != NULL, return -1);
    APPSPAWN_CHECK_ONLY_EXPER(message->tlvOffset != NULL, return -1);
    int findFdIndex = 0;
    AppSpawnMsgReceiverCtx recvCtx = connection->receiverCtx;
    APPSPAWN_CHECK(recvCtx.fds != NULL && recvCtx.fdCount > 0, return 0,
        "no need get fd info %{public}d, %{public}d", recvCtx.fds != NULL, recvCtx.fdCount);

    for (uint32_t index = TLV_MAX; index < (TLV_MAX + message->tlvCount); index++) {
        if (message->tlvOffset[index] == INVALID_OFFSET) {
            return APPSPAWN_SYSTEM_ERROR;
        }
        uint8_t *data = message->buffer + message->tlvOffset[index];
        if (((AppSpawnTlv *)data)->tlvType != TLV_MAX) {
            continue;
        }
        AppSpawnTlvExt *tlv = (AppSpawnTlvExt *)data;
        if (strcmp(tlv->tlvName, MSG_EXT_NAME_APP_FD) != 0) {
            continue;
        }
        APPSPAWN_CHECK(findFdIndex < recvCtx.fdCount && recvCtx.fds[findFdIndex] > 0, return -1,
            "check get fd args failed %{public}d, %{public}d, %{public}d",
            findFdIndex, recvCtx.fdCount, recvCtx.fds[findFdIndex]);

        if (strcmp((const char *)(data + sizeof(AppSpawnTlvExt)), fdName) == 0 && recvCtx.fds[findFdIndex] > 0) {
            *fd = recvCtx.fds[findFdIndex];
            APPSPAWN_LOGI("Spawn Listen fd %{public}s get success %{public}d", fdName, recvCtx.fds[findFdIndex]);
            break;
        }
        findFdIndex++;
        if (findFdIndex >= recvCtx.fdCount) {
            break;
        }
    }
    return 0;
}

APPSPAWN_STATIC void ProcessObserveProcessSignalMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    APPSPAWN_CHECK_ONLY_EXPER(message != NULL, return);
    int fd = 0;
    int ret = AppSpawnReqMsgFdGet(connection, message, SPAWN_LISTEN_FD_NAME, &fd);
    if (fd <= 0 || ret != 0) {
        APPSPAWN_LOGE("Spawn Listen appspawn signal fd get unsuccess");
        SendResponse(connection, &message->msgHeader, APPSPAWN_SYSTEM_ERROR, 0);
        DeleteAppSpawnMsg(&message);
        return;
    }

    AppSpawnContent *content = GetAppSpawnContent();
    APPSPAWN_CHECK(content != NULL, return, "Spawn Listen appspawn content is null");
    content->signalFd = fd;
    connection->receiverCtx.fdCount = 0;
    SendResponse(connection, &message->msgHeader, 0, 0);
    DeleteAppSpawnMsg(&message);
}

static void ProcessRecvMsg(AppSpawnConnection *connection, AppSpawnMsgNode *message)
{
    AppSpawnMsg *msg = &message->msgHeader;
    APPSPAWN_LOGI("Recv message header magic 0x%{public}x type %{public}u id %{public}u len %{public}u %{public}s",
        msg->magic, msg->msgType, msg->msgId, msg->msgLen, msg->processName);
    APPSPAWN_CHECK_ONLY_LOG(connection->receiverCtx.nextMsgId == msg->msgId,
        "Invalid msg id %{public}u %{public}u", connection->receiverCtx.nextMsgId, msg->msgId);
    connection->receiverCtx.nextMsgId++;

    int ret;
    switch (msg->msgType) {
        case MSG_GET_RENDER_TERMINATION_STATUS: {  // get status
            AppSpawnResult result = {0};
            ret = ProcessTerminationStatusMsg(message, &result);
            SendResponse(connection, msg, ret == 0 ? result.result : ret, result.pid);
            DeleteAppSpawnMsg(&message);
            break;
        }
        case MSG_SPAWN_NATIVE_PROCESS:  // spawn msg
        case MSG_APP_SPAWN: {
            ProcessSpawnReqMsg(connection, message);
            break;
        }
        case MSG_DUMP:
            ProcessAppSpawnDumpMsg(message);
            SendResponse(connection, msg, 0, 0);
            DeleteAppSpawnMsg(&message);
            break;
        case MSG_BEGET_CMD: {
            ProcessBegetCmdMsg(connection, message);
            break;
        }
        case MSG_BEGET_SPAWNTIME:
            SendResponse(connection, msg, GetAppSpawnMgr()->spawnTime.minAppspawnTime,
                         GetAppSpawnMgr()->spawnTime.maxAppspawnTime);
            DeleteAppSpawnMsg(&message);
            break;
        case MSG_UPDATE_MOUNT_POINTS:
            ret = ProcessSpawnRemountMsg(connection, message);
            SendResponse(connection, msg, ret, 0);
            break;
        case MSG_RESTART_SPAWNER:
            ProcessSpawnRestartMsg(connection, message);
            break;
        case MSG_DEVICE_DEBUG:
            ret = ProcessAppSpawnDeviceDebugMsg(message);
            SendResponse(connection, msg, ret, 0);
            DeleteAppSpawnMsg(&message);
            break;
        case MSG_UNINSTALL_DEBUG_HAP:
            ProcessUninstallDebugHap(connection, message);
            break;
        case MSG_LOCK_STATUS:
            ProcessAppSpawnLockStatusMsg(message);
            SendResponse(connection, msg, 0, 0);
            DeleteAppSpawnMsg(&message);
            break;
        case MSG_OBSERVE_PROCESS_SIGNAL_STATUS:
            ProcessObserveProcessSignalMsg(connection, message);
            break;
        default:
            SendResponse(connection, msg, APPSPAWN_MSG_INVALID, 0);
            DeleteAppSpawnMsg(&message);
            break;
    }
}
