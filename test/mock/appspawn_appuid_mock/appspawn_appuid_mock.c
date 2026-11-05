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

#undef open
#undef GetParameter
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/prctl.h>
#include "parameter.h"
#include "appspawn_appuid_mock.h"

#include <stdarg.h>
#include <string.h>
#include <sys/types.h>

#include "securec.h"
#ifdef WITH_SECCOMP
#include "seccomp_policy.h"
#endif

/* Private stubs capture production calls; open uses a disposable /dev/null fd. */
#ifdef WITH_SECCOMP
/* Isolate AppUid unit tests from seccomp policy loading. */
bool SetSeccompPolicyWithName(SeccompFilterType filter, const char *filterName)
{
    (void)filter;
    (void)filterName;
    return true;
}
#endif

static AppUidIoctlCall g_ioctlCalls[APPUID_MOCK_MAX_IOCTL_CALLS];
static int g_ioctlCallCount = 0;
static int g_rawIoctlCallCount = 0;
static int g_ioctlRet = 0;
static uint32_t g_getAppUidValue = 0;

static AppUidOpenCall g_openCalls[APPUID_MOCK_MAX_PATH_CALLS];
static int g_openCallCount = 0;
static int g_openRet = 0;

static AppUidPrctlCall g_prctlCalls[APPUID_MOCK_MAX_PATH_CALLS];
static int g_prctlCallCount = 0;
static int g_prctlRet = 0;

static AppUidChownCall g_chownCalls[APPUID_MOCK_MAX_PATH_CALLS];
static int g_chownCallCount = 0;
static int g_chownRet = 0;

static AppUidSetresuidCall g_setresuidCalls[APPUID_MOCK_MAX_PATH_CALLS];
static int g_setresuidCallCount = 0;

static int g_mountRet = 0;

static AppUidMountCall g_mountCalls[APPUID_MOCK_MAX_PATH_CALLS];
static int g_mountCallCount = 0;

static AppUidEventCall g_eventCall;
static int g_killUid;
static int g_killCallCount;
static char g_umountPath[APPUID_MOCK_PATH_MAX];
static char g_lockStatus[2] = "1";

/* Only the fixed prefix up to uid is read from the sysload ioctl payload. */
typedef struct {
    unsigned int magic;
    pid_t pid;
    int id;
    int adj;
    bool processed;
    bool foreground;
    pid_t eventPid;
    int uid;
} AppUidKillInfoPrefix;

#define APPUID_KILL_IOCTL_NR 7U
#define APPUID_KILL_IOCTL_BASE 'S'

void AppUidMockRecordEvent(int pid, int appUid, int status)
{
    g_eventCall.count++;
    g_eventCall.pid = pid;
    g_eventCall.appUid = appUid;
    g_eventCall.status = status;
}

AppUidEventCall AppUidMockGetEvent(void)
{
    return g_eventCall;
}

int AppUidMockGetKillUid(void)
{
    return g_killUid;
}

int AppUidMockKillCallCount(void)
{
    return g_killCallCount;
}

int AppUidMockFindUmountPath(const char *path)
{
    return path != NULL && strcmp(g_umountPath, path) == 0;
}

void AppUidMockSetLockStatus(const char *status)
{
    (void)strcpy_s(g_lockStatus, sizeof(g_lockStatus), status);
}

int AppUidGetParameterStub(const char *key, const char *def, char *value, uint32_t len)
{
    if (strcmp(key, "startup.appspawn.lockstatus_100") == 0) {
        return strcpy_s(value, len, g_lockStatus) == 0 ? 1 : -1;
    }
    return GetParameter(key, def, value, len);
}

/* Ioctl command encoding, mirrors kernel _IOW/_IOR for uint32_t payloads. */
static unsigned long AppUidMockEncodeIoctl(int isWrite, unsigned int nr)
{
    // _IOC(dir,type,nr,size), arm32 shifts: nr | type<<8 | size<<16 | dir<<30
    unsigned int dir = isWrite ? 1U : 2U;  // _IOC_WRITE=1, _IOC_READ=2
    return ((dir << APPUID_IOC_DIR_SHIFT) | ((unsigned int)sizeof(uint32_t) << APPUID_IOC_SIZE_SHIFT) |
        ((unsigned int)APPUID_IOCTL_BASE << APPUID_IOC_TYPE_SHIFT) | nr);
}

