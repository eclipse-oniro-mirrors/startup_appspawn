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

#include "subspawn_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "appspawn_error.h"
#include "appspawn_msg.h"
#include "appspawn_utils.h"
#include "securec.h"

// subspawn start message, from SubspawnEarlyParse argv
static SubspawnStartInfo g_startInfo = {.pipeFd = -1, .memFd = -1, .listenFd = -1};
static int g_startResult = APPSPAWN_SUBSPAWN_PRESET_FAILED;

static int ParseNumber(const char **text, uint32_t *number, bool last)
{
    char *end = NULL;
    if (**text < '0' || **text > '9') {
        return -1;
    }
    errno = 0;
    unsigned long value = strtoul(*text, &end, 10); // decimal descriptor/length/uid arguments
    if (errno != 0 || value > UINT32_MAX || (last ? *end != '\0' : *end != ':')) {
        return -1;
    }
    *number = (uint32_t)value;
    *text = last ? end : end + 1;
    return 0;
}

static int CheckStartArgumentValues(const uint32_t *values)
{
    if (values[0] > INT_MAX || values[1] > INT_MAX || values[2] > INT_MAX) {
        APPSPAWN_LOGE("Subspawn fd argument out of range: pipe %{public}u mem %{public}u listen %{public}u",
            values[0], values[1], values[2]);
        return -1;
    }
    if (values[0] == values[1] || values[0] == values[2] || values[1] == values[2]) {
        APPSPAWN_LOGE("Subspawn fd argument duplicated: pipe %{public}u mem %{public}u listen %{public}u",
            values[0], values[1], values[2]);
        return -1;
    }
    if (values[3] < sizeof(AppSpawnMsg) || values[3] >= MAX_MSG_TOTAL_LENGTH) {
        APPSPAWN_LOGE("Invalid subspawn message length %{public}u", values[3]);
        return -1;
    }
    if (values[4] == 0 || values[4] != (uint32_t)getuid()) {
        APPSPAWN_LOGE("Subspawn uid %{public}u does not match current uid %{public}u",
            values[4], (uint32_t)getuid());
        return -1;
    }
    return 0;
}

static int CheckStartPipeFd(int fd)
{
    struct stat readyStat;
    if (fcntl(fd, F_GETFD) < 0) {
        APPSPAWN_LOGE("Failed to get subspawn pipe fd %{public}d flags errno %{public}d", fd, errno);
        return -1;
    }
    if (fstat(fd, &readyStat) != 0) {
        APPSPAWN_LOGE("Failed to stat subspawn pipe fd %{public}d errno %{public}d", fd, errno);
        return -1;
    }
    if (!S_ISFIFO(readyStat.st_mode)) {
        APPSPAWN_LOGE("Subspawn pipe fd %{public}d is not a FIFO", fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        APPSPAWN_LOGE("Failed to get subspawn pipe fd %{public}d status flags errno %{public}d", fd, errno);
        return -1;
    }
    if ((flags & O_ACCMODE) != O_WRONLY) {
        APPSPAWN_LOGE("Subspawn pipe fd %{public}d is not write-only flags 0x%{public}x", fd, flags);
        return -1;
    }
    return 0;
}

static int CheckStartMessageFd(int fd, uint32_t expectedLength)
{
    struct stat messageStat;
    if (fstat(fd, &messageStat) != 0) {
        APPSPAWN_LOGE("Failed to stat subspawn message fd %{public}d errno %{public}d", fd, errno);
        return -1;
    }
    if (messageStat.st_size != (off_t)expectedLength) {
        APPSPAWN_LOGE("Subspawn message fd %{public}d size %{public}ld expected %{public}u",
            fd, (long)messageStat.st_size, expectedLength);
        return -1;
    }
    return 0;
}

static int CheckStartFds(const uint32_t *values)
{
    const int requiredSeals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    int seals = fcntl((int)values[1], F_GET_SEALS);
    if (CheckStartPipeFd((int)values[0]) != 0) {
        return -1;
    }
    if (seals < 0) {
        APPSPAWN_LOGE("Failed to get subspawn message fd %{public}u seals errno %{public}d", values[1], errno);
        return -1;
    }
    if ((seals & requiredSeals) != requiredSeals) {
        APPSPAWN_LOGE("Subspawn message fd %{public}u seals 0x%{public}x missing 0x%{public}x",
            values[1], (uint32_t)seals, (uint32_t)(requiredSeals & ~seals));
        return -1;
    }
    return CheckStartMessageFd((int)values[1], values[3]);
}

static int CheckStartListenFd(int fd)
{
    struct sockaddr_un address = {0};
    socklen_t addressLength = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &addressLength) != 0) {
        APPSPAWN_LOGE("Failed to get subspawn listen fd %{public}d socket name errno %{public}d", fd, errno);
        return -1;
    }
    if (address.sun_family != AF_UNIX) {
        APPSPAWN_LOGE("Subspawn listen fd %{public}d has invalid family %{public}d",
            fd, (int)address.sun_family);
        return -1;
    }
    if (memchr(address.sun_path, '\0', sizeof(address.sun_path)) == NULL) {
        APPSPAWN_LOGE("Subspawn listen fd %{public}d socket name is not terminated", fd);
        return -1;
    }
    if (strcmp(address.sun_path, g_startInfo.socketName) != 0) {
        APPSPAWN_LOGE("Subspawn listen fd %{public}d socket name %{public}s expected %{public}s",
            fd, address.sun_path, g_startInfo.socketName);
        return -1;
    }
    return 0;
}

