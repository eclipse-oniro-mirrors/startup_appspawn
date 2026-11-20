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

// _GNU_SOURCE is supplied by the global build configuration.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/capability.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "appspawn_error.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_service.h"
#include "appspawn_utils.h"
#include "child_process_api.h"
#include "parameters.h"
#include "securec.h"
#include "subspawn_manager.h"
#include "subspawn_preload.h"
#include "tokenid_kit.h"
#ifdef APPSPAWN_ENABLE_SPM
#include "token_setproc.h"
#endif
#ifdef WITH_SELINUX
#include "selinux/selinux.h"
#endif

enum {
    SUBSPAWN_CAP_WORDS = 2,
    SUBSPAWN_LISTEN_BACKLOG = 16,
};

static int SubspawnPreload(const AppSpawnMsgNode *message)
{
    uint32_t size = 0;
    char *extInfo = reinterpret_cast<char *>(GetAppSpawnMsgExtInfo(message, MSG_EXT_NAME_SUBSPAWN_PRELOAD, &size));
    if (size == 0 || extInfo == nullptr) {
        return 0;
    }
    const auto *tokenInfo = static_cast<const AppSpawnMsgAccessToken *>(
        GetAppSpawnMsgInfo(message, TLV_ACCESS_TOKEN_INFO));
    APPSPAWN_CHECK(tokenInfo != nullptr, return APPSPAWN_SUBSPAWN_PRELOAD_FAILED,
        "Missing access token for subspawn preload");
    const bool isSystemApp =
        OHOS::Security::AccessToken::TokenIdKit::IsSystemAppByFullTokenID(tokenInfo->accessTokenIdEx);
    cJSON *config = cJSON_Parse(extInfo);
    APPSPAWN_CHECK(config != nullptr, return APPSPAWN_SUBSPAWN_PRELOAD_FAILED, "Invalid subspawn preload JSON");
    int result = SubspawnPreloadLibraries(config, isSystemApp);
    cJSON_Delete(config);
    return result;
}

static int WriteAll(int fd, const void *data, size_t length, const char *part = "message")
{
    const uint8_t *buffer = static_cast<const uint8_t *>(data);
    while (length > 0) {
        ssize_t count = write(fd, buffer, length);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count < 0) {
                APPSPAWN_LOGE("Failed to write subspawn %{public}s fd %{public}d remaining %{public}zu "
                    "errno %{public}d", part, fd, length, errno);
            } else {
                APPSPAWN_LOGE("No progress writing subspawn %{public}s fd %{public}d remaining %{public}zu",
                    part, fd, length);
            }
            return APPSPAWN_SUBSPAWN_PRESET_FAILED;
        }
        length -= static_cast<size_t>(count);
        buffer += count;
    }
    return 0;
}

static int CreateMessageFd(const AppSpawnMsgNode *message)
{
    int fd = memfd_create("subspawn-message", MFD_ALLOW_SEALING);
    APPSPAWN_CHECK(fd >= 0, return -1, "Failed to create subspawn message fd errno %{public}d", errno);
    APPSPAWN_CHECK(message->msgHeader.msgLen >= sizeof(AppSpawnMsg), close(fd); return -1,
        "Invalid message length %{public}u, minimum %{public}zu, fd %{public}d",
        message->msgHeader.msgLen, sizeof(AppSpawnMsg), fd);

    uint32_t length = message->msgHeader.msgLen - sizeof(AppSpawnMsg);
    if (WriteAll(fd, &message->msgHeader, sizeof(AppSpawnMsg), "message header") != 0) {
        close(fd);
        return -1;
    }
    if (length > 0 && WriteAll(fd, message->buffer, length, "message body") != 0) {
        close(fd);
        return -1;
    }
    APPSPAWN_CHECK(lseek(fd, 0, SEEK_SET) >= 0, close(fd); return -1,
        "Failed to rewind subspawn message fd %{public}d errno %{public}d", fd, errno);
    APPSPAWN_CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) >= 0,
        close(fd); return -1, "Failed to seal subspawn message fd %{public}d errno %{public}d", fd, errno);
    return fd;
}