void AppUidMockReset(void)
{
    (void)memset_s(&g_eventCall, sizeof(g_eventCall), 0, sizeof(g_eventCall));
    g_killUid = 0;
    g_killCallCount = 0;
    g_umountPath[0] = '\0';
    (void)strcpy_s(g_lockStatus, sizeof(g_lockStatus), "1");
    (void)memset_s(g_ioctlCalls, sizeof(g_ioctlCalls), 0, sizeof(g_ioctlCalls));
    g_ioctlCallCount = 0;
    g_rawIoctlCallCount = 0;
    g_ioctlRet = 0;
    g_getAppUidValue = 0;
    (void)memset_s(g_openCalls, sizeof(g_openCalls), 0, sizeof(g_openCalls));
    g_openCallCount = 0;
    g_openRet = 0;
    (void)memset_s(g_prctlCalls, sizeof(g_prctlCalls), 0, sizeof(g_prctlCalls));
    g_prctlCallCount = 0;
    g_prctlRet = 0;
    (void)memset_s(g_chownCalls, sizeof(g_chownCalls), 0, sizeof(g_chownCalls));
    g_chownCallCount = 0;
    g_chownRet = 0;
    (void)memset_s(g_setresuidCalls, sizeof(g_setresuidCalls), 0, sizeof(g_setresuidCalls));
    g_setresuidCallCount = 0;
    g_mountRet = 0;
    (void)memset_s(g_mountCalls, sizeof(g_mountCalls), 0, sizeof(g_mountCalls));
    g_mountCallCount = 0;
}

void AppUidMockSetIoctlRet(int ret)
{
    g_ioctlRet = ret;
}

void AppUidMockSetOpenRet(int fd)
{
    g_openRet = fd;
}

void AppUidMockSetPrctlRet(int ret)
{
    g_prctlRet = ret;
}

void AppUidMockSetChownRet(int ret)
{
    g_chownRet = ret;
}

void AppUidMockSetMountRet(int ret)
{
    g_mountRet = ret;
}

void AppUidMockSetGetAppUidValue(uint32_t value)
{
    g_getAppUidValue = value;
}

int AppUidMockIoctlCallCount(void)
{
    return g_ioctlCallCount;
}

int AppUidMockGetIoctlCall(int index, AppUidIoctlCall *out)
{
    if (out == NULL || index < 0 || index >= g_ioctlCallCount) {
        return -1;
    }
    *out = g_ioctlCalls[index];
    return 0;
}

int AppUidMockFindIoctlCall(unsigned long cmd, AppUidIoctlCall *out)
{
    for (int i = 0; i < g_ioctlCallCount; i++) {
        if (g_ioctlCalls[i].cmd == cmd) {
            if (out != NULL) {
                *out = g_ioctlCalls[i];
            }
            return 1;
        }
    }
    return 0;
}

int AppUidMockFindOpenPath(const char *subStr)
{
    for (int i = 0; i < g_openCallCount; i++) {
        if (strstr(g_openCalls[i].path, subStr) != NULL) {
            return 1;
        }
    }
    return 0;
}

int AppUidMockSetresuidCallCount(void)
{
    return g_setresuidCallCount;
}

int AppUidMockGetSetresuidCall(int index, AppUidSetresuidCall *out)
{
    if (out == NULL || index < 0 || index >= g_setresuidCallCount) {
        return -1;
    }
    *out = g_setresuidCalls[index];
    return 0;
}

int AppUidMockPrctlCallCount(void)
{
    return g_prctlCallCount;
}

int AppUidMockGetPrctlCall(int index, AppUidPrctlCall *out)
{
    if (out == NULL || index < 0 || index >= g_prctlCallCount) {
        return -1;
    }
    *out = g_prctlCalls[index];
    return 0;
}

int AppUidMockChownCallCount(void)
{
    return g_chownCallCount;
}

int AppUidMockGetChownCall(int index, AppUidChownCall *out)
{
    if (out == NULL || index < 0 || index >= g_chownCallCount) {
        return -1;
    }
    *out = g_chownCalls[index];
    return 0;
}

/* ------------------ libc stubs ------------------ */

int AppUidIoctlStub(int fd, unsigned long request, ...)
{
    g_rawIoctlCallCount++;
    (void)fd;
    const unsigned long killCmd = (3UL << APPUID_IOC_DIR_SHIFT) |
        (sizeof(int32_t) << APPUID_IOC_SIZE_SHIFT) |
        ((unsigned long)APPUID_KILL_IOCTL_BASE << APPUID_IOC_TYPE_SHIFT) | APPUID_KILL_IOCTL_NR;
    if (request == killCmd) {
        va_list args;
        va_start(args, request);
        const void *payload = va_arg(args, const void *);
        va_end(args);
        AppUidKillInfoPrefix prefix = {0};
        if (payload != NULL && memcpy_s(&prefix, sizeof(prefix), payload, sizeof(prefix)) == 0) {
            g_killUid = prefix.uid;
            g_killCallCount++;
        }
    }
    // Record uint32_t payload ioctls on the access_token_id device namespace.
    unsigned long setAppUid = AppUidMockEncodeIoctl(1, APPUID_HM_SET_APPUID);
    unsigned long getAppUid = AppUidMockEncodeIoctl(0, APPUID_HM_GET_APPUID);
    unsigned long setUserId = AppUidMockEncodeIoctl(1, APPUID_HM_SET_USERID);
    if (request == setAppUid || request == setUserId || request == getAppUid) {
        va_list args;
        va_start(args, request);
        uint32_t *value = va_arg(args, uint32_t *);
        va_end(args);
        if (g_ioctlCallCount < APPUID_MOCK_MAX_IOCTL_CALLS && value != NULL) {
            if (request == getAppUid) {
                // read-back ioctl: fill the value the test configured
                *value = g_getAppUidValue;
            }
            g_ioctlCalls[g_ioctlCallCount].cmd = request;
            g_ioctlCalls[g_ioctlCallCount].value = *value;
            g_ioctlCallCount++;
        }
    }
    return g_ioctlRet;
}