int SubspawnEarlyParse(int argc, char *const argv[])
{
    if (argc < 5 || strcmp(argv[3], "-subspawn-fds") != 0) {
        APPSPAWN_LOGE("Invalid subspawn arguments argc %{public}d", argc);
        return -1;
    }
    uint32_t values[5] = {0}; // pipeFd, memFd, listenFd, message length, application uid
    const char *arg = argv[4];
    for (size_t i = 0; i < ARRAY_LENGTH(values); ++i) {
        if (ParseNumber(&arg, &values[i], i == ARRAY_LENGTH(values) - 1) != 0) {
            APPSPAWN_LOGE("Failed to parse subspawn fd argument %{public}s at index %{public}zu", argv[4], i);
            return -1;
        }
    }
    if (CheckStartArgumentValues(values) != 0 || CheckStartFds(values) != 0) {
        return -1;
    }
    g_startInfo.pipeFd = (int)values[0];
    g_startInfo.memFd = (int)values[1];
    g_startInfo.listenFd = (int)values[2];
    g_startInfo.messageLen = values[3];
    g_startInfo.uid = values[4];
    if (snprintf_s(g_startInfo.socketName, sizeof(g_startInfo.socketName),
        sizeof(g_startInfo.socketName) - 1, SUBSPAWN_SOCKET_PREFIX "%u", values[4]) <= 0) {
        APPSPAWN_LOGE("Failed to format subspawn socket name for uid %{public}u", values[4]);
        return -1;
    }
    return CheckStartListenFd(g_startInfo.listenFd);
}

int SubspawnSetProcessName(AppSpawnContent *content)
{
    APPSPAWN_CHECK(content != NULL && content->mode == MODE_FOR_SUB_SPAWN && content->longProcName != NULL,
        return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Invalid subspawn content for process name");
    char processName[MAX_LEN_SHORT_NAME] = {0};
    int ret = snprintf_s(processName, sizeof(processName), sizeof(processName) - 1,
        SUBSPAWN_SERVER_NAME "_%u", g_startInfo.uid % UID_BASE);
    APPSPAWN_CHECK(ret > 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Failed to format subspawn process name");
    ret = memset_s(content->longProcName, content->longProcNameLen, 0, content->longProcNameLen);
    APPSPAWN_CHECK(ret == EOK, return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Failed to clear subspawn long process name");
    ret = strncpy_s(content->longProcName, content->longProcNameLen, processName, strlen(processName));
    APPSPAWN_CHECK(ret == EOK, return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Failed to copy subspawn long process name");
    ret = prctl(PR_SET_NAME, processName);
    APPSPAWN_CHECK(ret == 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to set subspawn short process name errno %{public}d", errno);
    return 0;
}

const SubspawnStartInfo *GetSubspawnStartInfo(void)
{
    return &g_startInfo;
}

void SubspawnSetStartResult(int result)
{
    g_startResult = result;
}

int SubspawnGetStartResult(void)
{
    return g_startResult;
}

void SubspawnNotifyReady(int result)
{
    if (g_startInfo.pipeFd < 0) {
        return;
    }
    ssize_t written;
    do {
        written = write(g_startInfo.pipeFd, &result, sizeof(result));
    } while (written < 0 && errno == EINTR);
    int writeErrno = errno; // close() may overwrite errno, save the write failure first
    close(g_startInfo.pipeFd);
    g_startInfo.pipeFd = -1;
    if (written < 0) {
        APPSPAWN_LOGE("Failed to notify subspawn start result %{public}d errno %{public}d", result, writeErrno);
        _exit(EXIT_FAILURE);
    }
    if (written != (ssize_t)sizeof(result)) {
        APPSPAWN_LOGE("Short write notifying subspawn start result %{public}d written %{public}d of %{public}zu",
            result, (int)written, sizeof(result));
        _exit(EXIT_FAILURE);
    }
}
