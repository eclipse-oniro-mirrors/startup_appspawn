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

#ifndef APPSPAWN_APPUID_MOCK_H
#define APPSPAWN_APPUID_MOCK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dedicated mock for the appUid unit tests: production sources are compiled
 * with function-name macros (ioctl=AppUidIoctlStub, open=AppUidOpenStub, ...)
 * so every libc call is routed here, recorded and answered with configurable
 * results. Keep the ioctl numbers in sync with appspawn_custom_config.cpp:
 *   ACCESS_TOKENID_SET_USERID = _IOW('A', 12, uint32_t)
 *   ACCESS_TOKENID_SET_APPUID = _IOW('A', 31, uint32_t)
 *   ACCESS_TOKENID_GET_APPUID = _IOR('A', 32, uint32_t)
 */
#define APPUID_IOCTL_BASE 'A'
#define APPUID_HM_SET_USERID 12
#define APPUID_HM_SET_APPUID 31
#define APPUID_HM_GET_APPUID 32

#define APPUID_MOCK_MAX_IOCTL_CALLS 16
#define APPUID_MOCK_MAX_PATH_CALLS 16
#define APPUID_MOCK_PATH_MAX 256

/* arm32 _IOC command layout shifts (same encoding as the kernel _IOC_* macros). */
#define APPUID_IOC_TYPE_SHIFT 8
#define APPUID_IOC_SIZE_SHIFT 16
#define APPUID_IOC_DIR_SHIFT 30

/* Encode _IOW/_IOR commands manually (nr | type<<8 | size<<16 | dir<<30). */
static inline unsigned long AppUidMockEncodeIoctlCmd(int isWrite, unsigned int nr)
{
    unsigned long dir = isWrite ? 1UL : 2UL;  // _IOC_WRITE=1, _IOC_READ=2
    return (dir << APPUID_IOC_DIR_SHIFT) | ((unsigned long)sizeof(uint32_t) << APPUID_IOC_SIZE_SHIFT) |
           ((unsigned long)APPUID_IOCTL_BASE << APPUID_IOC_TYPE_SHIFT) | (unsigned long)nr;
}

/* Recorded ioctl call (only uint32_t payload ioctls are recorded). */
typedef struct {
    unsigned long cmd;
    uint32_t value;
} AppUidIoctlCall;

/* Recorded prctl call. */
typedef struct {
    int option;
    unsigned long arg2;
} AppUidPrctlCall;

/* Recorded chown call. */
typedef struct {
    char path[APPUID_MOCK_PATH_MAX];
    uint32_t owner;
    uint32_t group;
} AppUidChownCall;

/* Recorded setresuid call. */
typedef struct {
    uint32_t ruid;
    uint32_t euid;
    uint32_t suid;
} AppUidSetresuidCall;

/* Recorded open call (path only). */
typedef struct {
    char path[APPUID_MOCK_PATH_MAX];
} AppUidOpenCall;

/* Recorded mount call (target and data options string). */
typedef struct {
    char target[APPUID_MOCK_PATH_MAX];
    char data[512];
} AppUidMountCall;

/* Reset all records and restore default (success) stub results. */
void AppUidMockReset(void);

typedef struct {
    int count;
    int pid;
    int appUid;
    int status;
} AppUidEventCall;

void AppUidMockRecordEvent(int pid, int appUid, int status);
AppUidEventCall AppUidMockGetEvent(void);
int AppUidMockGetKillUid(void);
int AppUidMockKillCallCount(void);
int AppUidMockFindUmountPath(const char *path);
void AppUidMockSetLockStatus(const char *status);
int AppUidGetParameterStub(const char *key, const char *def, char *value, uint32_t len);

/* --- configurable stub results --- */
void AppUidMockSetIoctlRet(int ret);
void AppUidMockSetOpenRet(int fd);
void AppUidMockSetPrctlRet(int ret);
void AppUidMockSetChownRet(int ret);
void AppUidMockSetMountRet(int ret);

/* Value returned by ACCESS_TOKENID_GET_APPUID when recorded via AppUidMockSetGetAppUidValue */
void AppUidMockSetGetAppUidValue(uint32_t value);

/* --- recorded call queries --- */
/* Number of recorded ioctl calls. */
int AppUidMockIoctlCallCount(void);
int AppUidMockRawIoctlCallCount(void);
/* Get recorded ioctl call by index; returns -1 when index out of range. */
int AppUidMockGetIoctlCall(int index, AppUidIoctlCall *out);
/* Find the first recorded ioctl call with the given cmd; 1 found, 0 not found. */
int AppUidMockFindIoctlCall(unsigned long cmd, AppUidIoctlCall *out);
/* Find the first recorded open() call whose path contains subStr; 1 found. */
int AppUidMockFindOpenPath(const char *subStr);

/* Recorded setresuid calls. */
int AppUidMockSetresuidCallCount(void);
int AppUidMockGetSetresuidCall(int index, AppUidSetresuidCall *out);

/* Recorded prctl calls. */
int AppUidMockPrctlCallCount(void);
int AppUidMockGetPrctlCall(int index, AppUidPrctlCall *out);

/* Recorded chown calls. */
int AppUidMockChownCallCount(void);
int AppUidMockGetChownCall(int index, AppUidChownCall *out);

/* Recorded mount calls. */
int AppUidMockMountCallCount(void);
int AppUidMockGetMountCall(int index, AppUidMountCall *out);
/* Find the first mount call whose data options contain subStr; 1 found. */
int AppUidMockFindMountData(const char *subStr);

#ifdef __cplusplus
}
#endif

#endif // APPSPAWN_APPUID_MOCK_H