static bool IsKeptFd(long fd, const int *keepFds, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (fd == keepFds[i]) {
            return true;
        }
    }
    return false;
}

static int SetInheritedFdsCloseOnExec(const int *keepFds, size_t count)
{
    DIR *directory = opendir("/proc/self/fd");
    APPSPAWN_CHECK(directory != nullptr, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to open subspawn descriptor directory errno %{public}d", errno);
    int result = 0;
    struct dirent *entry = nullptr;
    do {
        errno = 0;
        entry = readdir(directory);
        if (entry == nullptr) {
            result = errno == 0 ? 0 : APPSPAWN_SUBSPAWN_PRESET_FAILED;
            APPSPAWN_CHECK_ONLY_LOG(result == 0,
                "Failed to read subspawn descriptor directory errno %{public}d", errno);
            break;
        }
        char *end = nullptr;
        long fd = strtol(entry->d_name, &end, 10);
        if (*end == '\0' && fd > STDERR_FILENO && fd != dirfd(directory) &&
            !IsKeptFd(fd, keepFds, count)) {
            // Let exec close inherited FDs without violating FDSAN ownership (for example FILE*).
            int flags = fcntl(static_cast<int>(fd), F_GETFD);
            if (flags < 0) {
                APPSPAWN_LOGE("Failed to get subspawn inherited fd flags fd %{public}ld errno %{public}d",
                    fd, errno);
                result = APPSPAWN_SUBSPAWN_PRESET_FAILED;
                break;
            }
            if (fcntl(static_cast<int>(fd), F_SETFD, flags | FD_CLOEXEC) < 0) {
                APPSPAWN_LOGE("Failed to set subspawn inherited fd close-on-exec fd %{public}ld errno %{public}d",
                    fd, errno);
                result = APPSPAWN_SUBSPAWN_PRESET_FAILED;
                break;
            }
        }
    } while (entry != nullptr);
    if (closedir(directory) != 0) {
        APPSPAWN_LOGE("Failed to close subspawn descriptor directory errno %{public}d", errno);
        result = APPSPAWN_SUBSPAWN_PRESET_FAILED;
    }
    return result;
}

static int GetInstanceSocketName(uid_t uid, char *name, size_t length)
{
    if (snprintf_s(name, length, length - 1, SUBSPAWN_SOCKET_PREFIX "%u", uid) <= 0) {
        APPSPAWN_LOGE("Failed to format subspawn socket name for uid %{public}u length %{public}zu", uid, length);
        return APPSPAWN_SUBSPAWN_PRESET_FAILED;
    }
    return 0;
}

static int CreateInstanceSocket()
{
#ifdef WITH_SELINUX
    // The listener survives exec into subspawn; label the socket for that domain.
    if (setsockcreatecon("u:r:subspawn:s0") != 0) {
        APPSPAWN_LOGE("Failed to set subspawn socket creation context errno %{public}d", errno);
        return -1;
    }
#endif
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
#ifdef WITH_SELINUX
    int socketErrno = errno;
    if (setsockcreatecon(nullptr) != 0) {
        APPSPAWN_LOGE("Failed to reset socket creation context errno %{public}d", errno);
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    errno = socketErrno;
#endif
    APPSPAWN_CHECK_ONLY_LOG(fd >= 0, "Failed to create subspawn socket errno %{public}d", errno);
    return fd;
}

static int BindInstanceSocket(uid_t uid)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (GetInstanceSocketName(uid, address.sun_path, sizeof(address.sun_path)) != 0) {
        return -1;
    }
    int fd = CreateInstanceSocket();
    if (fd < 0) {
        return -1;
    }
    (void)unlink(address.sun_path);
    APPSPAWN_CHECK(bind(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) == 0,
        close(fd);
        return -1, "Failed to bind subspawn socket for uid %{public}u errno %{public}d", uid, errno);
    APPSPAWN_CHECK(chmod(address.sun_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) == 0,
        close(fd);
        return -1, "Failed to chmod subspawn socket for uid %{public}u errno %{public}d", uid, errno);
    APPSPAWN_CHECK(chown(address.sun_path, uid, uid) == 0,
        close(fd);
        return -1, "Failed to chown subspawn socket for uid %{public}u errno %{public}d", uid, errno);
    APPSPAWN_CHECK(listen(fd, SUBSPAWN_LISTEN_BACKLOG) == 0,
        close(fd);
        return -1, "Failed to listen on subspawn socket for uid %{public}u errno %{public}d", uid, errno);
    return fd;
}

static int PrepareInstanceChild(AppSpawnMgr *content, AppSpawningCtx *property)
{
    if (GetAppSpawnMsgType(property) != MSG_SUBSPAWN_CREATE) {
        return 0;
    }
    APPSPAWN_CHECK(content != nullptr && content->content.mode == MODE_FOR_APP_SPAWN,
        return APPSPAWN_SUBSPAWN_INSTANCE_INVALID, "Only appspawn can prepare a subspawn instance");
    // Keep the creator's mode until exec. The new service selects subspawn from argv.
    const AppSpawnMsgDacInfo *dac = static_cast<const AppSpawnMsgDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    APPSPAWN_CHECK(dac != nullptr && property->forkCtx.fd[1] >= 0,
        return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Missing subspawn bootstrap context");
    APPSPAWN_CHECK(MakeDirRec(SUBSPAWN_SOCKET_DIR, 0711, 1) == 0,
        return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Failed to prepare subspawn socket directory");
    property->subspawnFds[SUBSPAWN_MEM_FD_INDEX] = CreateMessageFd(property->message);
    property->subspawnFds[SUBSPAWN_PIPE_FD_INDEX] = property->forkCtx.fd[1];
    property->subspawnFds[SUBSPAWN_LISTEN_FD_INDEX] = BindInstanceSocket(dac->uid);
    for (size_t i = 0; i < ARRAY_LENGTH(property->subspawnFds); ++i) {
        int fd = property->subspawnFds[i];
        APPSPAWN_CHECK(fd >= 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
            "Invalid subspawn descriptor index %{public}zu fd %{public}d", i, fd);
        int flags = fcntl(fd, F_GETFD);
        APPSPAWN_CHECK(flags >= 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
            "Failed to get subspawn fd flags index %{public}zu fd %{public}d errno %{public}d", i, fd, errno);
        APPSPAWN_CHECK(fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
            "Failed to clear subspawn fd close-on-exec index %{public}zu fd %{public}d errno %{public}d", i, fd, errno);
    }
    return 0;
}

static int ExecInstanceChild(AppSpawnMgr *content, AppSpawningCtx *property)
{
    if (!IsSubspawnInstanceCreate(content, property)) {
        return 0;
    }
    const AppSpawnMsgDacInfo *dac = static_cast<const AppSpawnMsgDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
    APPSPAWN_CHECK(dac != nullptr, return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Missing subspawn exec DAC info");
    APPSPAWN_CHECK(getuid() == dac->uid, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn exec uid %{public}u mismatch expected %{public}u", getuid(), dac->uid);
    APPSPAWN_CHECK(geteuid() == dac->uid, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn exec euid %{public}u mismatch expected %{public}u", geteuid(), dac->uid);
    APPSPAWN_CHECK(getgid() == dac->gid, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn exec gid %{public}u mismatch expected %{public}u", getgid(), dac->gid);
    APPSPAWN_CHECK(getegid() == dac->gid, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn exec egid %{public}u mismatch expected %{public}u", getegid(), dac->gid);
    APPSPAWN_CHECK(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to set subspawn parent death signal errno %{public}d", errno);
    APPSPAWN_CHECK(getppid() == content->servicePid, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn exec parent pid %{public}d changed from %{public}d", getppid(), content->servicePid);
    char arguments[128];
    char padding[APP_LEN_PROC_NAME];
    APPSPAWN_CHECK(memset_s(padding, sizeof(padding), ' ', sizeof(padding) - 1) == EOK,
        return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Failed to initialize subspawn argument padding");
    padding[sizeof(padding) - 1] = '\0';
    APPSPAWN_CHECK(snprintf_s(arguments, sizeof(arguments), sizeof(arguments) - 1, "%d:%d:%d:%u:%u",
        property->subspawnFds[SUBSPAWN_PIPE_FD_INDEX], property->subspawnFds[SUBSPAWN_MEM_FD_INDEX],
        property->subspawnFds[SUBSPAWN_LISTEN_FD_INDEX], property->message->msgHeader.msgLen, dac->uid) > 0,
        return APPSPAWN_SUBSPAWN_PRESET_FAILED, "Failed to format subspawn bootstrap arguments");

    int ret = SetInheritedFdsCloseOnExec(property->subspawnFds, ARRAY_LENGTH(property->subspawnFds));
    APPSPAWN_CHECK_ONLY_EXPER(ret == 0, return ret);
    const char *const argv[] = {"/system/bin/subspawn", "-mode", SUBSPAWN_SERVER_NAME,
        "-subspawn-fds", arguments, padding, nullptr};
    execv(argv[0], const_cast<char *const *>(argv));
    APPSPAWN_LOGE("Subspawn exec failed errno %{public}d", errno);
    return APPSPAWN_SUBSPAWN_PRESET_FAILED;
}

static void UnlinkInstanceSocket(uid_t uid)
{
    char name[SUBSPAWN_SOCKET_NAME_LENGTH];
    if (GetInstanceSocketName(uid, name, sizeof(name)) == 0) {
        (void)unlink(name);
    }
}

static int InstanceCreateAborted(AppSpawnMgr *content, AppSpawningCtx *property)
{
    (void)content;
    if (GetAppSpawnMsgType(property) == MSG_SUBSPAWN_CREATE && property->pid > 0) {
        const AppSpawnMsgDacInfo *dac = static_cast<const AppSpawnMsgDacInfo *>(GetAppProperty(property, TLV_DAC_INFO));
        if (dac != nullptr) {
            UnlinkInstanceSocket(dac->uid);
        }
    }
    return 0;
}

static int InstanceDied(const AppSpawnMgr *content, const AppSpawnedProcessInfo *appInfo)
{
    if (!IsSubSpawnMode(content) && appInfo->isSubspawn) {
        UnlinkInstanceSocket(appInfo->uid);
    }
    return 0;
}

static int ValidateInstanceSpawn(AppSpawnMgr *content, AppSpawningCtx *property)
{
    if (!IsSubspawnInstanceCreate(content, property) ||
        strcmp(content->content.longProcName, APPSPAWN_SERVER_NAME) != 0) {
        return APPSPAWN_SUBSPAWN_INSTANCE_INVALID;
    }
    return CheckAppSpawnMsg(property->message) == 0 ? 0 : APPSPAWN_SUBSPAWN_INSTANCE_INVALID;
}

static int LoadInstanceMessage(const SubspawnStartInfo *start, AppSpawnMsgNode **message)
{
    uint8_t *buffer = static_cast<uint8_t *>(malloc(start->messageLen));
    APPSPAWN_CHECK(buffer != nullptr, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to allocate subspawn message buffer length %{public}u", start->messageLen);
    size_t received = 0;
    while (received < start->messageLen) {
        ssize_t count = pread(start->memFd, buffer + received, start->messageLen - received, received);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count < 0) {
                APPSPAWN_LOGE("Failed to read subspawn message fd %{public}d offset %{public}zu errno %{public}d",
                    start->memFd, received, errno);
            } else {
                APPSPAWN_LOGE("Unexpected end of subspawn message at %{public}zu of %{public}u bytes",
                    received, start->messageLen);
            }
            free(buffer);
            buffer = nullptr;
            return APPSPAWN_SUBSPAWN_PRESET_FAILED;
        }
        received += static_cast<size_t>(count);
    }
    uint32_t length = 0;
    uint32_t remainder = 0;
    int result = GetAppSpawnMsgFromBuffer(buffer, start->messageLen, message, &length, &remainder);
    free(buffer);
    APPSPAWN_CHECK(result == 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to parse subspawn message buffer result %{public}d", result);
    APPSPAWN_CHECK(length == start->messageLen, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn parsed message length %{public}u expected %{public}u", length, start->messageLen);
    APPSPAWN_CHECK(remainder == 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn message has trailing bytes %{public}u", remainder);
    APPSPAWN_CHECK(*message != nullptr, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn message parser returned no message");
    int decodeResult = DecodeAppSpawnMsg(*message);
    APPSPAWN_CHECK(decodeResult == 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to decode subspawn message result %{public}d", decodeResult);
    return 0;
}

#ifdef WITH_SELINUX
static int CheckInstanceSelinuxContext()
{
    char *context = nullptr;
    int result = getcon(&context);
    bool valid = false;
    if (result != 0) {
        APPSPAWN_LOGE("Failed to get subspawn security context errno %{public}d", errno);
    } else if (context == nullptr) {
        APPSPAWN_LOGE("Subspawn security context is missing");
    } else {
        valid = strncmp(context, "u:r:subspawn:", strlen("u:r:subspawn:")) == 0;
        APPSPAWN_CHECK_ONLY_LOG(valid, "Subspawn security domain mismatch context %{public}s", context);
    }
    if (context != nullptr) {
        freecon(context);
    }
    return valid ? 0 : APPSPAWN_SUBSPAWN_PRESET_FAILED;
}
#endif

static int CheckInstanceCapabilities()
{
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct capabilities[SUBSPAWN_CAP_WORDS] = {{0}};
    APPSPAWN_CHECK(capget(&header, capabilities) == 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to get subspawn capabilities errno %{public}d", errno);
    for (size_t i = 0; i < SUBSPAWN_CAP_WORDS; i++) {
        uint32_t required = i == 0 ? (1U << CAP_SETUID) | (1U << CAP_SETGID) | (1U << CAP_KILL) : 0;
        APPSPAWN_CHECK(capabilities[i].permitted == required, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
            "Subspawn permitted capabilities word %{public}zu is 0x%{public}x expected 0x%{public}x",
            i, capabilities[i].permitted, required);
        APPSPAWN_CHECK(capabilities[i].effective == required, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
            "Subspawn effective capabilities word %{public}zu is 0x%{public}x expected 0x%{public}x",
            i, capabilities[i].effective, required);
        APPSPAWN_CHECK(capabilities[i].inheritable == required, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
            "Subspawn inheritable capabilities word %{public}zu is 0x%{public}x expected 0x%{public}x",
            i, capabilities[i].inheritable, required);
    }
    return 0;
}

static int CheckInstanceSecurity(const SubspawnStartInfo *start)
{
    APPSPAWN_CHECK(getuid() == start->uid, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn instance uid %{public}u mismatch expected %{public}u", getuid(), start->uid);
    APPSPAWN_CHECK(geteuid() == start->uid, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn instance euid %{public}u mismatch expected %{public}u", geteuid(), start->uid);
    int noNewPrivs = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    APPSPAWN_CHECK(noNewPrivs >= 0, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Failed to get subspawn no-new-privileges flag errno %{public}d", errno);
    APPSPAWN_CHECK(noNewPrivs == 1, return APPSPAWN_SUBSPAWN_PRESET_FAILED,
        "Subspawn no-new-privileges flag is %{public}d, not 1", noNewPrivs);
    if (CheckInstanceCapabilities() != 0) {
        return APPSPAWN_SUBSPAWN_PRESET_FAILED;
    }
#ifdef WITH_SELINUX
    return CheckInstanceSelinuxContext();
#else
    APPSPAWN_LOGE("Subspawn security validation requires SELinux support");
    return APPSPAWN_SUBSPAWN_PRESET_FAILED;
#endif
}

static int BuildFdInfoMap(const AppSpawnMsgNode *message, std::map<std::string, int> &fdMap, int isColdRun)
{
    APPSPAWN_CHECK_ONLY_EXPER(message != nullptr && message->buffer != nullptr, return -1);
    APPSPAWN_CHECK_ONLY_EXPER(message->tlvOffset != nullptr, return -1);
    int findFdIndex = 0;
    AppSpawnMsgReceiverCtx recvCtx;
    if (!isColdRun) {
        APPSPAWN_CHECK_ONLY_EXPER(message->connection != nullptr, return -1);
        recvCtx = message->connection->receiverCtx;
        if (recvCtx.fdCount <= 0) {
            APPSPAWN_LOGI("no need to build fd info %{public}d, %{public}d", recvCtx.fds != NULL, recvCtx.fdCount);
            return 0;
        }
    }
    for (uint32_t index = TLV_MAX; index < (TLV_MAX + message->tlvCount); index++) {
        if (message->tlvOffset[index] == INVALID_OFFSET) {
            return -1;
        }
        uint8_t *data = message->buffer + message->tlvOffset[index];
        if (((AppSpawnTlv *)data)->tlvType != TLV_MAX) {
            continue;
        }
        AppSpawnTlvExt *tlv = (AppSpawnTlvExt *)data;
        if (strcmp(tlv->tlvName, MSG_EXT_NAME_APP_FD) != 0) {
            continue;
        }
        std::string key((char *)data + sizeof(AppSpawnTlvExt));
        if (isColdRun) {
            std::string envKey = std::string(APP_FDENV_PREFIX) + key;
            char *fdChar = getenv(envKey.c_str());
            APPSPAWN_CHECK(fdChar != nullptr, continue, "getfd from env failed %{public}s", envKey.c_str());
            char *endptr = nullptr;
            int numberBase = 10;
            int fd = static_cast<int>(strtol(fdChar, &endptr, numberBase));
            APPSPAWN_CHECK(endptr != fdChar && *endptr == '\0' && fd > 0,
                continue, "getfd from env strol errno %{public}s,%{public}d", envKey.c_str(), fd);
            fdMap[key] = fd;
        } else {
            APPSPAWN_CHECK(findFdIndex < recvCtx.fdCount && recvCtx.fds[findFdIndex] > 0,
                return -1, "invalid fd info  %{public}d %{public}d", findFdIndex, recvCtx.fds[findFdIndex]);
            fdMap[key] = recvCtx.fds[findFdIndex++];
            if (findFdIndex >= recvCtx.fdCount) {
                break;
            }
        }
    }
    return 0;
}

static int RunChildThread(const AppSpawnMgr *content, const AppSpawningCtx *property)
{
    std::string checkExit;
    if (OHOS::system::GetBoolParameter("persist.init.debug.checkexit", true)) {
        checkExit = std::to_string(getpid());
    }
    setenv(APPSPAWN_CHECK_EXIT, checkExit.c_str(), true);
    if (CheckAppMsgFlagsSet(property, APP_FLAGS_CHILDPROCESS)) {
        std::map<std::string, int> fdMap;
        BuildFdInfoMap(property->message, fdMap, IsColdRunMode(content));
        AppSpawnEnvClear((AppSpawnContent *)&content->content, (AppSpawnClient *)&property->client);
        OHOS::AppExecFwk::ChildProcessApi::StartChild(fdMap);
    } else {
        APPSPAWN_LOGW("RunChildThread: Do not support this start method");
    }
    unsetenv(APPSPAWN_CHECK_EXIT);
    return 0;
}

APPSPAWN_STATIC int RunChildProcessor(AppSpawnContent *content, AppSpawnClient *client)
{
    APPSPAWN_CHECK(client != nullptr && content != nullptr, return -1, "Invalid client");
    AppSpawningCtx *property = reinterpret_cast<AppSpawningCtx *>(client);

    return RunChildThread(reinterpret_cast<AppSpawnMgr *>(content), property);
}

static int SubspawnInstanceInit(AppSpawnMgr *content)
{
    if (!IsSubSpawnMode(content)) {
        return 0;
    }
    const SubspawnStartInfo *start = GetSubspawnStartInfo();
    AppSpawnMsgNode *message = nullptr;
    const char *stage = "security check";
    int result = CheckInstanceSecurity(start);
    if (result == 0) {
        stage = "message decode";
        result = LoadInstanceMessage(start, &message);
    }
    close(start->memFd);
#ifdef APPSPAWN_ENABLE_SPM
    if (result == 0) {
        stage = "identity check";
        const AppSpawnMsgAccessToken *token =
            static_cast<const AppSpawnMsgAccessToken *>(GetAppSpawnMsgInfo(message, TLV_ACCESS_TOKEN_INFO));
        const AppDacInfo *dac = static_cast<const AppDacInfo *>(GetAppSpawnMsgInfo(message, TLV_DAC_INFO));
        if (token == nullptr) {
            APPSPAWN_LOGE("Subspawn message is missing the access token info");
            result = APPSPAWN_SUBSPAWN_PRESET_FAILED;
        } else if (dac == nullptr) {
            APPSPAWN_LOGE("Subspawn message is missing the DAC info");
            result = APPSPAWN_SUBSPAWN_PRESET_FAILED;
        } else if (getgid() != dac->gid || getegid() != dac->gid) {
            APPSPAWN_LOGE("Subspawn gid %{public}u egid %{public}u mismatch expected %{public}u",
                getgid(), getegid(), dac->gid);
            result = APPSPAWN_SUBSPAWN_PRESET_FAILED;
        }
    }
#else
    stage = "SPM support";
    result = APPSPAWN_SUBSPAWN_PRESET_FAILED;
#endif
    if (result == 0) {
        stage = "library preload";
        result = SubspawnPreload(message);
    }
    DeleteAppSpawnMsg(&message);
    SubspawnSetStartResult(result);
    if (result != 0) {
        APPSPAWN_LOGE("Failed to initialize subspawn at %{public}s, result: %{public}d", stage, result);
        return result;
    }
    RegChildLooper(&content->content, RunChildProcessor);
    return 0;
}

MODULE_CONSTRUCTOR(void)
{
    AddAppSpawnHook(STAGE_CREATE_SUBSPAWN_PRE_FORK, HOOK_PRIO_HIGHEST - 1, ValidateInstanceSpawn);
    AddAppSpawnHook(STAGE_CREATE_SUBSPAWN_EXECUTE, HOOK_PRIO_HIGHEST - 1, PrepareInstanceChild);
    AddAppSpawnHook(STAGE_CREATE_SUBSPAWN_EXECUTE, HOOK_PRIO_LOWEST, ExecInstanceChild);
    AddAppSpawnHook(STAGE_SERVER_SPAWN_ABORT, HOOK_PRIO_COMMON, InstanceCreateAborted);
    AddProcessMgrHook(STAGE_SERVER_APP_DIED, HOOK_PRIO_HIGHEST - 1, InstanceDied);
    AddPreloadHook(HOOK_PRIO_LOWEST, SubspawnInstanceInit);
}