int AppUidOpenStub(const char *pathname, int flags, ...)
{
    (void)flags;
    if (g_openCallCount < APPUID_MOCK_MAX_PATH_CALLS && pathname != NULL) {
        int ret = strncpy_s(g_openCalls[g_openCallCount].path, APPUID_MOCK_PATH_MAX,
                            pathname, strlen(pathname));
        if (ret == 0) {
            g_openCallCount++;
        }
    }
    // A real disposable fd keeps production close/write calls away from arbitrary descriptors.
    return g_openRet < 0 ? g_openRet : open("/dev/null", O_RDWR | O_CLOEXEC);
}

int AppUidPrctlStub(int option, ...)
{
    va_list args;
    va_start(args, option);
    unsigned long arg2 = 0;
    if (option == PR_SET_KEEPCAPS) {
        arg2 = (unsigned long)va_arg(args, int);
    }
    va_end(args);
    if (g_prctlCallCount < APPUID_MOCK_MAX_PATH_CALLS) {
        g_prctlCalls[g_prctlCallCount].option = option;
        g_prctlCalls[g_prctlCallCount].arg2 = arg2;
        g_prctlCallCount++;
    }
    return g_prctlRet;
}

int AppUidChownStub(const char *pathname, uint32_t owner, uint32_t group)
{
    if (g_chownCallCount < APPUID_MOCK_MAX_PATH_CALLS && pathname != NULL) {
        int ret = strncpy_s(g_chownCalls[g_chownCallCount].path, APPUID_MOCK_PATH_MAX,
                            pathname, strlen(pathname));
        if (ret == 0) {
            g_chownCalls[g_chownCallCount].owner = owner;
            g_chownCalls[g_chownCallCount].group = group;
            g_chownCallCount++;
        }
    }
    return g_chownRet;
}

int AppUidSetresuidStub(uid_t ruid, uid_t euid, uid_t suid)
{
    if (g_setresuidCallCount < APPUID_MOCK_MAX_PATH_CALLS) {
        g_setresuidCalls[g_setresuidCallCount].ruid = ruid;
        g_setresuidCalls[g_setresuidCallCount].euid = euid;
        g_setresuidCalls[g_setresuidCallCount].suid = suid;
        g_setresuidCallCount++;
    }
    return 0;
}

int AppUidSetresgidStub(uid_t rgid, uid_t egid, uid_t sgid)
{
    (void)rgid;
    (void)egid;
    (void)sgid;
    return 0;
}

int AppUidSetgroupsStub(size_t size, const gid_t *list)
{
    (void)size;
    (void)list;
    return 0;
}

int AppUidMountStub(const char *source, const char *target, const char *filesystemtype,
    unsigned long mountflags, const void *data)
{
    (void)source;
    (void)filesystemtype;
    (void)mountflags;
    if (g_mountCallCount < APPUID_MOCK_MAX_PATH_CALLS && target != NULL) {
        int ret = strncpy_s(g_mountCalls[g_mountCallCount].target, APPUID_MOCK_PATH_MAX,
                            target, strlen(target));
        if (ret == 0 && data != NULL) {
            ret = strncpy_s(g_mountCalls[g_mountCallCount].data,
                sizeof(g_mountCalls[g_mountCallCount].data), (const char *)data,
                strlen((const char *)data));
        }
        if (ret == 0) {
            g_mountCallCount++;
        }
    }
    return g_mountRet;
}

int AppUidMockMountCallCount(void)
{
    return g_mountCallCount;
}

int AppUidMockGetMountCall(int index, AppUidMountCall *out)
{
    if (out == NULL || index < 0 || index >= g_mountCallCount) {
        return -1;
    }
    *out = g_mountCalls[index];
    return 0;
}

int AppUidMockFindMountData(const char *subStr)
{
    for (int i = 0; i < g_mountCallCount; i++) {
        if (strstr(g_mountCalls[i].data, subStr) != NULL) {
            return 1;
        }
    }
    return 0;
}

int AppUidUmount2Stub(const char *target, int flags)
{
    (void)flags;
    (void)strcpy_s(g_umountPath, sizeof(g_umountPath), target);
    return 0;
}

int AppUidMockRawIoctlCallCount(void)
{
    return g_rawIoctlCallCount;
}
