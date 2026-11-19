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

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cJSON.h>
#include <gtest/gtest.h>

#include "appspawn.h"
#include "app_spawn_stub.h"
#include "appspawn_adapter.h"
#include "appspawn_hook.h"
#include "appspawn_manager.h"
#include "appspawn_msg.h"
#include "appspawn_server.h"
#include "app_spawn_test_helper.h"
#include "appspawn_utils.h"
#include "securec.h"
#include "subspawn_manager.h"
#include "subspawn_preload.h"

#include "lib_wrapper.h"
#include "child_process_api.h"
#include "parameters.h"

static const SubspawnStartInfo *TestGetStartInfo();
static void TestSetStartResult(int result);
static void TestRegChildLooper(AppSpawnContent *content, ChildLoop loop);
static void TestEnvClear(AppSpawnContent *content, AppSpawnClient *client);
static int TestMakeDirRec(const char *path, mode_t mode, int lastPath);
void ResetSubspawnPreloadMock();
bool GetSubspawnPreloadSystemAppForTest();

namespace OHOS {
namespace AppExecFwk {
class SubspawnChildProcessMock {
public:
    static void StartChild(const std::map<std::string, int32_t> &fds);
};
}
namespace system {
bool SubspawnTestGetBoolParameter(const std::string &key, bool def);
}
}

// The module keeps its helpers static; including the implementation is the
// established pattern for covering them (see app_spawn_sandbox_debug_mode).
// RunChildProcessor is exported when APPSPAWN_TEST is defined and clashes with
// the nweb adapter copy, so give this translation unit a private name.
#define RunChildProcessor SubspawnRunChildProcessor
// 仅当前测试翻译单元启用实例身份校验，不改变其他模块的 SPM 构建行为。
#define APPSPAWN_ENABLE_SPM
#define GetSubspawnStartInfo TestGetStartInfo
#define SubspawnSetStartResult TestSetStartResult
#define RegChildLooper TestRegChildLooper
#define AppSpawnEnvClear TestEnvClear
#define MakeDirRec TestMakeDirRec
#define ChildProcessApi SubspawnChildProcessMock
#define GetBoolParameter SubspawnTestGetBoolParameter
#include "subspawn_adapter.cpp"
#undef RunChildProcessor
#undef APPSPAWN_ENABLE_SPM
#undef GetSubspawnStartInfo
#undef SubspawnSetStartResult
#undef RegChildLooper
#undef AppSpawnEnvClear
#undef MakeDirRec
#undef ChildProcessApi
#undef GetBoolParameter

using namespace testing;
using namespace testing::ext;
using namespace OHOS;

constexpr uid_t TEST_APP_UID = 20010029;
constexpr uint32_t TEST_BUNDLE_INDEX = 100;
constexpr size_t TEST_FD_KEY_MAX_LEN = 32;
constexpr int TEST_FD_BASE = 40;

struct SubspawnMock {
    bool bindFail = false;
    bool chmodFail = false;
    bool chownFail = false;
    bool listenFail = false;
    bool memfdFail = false;
    bool sealFail = false;
    bool opendirFail = false;
    bool lseekFail = false;
    bool socketFail = false;
    bool fcntlFail = false;
    int writeEintrTimes = 0;
    int writeCalls = 0;
    int writeFailAt = 0;
    size_t writeLimit = 0;
    bool writeZero = false;
    int writeFd = -1;
    int preadEintrTimes = 0;
    size_t preadLimit = 0;
    bool readdirFail = false;
    bool closedirFail = false;
    int fcntlFailCommand = -1;
    int fcntlFailFd = -1;
    int fcntlFlags = 0;
    std::vector<std::pair<int, unsigned long>> fdFlags;
    bool mkdirFail = false;
    bool capgetFail = false;
    int noNewPrivs = 1;
    bool setNoNewPrivsFail = false;
    int setNoNewPrivsCalls = 0;
    int bootstrapListenFd = -1;
    bool encapsOpenFail = false;
    int encapsOpenCalls = 0;
    int encapsFd = -1;
    bool pdeathFail = false;
    bool getconFail = false;
    bool nullContext = false;
    std::string context = "u:r:subspawn:s0";
    int freeconCalls = 0;
    struct __user_cap_data_struct caps[2] = {};
    bool setsockFail = false;
    bool resetSockConFail = false;
    int uidOverride = -1;
    int euidOverride = -1;
    int gidOverride = -1;
    int egidOverride = -1;
    int ppidOverride = -1;
    int socketFd = -1;
    int unlinkCount = 0;
    int chownUid = -1;
    int chownGid = -1;
    int execCalls = 0;
    std::vector<std::string> execArgs;
    int startResult = -1;
    int startResultCalls = 0;
    int registerCalls = 0;
    ChildLoop childLoop = nullptr;
    int envClearCalls = 0;
    int startChildCalls = 0;
    bool checkExit = true;
    std::string checkExitAtStart;
    std::map<std::string, int32_t> childFds;
};

static SubspawnMock g_subspawnMock;
static SubspawnStartInfo g_startInfo;
static bool g_trackFds = false;
static std::set<int> g_ownedFds;
static std::vector<AppSpawningCtx *> g_properties;
static std::map<std::string, std::pair<bool, std::string>> g_savedEnvironment;

static int TrackFd(int fd)
{
    if (g_trackFds && fd >= 0) {
        g_ownedFds.insert(fd);
    }
    return fd;
}

static void SaveEnvironment(const std::string &key)
{
    if (g_savedEnvironment.count(key) == 0) {
        const char *value = getenv(key.c_str());
        g_savedEnvironment[key] = {value != nullptr, value == nullptr ? "" : value};
    }
}

// __wrap_*/__real_* names are fixed by the linker --wrap contract, so the
// reserved-identifier and naming checks do not apply to them.
// NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming)
extern "C" {
int __real_socket(int domain, int type, int protocol);
int __real_getsockname(int fd, struct sockaddr *address, socklen_t *length);
int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);
int __real_chmod(const char *path, mode_t mode);
int __real_chown(const char *path, uid_t uid, gid_t gid);
int __real_listen(int fd, int backlog);
off_t __real_lseek(int fd, off_t offset, int whence);
DIR *__real_opendir(const char *name);
struct dirent *__real_readdir(DIR *dir);
int __real_closedir(DIR *dir);
int __real_unlink(const char *path);
int __real_execv(const char *path, char *const argv[]);
int __real_prctl(int option, ...);
uid_t __real_getuid(void);
uid_t __real_geteuid(void);
gid_t __real_getgid(void);
gid_t __real_getegid(void);
pid_t __real_getppid(void);
int __real_close(int fd);
int __real_pipe(int fds[2]);
ssize_t __real_pread(int fd, void *buffer, size_t size, off_t offset);

int __wrap_socket(int domain, int type, int protocol)
{
    if (g_subspawnMock.socketFail) {
        return -1;
    }
    g_subspawnMock.socketFd = TrackFd(__real_socket(domain, type, protocol));
    return g_subspawnMock.socketFd;
}

int __wrap_getsockname(int fd, struct sockaddr *address, socklen_t *length)
{
    if (g_subspawnMock.bootstrapListenFd < 0 || fd != g_subspawnMock.bootstrapListenFd) {
        return __real_getsockname(fd, address, length);
    }
    if (address == nullptr || length == nullptr || *length < sizeof(sockaddr_un)) {
        errno = EINVAL;
        return -1;
    }
    auto *unixAddress = reinterpret_cast<sockaddr_un *>(address);
    unixAddress->sun_family = AF_UNIX;
    const std::string path = SUBSPAWN_SOCKET_PREFIX + std::to_string(TEST_APP_UID);
    *length = sizeof(sockaddr_un);
    return strcpy_s(unixAddress->sun_path, sizeof(unixAddress->sun_path), path.c_str()) == EOK ? 0 : -1;
}

int __wrap_close(int fd)
{
    if (g_trackFds) {
        g_ownedFds.erase(fd);
    }
    return __real_close(fd);
}

int __wrap_pipe(int fds[2])
{
    int result = __real_pipe(fds);
    if (result == 0) {
        TrackFd(fds[0]);
        TrackFd(fds[1]);
    }
    return result;
}

ssize_t __wrap_pread(int fd, void *buffer, size_t size, off_t offset)
{
    if (g_subspawnMock.preadEintrTimes > 0) {
        --g_subspawnMock.preadEintrTimes;
        errno = EINTR;
        return -1;
    }
    if (g_subspawnMock.preadLimit > 0 && size > g_subspawnMock.preadLimit) {
        size = g_subspawnMock.preadLimit;
    }
    return __real_pread(fd, buffer, size, offset);
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return g_subspawnMock.bindFail ? -1 : 0;
}

int __wrap_chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return g_subspawnMock.chmodFail ? -1 : 0;
}

int __wrap_chown(const char *path, uid_t uid, gid_t gid)
{
    if (g_subspawnMock.chownFail) {
        return -1;
    }
    g_subspawnMock.chownUid = static_cast<int>(uid);
    g_subspawnMock.chownGid = static_cast<int>(gid);
    return 0;
}

int __wrap_listen(int fd, int backlog)
{
    return g_subspawnMock.listenFail ? -1 : 0;
}

off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    return g_subspawnMock.lseekFail ? -1 : __real_lseek(fd, offset, whence);
}

DIR *__wrap_opendir(const char *name)
{
    return g_subspawnMock.opendirFail ? nullptr : __real_opendir(name);
}

struct dirent *__wrap_readdir(DIR *dir)
{
    if (g_subspawnMock.readdirFail) {
        errno = EIO;
        return nullptr;
    }
    return __real_readdir(dir);
}

int __wrap_closedir(DIR *dir)
{
    int result = __real_closedir(dir);
    if (g_subspawnMock.closedirFail) {
        errno = EIO;
        return -1;
    }
    return result;
}

int __wrap_unlink(const char *path)
{
    g_subspawnMock.unlinkCount++;
    return 0;
}

int __wrap_execv(const char *path, char *const argv[])
{
    ++g_subspawnMock.execCalls;
    for (size_t i = 0; argv[i] != nullptr; ++i) {
        g_subspawnMock.execArgs.emplace_back(argv[i]);
    }
    errno = ENOENT;
    return -1;
}

int __wrap_prctl(int option, ...)
{
    if (option == PR_GET_NO_NEW_PRIVS) {
        return g_subspawnMock.noNewPrivs;
    }
    if (option == PR_SET_NO_NEW_PRIVS) {
        ++g_subspawnMock.setNoNewPrivsCalls;
        if (g_subspawnMock.setNoNewPrivsFail) {
            errno = EPERM;
            return -1;
        }
    }
    return option == PR_SET_PDEATHSIG && g_subspawnMock.pdeathFail ? -1 : 0;
}

int __wrap_capget(cap_user_header_t header, cap_user_data_t data)
{
    if (g_subspawnMock.capgetFail) {
        return -1;
    }
    return memcpy_s(data, sizeof(g_subspawnMock.caps), g_subspawnMock.caps, sizeof(g_subspawnMock.caps));
}

uid_t __wrap_getuid(void)
{
    return g_subspawnMock.uidOverride >= 0 ? static_cast<uid_t>(g_subspawnMock.uidOverride) : __real_getuid();
}

uid_t __wrap_geteuid(void)
{
    if (g_subspawnMock.euidOverride >= 0) {
        return static_cast<uid_t>(g_subspawnMock.euidOverride);
    }
    return g_subspawnMock.uidOverride >= 0 ? static_cast<uid_t>(g_subspawnMock.uidOverride) : __real_geteuid();
}

gid_t __wrap_getgid(void)
{
    return g_subspawnMock.gidOverride >= 0 ? static_cast<gid_t>(g_subspawnMock.gidOverride) : __real_getgid();
}

gid_t __wrap_getegid(void)
{
    if (g_subspawnMock.egidOverride >= 0) {
        return static_cast<gid_t>(g_subspawnMock.egidOverride);
    }
    return g_subspawnMock.gidOverride >= 0 ? static_cast<gid_t>(g_subspawnMock.gidOverride) : __real_getegid();
}

pid_t __wrap_getppid(void)
{
    return g_subspawnMock.ppidOverride >= 0 ? static_cast<pid_t>(g_subspawnMock.ppidOverride) : __real_getppid();
}

#ifdef WITH_SELINUX
int __wrap_getcon(char **context)
{
    *context = g_subspawnMock.nullContext ? nullptr : strdup(g_subspawnMock.context.c_str());
    return g_subspawnMock.getconFail ? -1 : 0;
}

void __wrap_freecon(char *context)
{
    ++g_subspawnMock.freeconCalls;
    free(context);
}

int __wrap_setsockcreatecon(const char *context)
{
    if (context == nullptr) {
        return g_subspawnMock.resetSockConFail ? -1 : 0;
    }
    return g_subspawnMock.setsockFail ? -1 : 0;
}
#endif
}  // extern "C"

static ssize_t MockWrite(int fd, const void *buf, size_t count)
{
    bool selected = g_subspawnMock.writeFd < 0 || fd == g_subspawnMock.writeFd;
    if (selected && g_subspawnMock.writeEintrTimes > 0) {
        g_subspawnMock.writeEintrTimes--;
        errno = EINTR;
        return -1;
    }
    if (selected && ++g_subspawnMock.writeCalls == g_subspawnMock.writeFailAt) {
        errno = EIO;
        return -1;
    }
    if (selected && g_subspawnMock.writeZero) {
        return 0;
    }
    if (selected && g_subspawnMock.writeLimit > 0 && count > g_subspawnMock.writeLimit) {
        count = g_subspawnMock.writeLimit;
    }
    UpdateWriteFunc(nullptr);
    ssize_t result = write(fd, buf, count);
    UpdateWriteFunc(MockWrite);
    return result;
}

static int MockFcntl(int fd, int flag, unsigned long arg)
{
    if (g_subspawnMock.fcntlFail || (flag == g_subspawnMock.fcntlFailCommand &&
        (g_subspawnMock.fcntlFailFd < 0 || fd == g_subspawnMock.fcntlFailFd))) {
        return -1;
    }
    if (flag == F_ADD_SEALS && g_subspawnMock.sealFail) {
        return -1;
    }
    if (flag == F_GETFD) {
        return g_subspawnMock.fcntlFlags;
    }
    if (flag == F_SETFD) {
        g_subspawnMock.fdFlags.emplace_back(fd, arg);
        return 0;
    }
    return __real_fcntl(fd, flag, arg);
}

static int MockEncapsOpen(const char *path, int flags, mode_t mode)
{
    if (strcmp(path, "/dev/encaps") != 0) {
        return __real_open(path, flags, mode);
    }
    ++g_subspawnMock.encapsOpenCalls;
    if (g_subspawnMock.encapsOpenFail) {
        errno = EACCES;
        return -1;
    }
    g_subspawnMock.encapsFd = TrackFd(__real_open("/dev/null", O_RDONLY));
    return g_subspawnMock.encapsFd;
}

static int MockMemfdCreate(const char *name, unsigned flags)
{
    if (g_subspawnMock.memfdFail) {
        return -1;
    }
    int fd = TrackFd(__real_memfd_create(name, flags));
    g_subspawnMock.writeFd = fd;
    g_subspawnMock.writeCalls = 0;
    return fd;
}

static const SubspawnStartInfo *TestGetStartInfo()
{
    return &g_startInfo;
}

static void TestSetStartResult(int result)
{
    g_subspawnMock.startResult = result;
    ++g_subspawnMock.startResultCalls;
}

static void TestRegChildLooper(AppSpawnContent *content, ChildLoop loop)
{
    ++g_subspawnMock.registerCalls;
    g_subspawnMock.childLoop = loop;
}

static void TestEnvClear(AppSpawnContent *content, AppSpawnClient *client)
{
    ++g_subspawnMock.envClearCalls;
}

static int TestMakeDirRec(const char *path, mode_t mode, int lastPath)
{
    return g_subspawnMock.mkdirFail ? -1 : 0;
}

void OHOS::AppExecFwk::SubspawnChildProcessMock::StartChild(const std::map<std::string, int32_t> &fds)
{
    ++g_subspawnMock.startChildCalls;
    g_subspawnMock.childFds = fds;
    const char *value = getenv(APPSPAWN_CHECK_EXIT);
    if (value == nullptr) {
        value = "";
    }
    g_subspawnMock.checkExitAtStart = value;
}

bool OHOS::system::SubspawnTestGetBoolParameter(const std::string &key, bool def)
{
    return g_subspawnMock.checkExit;
}
// NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming)

namespace OHOS {
class SubspawnAdapterTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp()
    {
        g_subspawnMock = SubspawnMock();
        g_trackFds = true;
        g_startInfo = {};
        g_startInfo.memFd = -1;
        g_startInfo.uid = TEST_APP_UID;
        uint32_t caps = (1U << CAP_SETUID) | (1U << CAP_SETGID) | (1U << CAP_KILL);
        g_subspawnMock.caps[0] = {caps, caps, caps};
        ResetSubspawnPreloadMock();
        SaveEnvironment(APPSPAWN_CHECK_EXIT);
        UpdateMemfdCreateFunc(MockMemfdCreate);
        UpdateWriteFunc(MockWrite);
        // The wrapper's second argument is a bypass filter; 0 lets every command reach the mock.
        UpdateFcntlFunc(MockFcntl, 0);
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " start";
        APPSPAWN_LOGI("%{public}s.%{public}s start", info->test_suite_name(), info->name());
    }
    void TearDown()
    {
        UpdateFcntlFunc(nullptr, -1);
        UpdateMemfdCreateFunc(nullptr);
        UpdateWriteFunc(nullptr);
        UpdateMallocFunc(nullptr);
        UpdateOpenFunc(nullptr);
        SetIoctlResult(0);
        for (AppSpawningCtx *property : g_properties) {
            // pipe/memfd/socket 由本夹具统一管理，避免与用例内 close 重复关闭。
            property->forkCtx.fd[0] = -1;
            property->forkCtx.fd[1] = -1;
            DeleteAppSpawningCtx(property);
        }
        g_properties.clear();
        while (!g_ownedFds.empty()) {
            close(*g_ownedFds.begin());
        }
        g_trackFds = false;
        for (const auto &entry : g_savedEnvironment) {
            if (entry.second.first) {
                setenv(entry.first.c_str(), entry.second.second.c_str(), 1);
            } else {
                unsetenv(entry.first.c_str());
            }
        }
        g_savedEnvironment.clear();
        const TestInfo *info = UnitTest::GetInstance()->current_test_info();
        GTEST_LOG_(INFO) << info->test_suite_name() << "." << info->name() << " end";
        APPSPAWN_LOGI("%{public}s.%{public}s end", info->test_suite_name(), info->name());
    }
};

/**
 * @tc.name: Subspawn_GetInstanceSocketName_001
 * @tc.desc: 正常 UID 能生成实例 socket 路径
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_GetInstanceSocketName_001, TestSize.Level0)
{
    char name[SUBSPAWN_SOCKET_NAME_LENGTH] = {0};
    ASSERT_EQ(GetInstanceSocketName(TEST_APP_UID, name, sizeof(name)), 0);
    ASSERT_NE(std::string(name).find("subspawn_20010029"), std::string::npos);
}

/**
 * @tc.name: Subspawn_GetInstanceSocketName_002
 * @tc.desc: 长度非法时返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_GetInstanceSocketName_002, TestSize.Level0)
{
    char name[SUBSPAWN_SOCKET_NAME_LENGTH] = {0};
    ASSERT_EQ(GetInstanceSocketName(TEST_APP_UID, name, 0), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_WriteAll_001
 * @tc.desc: 正常写入返回成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_WriteAll_001, TestSize.Level0)
{
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    const char payload[] = "subspawn";
    ASSERT_EQ(WriteAll(fds[1], payload, sizeof(payload)), 0);
    close(fds[0]);
    close(fds[1]);
}

/**
 * @tc.name: Subspawn_WriteAll_002
 * @tc.desc: 写入失败返回错误码
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_WriteAll_002, TestSize.Level0)
{
    const char payload[] = "subspawn";
    ASSERT_EQ(WriteAll(-1, payload, sizeof(payload)), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_CreateMessageFd_001
 * @tc.desc: 消息头写入成功返回有效 fd
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_CreateMessageFd_001, TestSize.Level0)
{
    AppSpawnMsgNode message = {};
    message.msgHeader.msgLen = sizeof(AppSpawnMsg);
    int fd = CreateMessageFd(&message);
    ASSERT_GE(fd, 0);
    close(fd);
}

/**
 * @tc.name: Subspawn_CreateMessageFd_002
 * @tc.desc: memfd 创建失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_CreateMessageFd_002, TestSize.Level0)
{
    g_subspawnMock.memfdFail = true;
    AppSpawnMsgNode message = {};
    message.msgHeader.msgLen = sizeof(AppSpawnMsg);
    ASSERT_EQ(CreateMessageFd(&message), -1);
}

/**
 * @tc.name: Subspawn_CreateMessageFd_003
 * @tc.desc: 加 seal 失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_CreateMessageFd_003, TestSize.Level0)
{
    g_subspawnMock.sealFail = true;
    AppSpawnMsgNode message = {};
    message.msgHeader.msgLen = sizeof(AppSpawnMsg);
    ASSERT_EQ(CreateMessageFd(&message), -1);
}

/**
 * @tc.name: Subspawn_SetInheritedFdsCloseOnExec_001
 * @tc.desc: 正常遍历 /proc/self/fd 返回成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SetInheritedFdsCloseOnExec_001, TestSize.Level0)
{
    ASSERT_EQ(SetInheritedFdsCloseOnExec(nullptr, 0), 0);
}

/**
 * @tc.name: Subspawn_SetInheritedFdsCloseOnExec_002
 * @tc.desc: 打开 /proc/self/fd 失败返回错误码
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SetInheritedFdsCloseOnExec_002, TestSize.Level0)
{
    g_subspawnMock.opendirFail = true;
    ASSERT_EQ(SetInheritedFdsCloseOnExec(nullptr, 0), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_BindInstanceSocket_001
 * @tc.desc: socket 全流程成功返回 fd
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_001, TestSize.Level0)
{
    int fd = BindInstanceSocket(TEST_APP_UID);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(fd, g_subspawnMock.socketFd);
    ASSERT_EQ(g_subspawnMock.chownUid, TEST_APP_UID);
    ASSERT_EQ(g_subspawnMock.chownGid, TEST_APP_UID);
}

/**
 * @tc.name: Subspawn_BindInstanceSocket_002
 * @tc.desc: bind 失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_002, TestSize.Level0)
{
    g_subspawnMock.bindFail = true;
    ASSERT_EQ(BindInstanceSocket(TEST_APP_UID), -1);
}

/**
 * @tc.name: Subspawn_BindInstanceSocket_003
 * @tc.desc: chmod 失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_003, TestSize.Level0)
{
    g_subspawnMock.chmodFail = true;
    ASSERT_EQ(BindInstanceSocket(TEST_APP_UID), -1);
}

/**
 * @tc.name: Subspawn_BindInstanceSocket_004
 * @tc.desc: chown 失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_004, TestSize.Level0)
{
    g_subspawnMock.chownFail = true;
    ASSERT_EQ(BindInstanceSocket(TEST_APP_UID), -1);
}

/**
 * @tc.name: Subspawn_BindInstanceSocket_005
 * @tc.desc: listen 失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_005, TestSize.Level0)
{
    g_subspawnMock.listenFail = true;
    ASSERT_EQ(BindInstanceSocket(TEST_APP_UID), -1);
}

/**
 * @tc.name: Subspawn_UnlinkInstanceSocket_001
 * @tc.desc: 合法 uid 会尝试 unlink 实例 socket
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_UnlinkInstanceSocket_001, TestSize.Level0)
{
    int before = g_subspawnMock.unlinkCount;
    UnlinkInstanceSocket(TEST_APP_UID);
    ASSERT_EQ(g_subspawnMock.unlinkCount, before + 1);
}

/**
 * @tc.name: Subspawn_PreloadLibraries_001
 * @tc.desc: 模拟库加载失败，预加载返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PreloadLibraries_001, TestSize.Level0)
{
    cJSON *config = cJSON_Parse("[{\"ns\":\"default\",\"libPath\":[\"/lib\"],\"so\":[\"libentry.so\"]}]");
    ASSERT_NE(config, nullptr);
    ASSERT_EQ(SubspawnPreloadLibraries(config, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    cJSON_Delete(config);
}

/**
 * @tc.name: Subspawn_SetAppLibPath_001
 * @tc.desc: 入参非法时返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SetAppLibPath_001, TestSize.Level0)
{
    cJSON *paths = cJSON_CreateArray();
    ASSERT_NE(paths, nullptr);
    ASSERT_EQ(SubspawnSetAppLibPath(nullptr, paths, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    ASSERT_EQ(SubspawnSetAppLibPath("", paths, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    ASSERT_EQ(SubspawnSetAppLibPath("default", nullptr, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    cJSON_Delete(paths);
}

/**
 * @tc.name: Subspawn_SetAppLibPath_002
 * @tc.desc: 空数组或全空字符串返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SetAppLibPath_002, TestSize.Level0)
{
    cJSON *paths = cJSON_CreateArray();
    ASSERT_NE(paths, nullptr);
    ASSERT_EQ(SubspawnSetAppLibPath("default", paths, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    cJSON_AddItemToArray(paths, cJSON_CreateString(""));
    ASSERT_EQ(SubspawnSetAppLibPath("default", paths, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    cJSON_Delete(paths);
}

/**
 * @tc.name: Subspawn_SetAppLibPath_003
 * @tc.desc: 数组元素非字符串返回失败
 * @tc.type: FUNC
 */
/**
 * @tc.name: Subspawn_WriteAll_003
 * @tc.desc: EINTR 时重试并最终成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_WriteAll_003, TestSize.Level0)
{
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    g_subspawnMock.writeEintrTimes = 1;
    const char payload[] = "eintr";
    ASSERT_EQ(WriteAll(fds[1], payload, sizeof(payload)), 0);
    close(fds[0]);
    close(fds[1]);
}

/**
 * @tc.name: Subspawn_CreateMessageFd_004
 * @tc.desc: lseek 失败时关闭 fd 并返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_CreateMessageFd_004, TestSize.Level0)
{
    g_subspawnMock.lseekFail = true;
    AppSpawnMsgNode message = {};
    message.msgHeader.msgLen = sizeof(AppSpawnMsg);
    ASSERT_EQ(CreateMessageFd(&message), -1);
}

/**
 * @tc.name: Subspawn_SetInheritedFdsCloseOnExec_003
 * @tc.desc: fcntl 失败时返回错误码
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SetInheritedFdsCloseOnExec_003, TestSize.Level0)
{
    int fd = TrackFd(open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_GE(fd, 0);
    g_subspawnMock.fcntlFail = true;
    ASSERT_EQ(SetInheritedFdsCloseOnExec(nullptr, 0), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_BindInstanceSocket_006
 * @tc.desc: socket 创建失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_006, TestSize.Level0)
{
    g_subspawnMock.socketFail = true;
    ASSERT_EQ(BindInstanceSocket(TEST_APP_UID), -1);
}

#ifdef WITH_SELINUX
/**
 * @tc.name: Subspawn_BindInstanceSocket_007
 * @tc.desc: socket 打标签失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_007, TestSize.Level0)
{
    g_subspawnMock.setsockFail = true;
    ASSERT_EQ(BindInstanceSocket(TEST_APP_UID), -1);
}

/**
 * @tc.name: Subspawn_BindInstanceSocket_008
 * @tc.desc: 恢复 socket 创建上下文失败返回 -1
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BindInstanceSocket_008, TestSize.Level0)
{
    g_subspawnMock.resetSockConFail = true;
    ASSERT_EQ(BindInstanceSocket(TEST_APP_UID), -1);
}
#endif

HWTEST_F(SubspawnAdapterTest, Subspawn_SetAppLibPath_003, TestSize.Level0)
{
    cJSON *paths = cJSON_CreateArray();
    ASSERT_NE(paths, nullptr);
    cJSON_AddItemToArray(paths, cJSON_CreateNumber(1));
    ASSERT_EQ(SubspawnSetAppLibPath("default", paths, false), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    cJSON_Delete(paths);
}

static AppSpawningCtx *CreateSubspawnProperty(AppSpawnTestHelper &helper, AppSpawnMsgType msgType,
    bool withDac = true, bool withToken = true, bool childProcess = false)
{
    static const char *bundleName = "com.example.subspawn";
    AppSpawnReqMsgHandle req = 0;
    if (AppSpawnReqMsgCreate(msgType, bundleName, &req) != 0) {
        return nullptr;
    }
    (void)AppSpawnReqMsgSetBundleInfo(req, TEST_BUNDLE_INDEX, bundleName);
    (void)AppSpawnReqMsgSetAppFlag(req, APP_FLAGS_COLD_BOOT);
    if (childProcess) {
        (void)AppSpawnReqMsgSetAppFlag(req, APP_FLAGS_CHILDPROCESS);
        (void)AppSpawnReqMsgAddStringInfo(req, MSG_EXT_NAME_APP_FD, "input");
    }
    (void)AppSpawnReqMsgSetAppDomainInfo(req, 0, "normal");
    if (withToken) {
        (void)AppSpawnReqMsgSetAppAccessToken(req, 0x12345678);
    }
    if (withDac) {
        AppDacInfo dac = {};
        dac.uid = TEST_APP_UID;
        dac.gid = TEST_APP_UID;
        dac.gidCount = 1;
        dac.gidTable[0] = TEST_APP_UID;
        (void)strcpy_s(dac.userName, sizeof(dac.userName), "subspawn_test");
        (void)AppSpawnReqMsgSetAppDacInfo(req, &dac);
    }
    AppSpawningCtx *property = helper.GetAppProperty(nullptr, req);
    if (property != nullptr) {
        g_properties.push_back(property);
    }
    return property;
}

static void InitSubspawnMgr(AppSpawnMgr &mgr, RunMode mode)
{
    mgr = AppSpawnMgr();
    mgr.content.mode = mode;
    mgr.content.longProcName = const_cast<char *>(APPSPAWN_SERVER_NAME);
    mgr.servicePid = getppid();
}

/**
 * @tc.name: Subspawn_ValidateInstanceSpawn_001
 * @tc.desc: 合法 CREATE 请求通过校验
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ValidateInstanceSpawn_001, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(ValidateInstanceSpawn(&mgr, property), 0);
}

/**
 * @tc.name: Subspawn_ValidateInstanceSpawn_002
 * @tc.desc: 非 appspawn 服务名拒绝创建实例
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ValidateInstanceSpawn_002, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    mgr.content.longProcName = const_cast<char *>("nwebspawn");
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(ValidateInstanceSpawn(&mgr, property), APPSPAWN_SUBSPAWN_INSTANCE_INVALID);
}

/**
 * @tc.name: Subspawn_ValidateInstanceSpawn_003
 * @tc.desc: 非 CREATE 消息拒绝创建实例
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ValidateInstanceSpawn_003, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_APP_SPAWN);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(ValidateInstanceSpawn(&mgr, property), APPSPAWN_SUBSPAWN_INSTANCE_INVALID);
}

/**
 * @tc.name: Subspawn_PrepareInstanceChild_001
 * @tc.desc: 非 CREATE 消息直接返回成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PrepareInstanceChild_001, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_APP_SPAWN);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), 0);
}

/**
 * @tc.name: Subspawn_PrepareInstanceChild_002
 * @tc.desc: 非 appspawn 模式拒绝准备实例
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PrepareInstanceChild_002, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_COLD_RUN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_INSTANCE_INVALID);
}

/**
 * @tc.name: Subspawn_PrepareInstanceChild_003
 * @tc.desc: 缺少 DAC 信息返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PrepareInstanceChild_003, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE, false);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_PrepareInstanceChild_004
 * @tc.desc: 缺少 bootstrap 管道返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PrepareInstanceChild_004, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    property->forkCtx.fd[1] = -1;
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_PrepareInstanceChild_005
 * @tc.desc: 正常准备实例返回成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PrepareInstanceChild_005, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    property->forkCtx.fd[1] = fds[1];
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), 0);
    ASSERT_EQ(property->subspawnFds[SUBSPAWN_PIPE_FD_INDEX], fds[1]);
    ASSERT_EQ(property->subspawnFds[SUBSPAWN_LISTEN_FD_INDEX], g_subspawnMock.socketFd);
    close(fds[0]);
    close(fds[1]);
}

/**
 * @tc.name: Subspawn_ExecInstanceChild_001
 * @tc.desc: 非实例创建直接返回成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ExecInstanceChild_001, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_APP_SPAWN);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(ExecInstanceChild(&mgr, property), 0);
}

/**
 * @tc.name: Subspawn_ExecInstanceChild_002
 * @tc.desc: 缺少 DAC 信息返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ExecInstanceChild_002, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE, false);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_ExecInstanceChild_004
 * @tc.desc: 进程身份与 DAC 不一致返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ExecInstanceChild_004, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_ExecInstanceChild_005
 * @tc.desc: 父进程身份不一致返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ExecInstanceChild_005, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    mgr.servicePid = 1;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    g_subspawnMock.uidOverride = TEST_APP_UID;
    g_subspawnMock.gidOverride = TEST_APP_UID;
    ASSERT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

/**
 * @tc.name: Subspawn_ExecInstanceChild_006
 * @tc.desc: execv 失败返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_ExecInstanceChild_006, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    mgr.servicePid = getppid();
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    property->subspawnFds[SUBSPAWN_MEM_FD_INDEX] = 10;
    property->subspawnFds[SUBSPAWN_PIPE_FD_INDEX] = 11;
    property->subspawnFds[SUBSPAWN_LISTEN_FD_INDEX] = 12;
    g_subspawnMock.uidOverride = TEST_APP_UID;
    g_subspawnMock.gidOverride = TEST_APP_UID;
    ASSERT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    ASSERT_EQ(g_subspawnMock.execCalls, 1);
    ASSERT_GE(g_subspawnMock.execArgs.size(), 5U);
    EXPECT_EQ(g_subspawnMock.execArgs[0], "/system/bin/subspawn");
    EXPECT_EQ(g_subspawnMock.execArgs[3], "-subspawn-fds");
    EXPECT_EQ(g_subspawnMock.execArgs[4], "11:10:12:" +
        std::to_string(property->message->msgHeader.msgLen) + ":20010029");
}

/**
 * @tc.name: Subspawn_InstanceCreateAborted_001
 * @tc.desc: 创建失败的实例会清理 socket
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_InstanceCreateAborted_001, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    property->pid = 1234;
    int before = g_subspawnMock.unlinkCount;
    ASSERT_EQ(InstanceCreateAborted(&mgr, property), 0);
    ASSERT_EQ(g_subspawnMock.unlinkCount, before + 1);
}

/**
 * @tc.name: Subspawn_InstanceCreateAborted_002
 * @tc.desc: 非 CREATE 失败不清理 socket
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_InstanceCreateAborted_002, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_APP_SPAWN);
    ASSERT_NE(property, nullptr);
    property->pid = 1234;
    int before = g_subspawnMock.unlinkCount;
    ASSERT_EQ(InstanceCreateAborted(&mgr, property), 0);
    ASSERT_EQ(g_subspawnMock.unlinkCount, before);
}

/**
 * @tc.name: Subspawn_InstanceDied_001
 * @tc.desc: 实例退出后清理 socket
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_InstanceDied_001, TestSize.Level0)
{
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawnedProcessInfo info = {};
    info.isSubspawn = true;
    info.uid = TEST_APP_UID;
    int before = g_subspawnMock.unlinkCount;
    ASSERT_EQ(InstanceDied(&mgr, &info), 0);
    ASSERT_EQ(g_subspawnMock.unlinkCount, before + 1);
}

/**
 * @tc.name: Subspawn_InstanceDied_002
 * @tc.desc: 非实例进程退出不清理 socket
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_InstanceDied_002, TestSize.Level0)
{
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawnedProcessInfo info = {};
    info.isSubspawn = false;
    int before = g_subspawnMock.unlinkCount;
    ASSERT_EQ(InstanceDied(&mgr, &info), 0);
    ASSERT_EQ(g_subspawnMock.unlinkCount, before);
}

static AppSpawningCtx *CreateSubspawnPropertyWithPreload(AppSpawnTestHelper &helper, const char *json)
{
    static const char *bundleName = "com.example.subspawn";
    AppSpawnReqMsgHandle req = 0;
    if (AppSpawnReqMsgCreate(MSG_SUBSPAWN_CREATE, bundleName, &req) != 0) {
        return nullptr;
    }
    (void)AppSpawnReqMsgSetBundleInfo(req, TEST_BUNDLE_INDEX, bundleName);
    (void)AppSpawnReqMsgSetAppFlag(req, APP_FLAGS_COLD_BOOT);
    (void)AppSpawnReqMsgSetAppDomainInfo(req, 0, "normal");
    (void)AppSpawnReqMsgSetAppAccessToken(req, 0x12345678);
    AppDacInfo dac = {};
    dac.uid = TEST_APP_UID;
    dac.gid = TEST_APP_UID;
    dac.gidCount = 1;
    dac.gidTable[0] = TEST_APP_UID;
    (void)AppSpawnReqMsgSetAppDacInfo(req, &dac);
    (void)AppSpawnReqMsgAddStringInfo(req, MSG_EXT_NAME_SUBSPAWN_PRELOAD, json);
    AppSpawningCtx *property = helper.GetAppProperty(nullptr, req);
    if (property != nullptr) {
        g_properties.push_back(property);
    }
    return property;
}

/**
 * @tc.name: Subspawn_SubspawnPreload_001
 * @tc.desc: 无预加载配置直接返回成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SubspawnPreload_001, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(SubspawnPreload(property->message), 0);
}

/**
 * @tc.name: Subspawn_SubspawnPreload_002
 * @tc.desc: 非法 JSON 返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SubspawnPreload_002, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnPropertyWithPreload(helper, "{not-json");
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(SubspawnPreload(property->message), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
}

/**
 * @tc.name: Subspawn_SubspawnPreload_003
 * @tc.desc: 合法 JSON 转交预加载流程
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_SubspawnPreload_003, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnPropertyWithPreload(helper,
        "[{\"ns\":\"default\",\"libPath\":[\"/lib\"],\"so\":[\"libentry.so\"]}]");
    ASSERT_NE(property, nullptr);
    ASSERT_EQ(SubspawnPreload(property->message), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
}

HWTEST_F(SubspawnAdapterTest, PreloadUsesSystemAppFullToken, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnPropertyWithPreload(helper,
        "[{\"ns\":\"default\",\"libPath\":[\"/lib\"],\"so\":[]}]");
    ASSERT_NE(property, nullptr);
    auto *token = static_cast<AppSpawnMsgAccessToken *>(GetAppSpawnMsgInfo(property->message, TLV_ACCESS_TOKEN_INFO));
    ASSERT_NE(token, nullptr);
    token->accessTokenIdEx = (1ULL << 32) | 0x12345678ULL;
    ASSERT_EQ(SubspawnPreload(property->message), 0);
    EXPECT_TRUE(GetSubspawnPreloadSystemAppForTest());
}

HWTEST_F(SubspawnAdapterTest, PreloadUsesOrdinaryAppFullToken, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnPropertyWithPreload(helper,
        "[{\"ns\":\"default\",\"libPath\":[\"/lib\"],\"so\":[]}]");
    ASSERT_NE(property, nullptr);
    auto *token = static_cast<AppSpawnMsgAccessToken *>(GetAppSpawnMsgInfo(property->message, TLV_ACCESS_TOKEN_INFO));
    ASSERT_NE(token, nullptr);
    token->accessTokenIdEx = 0x12345678ULL;
    ASSERT_EQ(SubspawnPreload(property->message), 0);
    EXPECT_FALSE(GetSubspawnPreloadSystemAppForTest());
}

HWTEST_F(SubspawnAdapterTest, PreloadRejectsMissingToken, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnPropertyWithPreload(helper,
        "[{\"ns\":\"default\",\"libPath\":[\"/lib\"],\"so\":[]}]");
    ASSERT_NE(property, nullptr);
    property->message->tlvOffset[TLV_ACCESS_TOKEN_INFO] = INVALID_OFFSET;
    EXPECT_EQ(SubspawnPreload(property->message), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
}

/**
 * @tc.name: Subspawn_LoadInstanceMessage_001
 * @tc.desc: 从 memfd 恢复消息成功
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_LoadInstanceMessage_001, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    int fd = CreateMessageFd(property->message);
    ASSERT_GE(fd, 0);
    SubspawnStartInfo start = {};
    start.memFd = fd;
    start.messageLen = property->message->msgHeader.msgLen;
    AppSpawnMsgNode *message = nullptr;
    ASSERT_EQ(LoadInstanceMessage(&start, &message), 0);
    ASSERT_NE(message, nullptr);
    DeleteAppSpawnMsg(&message);
    close(fd);
}

/**
 * @tc.name: Subspawn_LoadInstanceMessage_002
 * @tc.desc: memfd 为空返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_LoadInstanceMessage_002, TestSize.Level0)
{
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    SubspawnStartInfo start = {};
    start.memFd = fds[0];
    start.messageLen = 64;
    AppSpawnMsgNode *message = nullptr;
    ASSERT_EQ(LoadInstanceMessage(&start, &message), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    close(fds[0]);
    close(fds[1]);
}

/**
 * @tc.name: Subspawn_LoadInstanceMessage_003
 * @tc.desc: 内容非法返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_LoadInstanceMessage_003, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    uint32_t length = property->message->msgHeader.msgLen;
    int fd = memfd_create("subspawn-bad-message", MFD_ALLOW_SEALING);
    ASSERT_GE(fd, 0);
    std::vector<uint8_t> zeros(length, 0);
    ASSERT_EQ(write(fd, zeros.data(), zeros.size()), (ssize_t)zeros.size());
    SubspawnStartInfo start = {};
    start.memFd = fd;
    start.messageLen = length;
    AppSpawnMsgNode *message = nullptr;
    ASSERT_EQ(LoadInstanceMessage(&start, &message), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    close(fd);
}

/**
 * @tc.name: Subspawn_RunChildProcessor_001
 * @tc.desc: 入参为空返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_RunChildProcessor_001, TestSize.Level0)
{
    ASSERT_EQ(SubspawnRunChildProcessor(nullptr, nullptr), -1);
}

/**
 * @tc.name: Subspawn_PrepareInstanceChild_006
 * @tc.desc: 消息 memfd 创建失败返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PrepareInstanceChild_006, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    property->forkCtx.fd[1] = fds[1];
    g_subspawnMock.memfdFail = true;
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    close(fds[0]);
    close(fds[1]);
}

/**
 * @tc.name: Subspawn_PrepareInstanceChild_007
 * @tc.desc: 描述符 CLOEXEC 设置失败返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_PrepareInstanceChild_007, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    property->forkCtx.fd[1] = fds[1];
    g_subspawnMock.fcntlFail = true;
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    close(fds[0]);
    close(fds[1]);
}

/**
 * @tc.name: Subspawn_BuildFdInfoMap_001
 * @tc.desc: 消息为空返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BuildFdInfoMap_001, TestSize.Level0)
{
    std::map<std::string, int> fdMap;
    ASSERT_EQ(BuildFdInfoMap(nullptr, fdMap, 0), -1);
}

/**
 * @tc.name: Subspawn_BuildFdInfoMap_002
 * @tc.desc: 消息无 TLV 时正常返回
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BuildFdInfoMap_002, TestSize.Level0)
{
    AppSpawnMsgNode message = {};
    std::vector<uint8_t> buffer(64, 0);
    std::vector<uint32_t> offsets(TLV_MAX, INVALID_OFFSET);
    message.buffer = buffer.data();
    message.tlvOffset = offsets.data();
    message.tlvCount = 0;
    std::map<std::string, int> fdMap;
    ASSERT_EQ(BuildFdInfoMap(&message, fdMap, 1), 0);
}

/**
 * @tc.name: Subspawn_BuildFdInfoMap_003
 * @tc.desc: 非冷启动且无连接上下文返回失败
 * @tc.type: FUNC
 */
HWTEST_F(SubspawnAdapterTest, Subspawn_BuildFdInfoMap_003, TestSize.Level0)
{
    AppSpawnMsgNode message = {};
    std::vector<uint8_t> buffer(64, 0);
    std::vector<uint32_t> offsets(TLV_MAX, INVALID_OFFSET);
    message.buffer = buffer.data();
    message.tlvOffset = offsets.data();
    message.tlvCount = 0;
    std::map<std::string, int> fdMap;
    ASSERT_EQ(BuildFdInfoMap(&message, fdMap, 0), -1);
}
// 构造仅供 BuildFdInfoMap 使用的扩展 TLV，不依赖真实 socket 或环境中的 FD。
struct FdMapMessage {
    AppSpawnMsgNode message = {};
    AppSpawnConnection connection = {};
    std::vector<uint8_t> buffer;
    std::vector<uint32_t> offsets;

    explicit FdMapMessage(const std::vector<std::string> &keys)
        : buffer(keys.size() * (sizeof(AppSpawnTlvExt) + TEST_FD_KEY_MAX_LEN)),
          offsets(TLV_MAX + keys.size(), INVALID_OFFSET)
    {
        for (size_t i = 0; i < keys.size(); ++i) {
            offsets[TLV_MAX + i] = i * (sizeof(AppSpawnTlvExt) + TEST_FD_KEY_MAX_LEN);
            auto *tlv = reinterpret_cast<AppSpawnTlvExt *>(buffer.data() + offsets[TLV_MAX + i]);
            tlv->tlvType = TLV_MAX;
            EXPECT_EQ(strcpy_s(tlv->tlvName, sizeof(tlv->tlvName), MSG_EXT_NAME_APP_FD), EOK);
            EXPECT_EQ(strcpy_s(reinterpret_cast<char *>(tlv + 1), TEST_FD_KEY_MAX_LEN, keys[i].c_str()), EOK);
        }
        message.buffer = buffer.data();
        message.tlvOffset = offsets.data();
        message.tlvCount = keys.size();
        message.connection = &connection;
        connection.receiverCtx.fdCount = keys.size();
        for (size_t i = 0; i < keys.size(); ++i) {
            connection.receiverCtx.fds[i] = TEST_FD_BASE + i;
        }
    }
};

static void SetMatchingIdentity()
{
    g_subspawnMock.uidOverride = TEST_APP_UID;
    g_subspawnMock.gidOverride = TEST_APP_UID;
}

static bool PrepareStartMessage(const AppSpawningCtx *property)
{
    SetMatchingIdentity();
    g_startInfo.memFd = CreateMessageFd(property->message);
    g_startInfo.messageLen = property->message->msgHeader.msgLen;
    return g_startInfo.memFd >= 0;
}

static void CheckBootstrapHandshake()
{
    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    ASSERT_EQ(__real_fcntl(ready[0], F_SETFL, O_NONBLOCK), 0);
    AppSpawnMsgNode message = {};
    message.msgHeader.msgLen = sizeof(AppSpawnMsg);
    const int memFd = CreateMessageFd(&message);
    ASSERT_GE(memFd, 0);
    const int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(listenFd, 0);
    g_subspawnMock.bootstrapListenFd = listenFd;
    g_subspawnMock.uidOverride = TEST_APP_UID;

    char program[] = "subspawn";
    char modeOption[] = "-mode";
    char mode[] = "subspawn";
    char fdOption[] = "-subspawn-fds";
    std::string arg = std::to_string(ready[1]) + ":" + std::to_string(memFd) + ":" +
        std::to_string(listenFd) + ":" + std::to_string(message.msgHeader.msgLen) + ":" + std::to_string(TEST_APP_UID);
    char *argv[] = {program, modeOption, mode, fdOption, &arg[0], nullptr};
    constexpr int ARG_COUNT = static_cast<int>(sizeof(argv) / sizeof(argv[0])) - 1;
    ASSERT_EQ(SubspawnEarlyParse(ARG_COUNT, argv), 0);
    const SubspawnStartInfo *start = GetSubspawnStartInfo();
    EXPECT_EQ(start->pipeFd, ready[1]);
    EXPECT_EQ(start->memFd, memFd);
    EXPECT_EQ(start->listenFd, listenFd);
    EXPECT_EQ(start->uid, TEST_APP_UID);
    EXPECT_EQ(start->messageLen, message.msgHeader.msgLen);

    // 发送非零启动结果，核对原值透传以及写端关闭。
    g_subspawnMock.writeFd = ready[1];
    g_subspawnMock.writeEintrTimes = 1;
    SubspawnSetStartResult(APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    SubspawnNotifyReady(SubspawnGetStartResult());
    EXPECT_EQ(g_subspawnMock.writeEintrTimes, 0);
    int result = 0;
    ASSERT_EQ(read(ready[0], &result, sizeof(result)), static_cast<ssize_t>(sizeof(result)));
    EXPECT_EQ(result, APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(start->pipeFd, -1);
    EXPECT_EQ(read(ready[0], &result, sizeof(result)), 0);
}

HWTEST_F(SubspawnAdapterTest, BootstrapParsesInheritedDescriptorsAndClosesReadyPipe, TestSize.Level0)
{
    // 启动信息是进程全局状态，在子进程内验证以隔离其他用例。
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        CheckBootstrapHandshake();
        _exit(HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    ASSERT_EQ(waited, pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
}

#ifdef WITH_SECCOMP
HWTEST_F(SubspawnAdapterTest, SubspawnCreateInstallsSeccompAfterSettingNoNewPrivs, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);

    const char *previousPolicy = GetLastSeccompPolicyName();
    g_subspawnMock.setNoNewPrivsFail = true;
    EXPECT_EQ(SetSeccompFilter(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.setNoNewPrivsCalls, 1);
    EXPECT_EQ(GetLastSeccompPolicyName(), previousPolicy);

    g_subspawnMock.setNoNewPrivsFail = false;
    EXPECT_EQ(SetSeccompFilter(&mgr, property), 0);
    EXPECT_EQ(g_subspawnMock.setNoNewPrivsCalls, 2);
    EXPECT_STREQ(GetLastSeccompPolicyName(), SUBSPAWN_NAME);
}
#endif

HWTEST_F(SubspawnAdapterTest, SubspawnEncapsPropagatesDeviceErrors, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    auto *create = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    auto *spawn = CreateSubspawnProperty(helper, MSG_APP_SPAWN);
    ASSERT_NE(create, nullptr);
    ASSERT_NE(spawn, nullptr);
    UpdateOpenFunc(MockEncapsOpen);
    g_subspawnMock.encapsOpenFail = true;
    EXPECT_EQ(SpawnSetEncapsPermissions(&mgr, create), 0);
    EXPECT_EQ(SpawnSetEncapsPermissions(&mgr, spawn), 0);
    mgr.content.mode = MODE_FOR_SUB_SPAWN;
    EXPECT_EQ(SpawnSetEncapsPermissions(&mgr, spawn), 0);
    EXPECT_EQ(g_subspawnMock.encapsOpenCalls, 3);

    g_subspawnMock.encapsOpenFail = false;
    SetIoctlResult(-1);
    EXPECT_EQ(SpawnSetEncapsPermissions(&mgr, spawn), 0);
    ASSERT_GE(g_subspawnMock.encapsFd, 0);
    EXPECT_EQ(__real_fcntl(g_subspawnMock.encapsFd, F_GETFD, 0), -1);
    EXPECT_EQ(errno, EBADF);
}

HWTEST_F(SubspawnAdapterTest, WriteAllHandlesShortWritesAndZeroWrite, TestSize.Level0)
{
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    g_subspawnMock.writeFd = fds[1];
    g_subspawnMock.writeLimit = 2;
    const char payload[] = "short-write";
    ASSERT_EQ(WriteAll(fds[1], payload, sizeof(payload)), 0);
    char received[sizeof(payload)] = {};
    ASSERT_EQ(read(fds[0], received, sizeof(received)), static_cast<ssize_t>(sizeof(payload)));
    EXPECT_STREQ(received, payload);
    EXPECT_GT(g_subspawnMock.writeCalls, 1);
    g_subspawnMock.writeZero = true;
    EXPECT_EQ(WriteAll(fds[1], payload, sizeof(payload)), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(WriteAll(fds[1], payload, 0), 0);
}

HWTEST_F(SubspawnAdapterTest, MessageFdWriteFailuresCloseDescriptor, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    size_t before = g_ownedFds.size();
    for (int failAt : {1, 2}) {
        SCOPED_TRACE(failAt);
        g_subspawnMock.writeFailAt = failAt;
        EXPECT_EQ(CreateMessageFd(property->message), -1);
        EXPECT_EQ(g_ownedFds.size(), before);
    }
}

HWTEST_F(SubspawnAdapterTest, MessageFdContainsSealedMessageAtOffsetZero, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    int fd = CreateMessageFd(property->message);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(__real_lseek(fd, 0, SEEK_CUR), 0);
    EXPECT_EQ(__real_fcntl(fd, F_GET_SEALS, 0), F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    AppSpawnMsg header = {};
    ASSERT_EQ(read(fd, &header, sizeof(header)), static_cast<ssize_t>(sizeof(header)));
    EXPECT_EQ(header.msgLen, property->message->msgHeader.msgLen);
    EXPECT_EQ(header.msgType, MSG_SUBSPAWN_CREATE);
}

HWTEST_F(SubspawnAdapterTest, InheritedFdsPreserveBootstrapAndMarkOtherDescriptors, TestSize.Level0)
{
    int fds[4];
    for (int &fd : fds) {
        fd = TrackFd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        ASSERT_GE(fd, 0);
    }
    ASSERT_EQ(SetInheritedFdsCloseOnExec(fds, SUBSPAWN_FD_COUNT), 0);
    bool markedOther = false;
    for (const auto &call : g_subspawnMock.fdFlags) {
        EXPECT_NE(call.first, fds[0]);
        EXPECT_NE(call.first, fds[1]);
        EXPECT_NE(call.first, fds[2]);
        if (call.first == fds[3]) {
            markedOther = true;
            EXPECT_NE(call.second & FD_CLOEXEC, 0UL);
        }
    }
    EXPECT_TRUE(markedOther);
}

HWTEST_F(SubspawnAdapterTest, InheritedFdsRejectSetFlagsAndDirectoryErrors, TestSize.Level0)
{
    int fd = TrackFd(open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_GE(fd, 0);
    g_subspawnMock.fcntlFailCommand = F_SETFD;
    g_subspawnMock.fcntlFailFd = fd;
    EXPECT_EQ(SetInheritedFdsCloseOnExec(nullptr, 0), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.fcntlFailCommand = -1;
    g_subspawnMock.readdirFail = true;
    EXPECT_EQ(SetInheritedFdsCloseOnExec(nullptr, 0), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.readdirFail = false;
    g_subspawnMock.closedirFail = true;
    EXPECT_EQ(SetInheritedFdsCloseOnExec(nullptr, 0), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

HWTEST_F(SubspawnAdapterTest, PrepareRejectsDirectoryAndSocketFailures, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    property->forkCtx.fd[1] = fds[1];
    g_subspawnMock.mkdirFail = true;
    EXPECT_EQ(PrepareInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.mkdirFail = false;
    g_subspawnMock.socketFail = true;
    EXPECT_EQ(PrepareInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
}

HWTEST_F(SubspawnAdapterTest, PrepareClearsCloexecOnlyForBootstrapDescriptors, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    property->forkCtx.fd[1] = fds[1];
    g_subspawnMock.fcntlFlags = FD_CLOEXEC;
    ASSERT_EQ(PrepareInstanceChild(&mgr, property), 0);
    ASSERT_EQ(g_subspawnMock.fdFlags.size(), 3U);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(g_subspawnMock.fdFlags[i].first, property->subspawnFds[i]);
        EXPECT_EQ(g_subspawnMock.fdFlags[i].second & FD_CLOEXEC, 0UL);
    }
}

HWTEST_F(SubspawnAdapterTest, ExecRejectsEffectiveIdentityAndPdeathFailures, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    SetMatchingIdentity();
    g_subspawnMock.euidOverride = 20010030;
    EXPECT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.euidOverride = -1;
    g_subspawnMock.gidOverride = 20010030;
    EXPECT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.gidOverride = TEST_APP_UID;
    g_subspawnMock.egidOverride = 20010030;
    EXPECT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.egidOverride = -1;
    g_subspawnMock.pdeathFail = true;
    EXPECT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.execCalls, 0);
}

HWTEST_F(SubspawnAdapterTest, ExecStopsBeforeExecWhenFdSweepFails, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    SetMatchingIdentity();
    g_subspawnMock.opendirFail = true;
    EXPECT_EQ(ExecInstanceChild(&mgr, property), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.execCalls, 0);
}

HWTEST_F(SubspawnAdapterTest, ValidateRejectsMalformedCreateMessage, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE, false);
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(ValidateInstanceSpawn(&mgr, property), APPSPAWN_SUBSPAWN_INSTANCE_INVALID);
}

HWTEST_F(SubspawnAdapterTest, CleanupSkipsUnstartedOrMissingDacAndInstanceChildren, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(InstanceCreateAborted(&mgr, property), 0);
    AppSpawningCtx *missingDac = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE, false);
    ASSERT_NE(missingDac, nullptr);
    missingDac->pid = 42;
    EXPECT_EQ(InstanceCreateAborted(&mgr, missingDac), 0);
    mgr.content.mode = MODE_FOR_SUB_SPAWN;
    AppSpawnedProcessInfo info = {};
    info.isSubspawn = true;
    EXPECT_EQ(InstanceDied(&mgr, &info), 0);
    EXPECT_EQ(g_subspawnMock.unlinkCount, 0);
}

HWTEST_F(SubspawnAdapterTest, LoadMessageRetriesInterruptedAndPartialReads, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(PrepareStartMessage(property));
    g_subspawnMock.preadEintrTimes = 1;
    g_subspawnMock.preadLimit = 7;
    AppSpawnMsgNode *message = nullptr;
    int result = LoadInstanceMessage(&g_startInfo, &message);
    EXPECT_EQ(result, 0);
    if (message != nullptr) {
        EXPECT_EQ(message->msgHeader.msgLen, property->message->msgHeader.msgLen);
        EXPECT_EQ(message->msgHeader.msgType, MSG_SUBSPAWN_CREATE);
    }
    DeleteAppSpawnMsg(&message);
}

HWTEST_F(SubspawnAdapterTest, LoadMessageRejectsTruncationAndAllocationFailure, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(PrepareStartMessage(property));
    ++g_startInfo.messageLen;
    AppSpawnMsgNode *message = nullptr;
    EXPECT_EQ(LoadInstanceMessage(&g_startInfo, &message), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(message, nullptr);
    UpdateMallocFunc([](size_t) -> void * { return nullptr; });
    int result = LoadInstanceMessage(&g_startInfo, &message);
    UpdateMallocFunc(nullptr);
    EXPECT_EQ(result, APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(message, nullptr);
}

HWTEST_F(SubspawnAdapterTest, SecurityRejectsUidAndNoNewPrivsMismatch, TestSize.Level0)
{
    SetMatchingIdentity();
    g_subspawnMock.uidOverride = 20010030;
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.uidOverride = TEST_APP_UID;
    g_subspawnMock.euidOverride = 20010030;
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.euidOverride = -1;
    for (int value : {0, -1}) {
        g_subspawnMock.noNewPrivs = value;
        EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    }
}

HWTEST_F(SubspawnAdapterTest, SecurityRejectsCapabilityQueryAndEverySetMismatch, TestSize.Level0)
{
    SetMatchingIdentity();
    g_subspawnMock.capgetFail = true;
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.capgetFail = false;
    uint32_t required = (1U << CAP_SETUID) | (1U << CAP_SETGID) | (1U << CAP_KILL);
    uint32_t *low[] = {&g_subspawnMock.caps[0].permitted, &g_subspawnMock.caps[0].effective,
        &g_subspawnMock.caps[0].inheritable};
    for (uint32_t *word : low) {
        for (uint32_t value : {0U, required & ~(1U << CAP_KILL), required | (1U << CAP_SETPCAP)}) {
            *word = value;
            EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
        }
        *word = required;
    }
    uint32_t *high[] = {&g_subspawnMock.caps[1].permitted, &g_subspawnMock.caps[1].effective,
        &g_subspawnMock.caps[1].inheritable};
    for (uint32_t *word : high) {
        *word = 1;
        EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
        *word = 0;
    }
}

HWTEST_F(SubspawnAdapterTest, SecurityAcceptsOnlyExpectedDomain, TestSize.Level0)
{
    SetMatchingIdentity();
#ifdef WITH_SELINUX
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), 0);
    EXPECT_EQ(g_subspawnMock.freeconCalls, 1);
    g_subspawnMock.getconFail = true;
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.getconFail = false;
    g_subspawnMock.nullContext = true;
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    g_subspawnMock.nullContext = false;
    g_subspawnMock.context = "u:r:appspawn:s0";
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.freeconCalls, 3);
#else
    EXPECT_EQ(CheckInstanceSecurity(&g_startInfo), APPSPAWN_SUBSPAWN_PRESET_FAILED);
#endif
}

HWTEST_F(SubspawnAdapterTest, InstanceInitSkipsOtherModesAndReportsSecurityFailure, TestSize.Level0)
{
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_APP_SPAWN);
    EXPECT_EQ(SubspawnInstanceInit(&mgr), 0);
    EXPECT_EQ(g_subspawnMock.startResultCalls, 0);
    mgr.content.mode = MODE_FOR_SUB_SPAWN;
    SetMatchingIdentity();
    g_subspawnMock.noNewPrivs = 0;
    EXPECT_EQ(SubspawnInstanceInit(&mgr), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.startResult, APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.startResultCalls, 1);
    EXPECT_EQ(g_subspawnMock.registerCalls, 0);
}

#ifdef WITH_SELINUX
HWTEST_F(SubspawnAdapterTest, InstanceInitRestoresMessageAndRegistersChildLoop, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_SUB_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE);
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(PrepareStartMessage(property));
    EXPECT_EQ(SubspawnInstanceInit(&mgr), 0);
    EXPECT_EQ(g_subspawnMock.startResult, 0);
    EXPECT_EQ(g_subspawnMock.startResultCalls, 1);
    EXPECT_EQ(g_subspawnMock.registerCalls, 1);
    EXPECT_EQ(g_subspawnMock.childLoop, SubspawnRunChildProcessor);
    EXPECT_EQ(__real_fcntl(g_startInfo.memFd, F_GETFD, 0), -1);
    EXPECT_EQ(errno, EBADF);
}

HWTEST_F(SubspawnAdapterTest, InstanceInitRejectsMissingIdentityAndGidMismatch, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_SUB_SPAWN);
    for (int fault = 0; fault < 4; ++fault) {
        SCOPED_TRACE(fault);
        AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_SUBSPAWN_CREATE, fault != 0, fault != 1);
        ASSERT_NE(property, nullptr);
        ASSERT_TRUE(PrepareStartMessage(property));
        g_subspawnMock.egidOverride = -1;
        if (fault == 2) {
            g_subspawnMock.gidOverride = 20010030;
        } else if (fault == 3) {
            g_subspawnMock.egidOverride = 20010030;
        }
        EXPECT_EQ(SubspawnInstanceInit(&mgr), APPSPAWN_SUBSPAWN_PRESET_FAILED);
        EXPECT_EQ(g_subspawnMock.registerCalls, 0);
    }
}

HWTEST_F(SubspawnAdapterTest, InstanceInitReportsDecodeAndPreloadFailures, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_SUB_SPAWN);
    SetMatchingIdentity();
    // 非零长度确保无效 memFd 命中读取失败，不依赖 malloc(0) 及空消息的解码行为。
    g_startInfo.messageLen = sizeof(AppSpawnMsg);
    EXPECT_EQ(SubspawnInstanceInit(&mgr), APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.startResult, APPSPAWN_SUBSPAWN_PRESET_FAILED);
    EXPECT_EQ(g_subspawnMock.startResultCalls, 1);
    AppSpawningCtx *property = CreateSubspawnPropertyWithPreload(helper, "invalid-json");
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(PrepareStartMessage(property));
    EXPECT_EQ(SubspawnInstanceInit(&mgr), APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(g_subspawnMock.startResult, APPSPAWN_SUBSPAWN_PRELOAD_FAILED);
    EXPECT_EQ(g_subspawnMock.registerCalls, 0);
}
#endif

HWTEST_F(SubspawnAdapterTest, FdMapTransfersNamedDescriptorsInOrder, TestSize.Level0)
{
    FdMapMessage input({"input", "output"});
    std::map<std::string, int> result;
    ASSERT_EQ(BuildFdInfoMap(&input.message, result, 0), 0);
    EXPECT_EQ(result, (std::map<std::string, int>{{"input", TEST_FD_BASE}, {"output", TEST_FD_BASE + 1}}));
}

HWTEST_F(SubspawnAdapterTest, FdMapRejectsMissingStorageInvalidOffsetAndInvalidFd, TestSize.Level0)
{
    FdMapMessage input({"input"});
    std::map<std::string, int> result;
    input.message.buffer = nullptr;
    EXPECT_EQ(BuildFdInfoMap(&input.message, result, 0), -1);
    input.message.buffer = input.buffer.data();
    input.message.tlvOffset = nullptr;
    EXPECT_EQ(BuildFdInfoMap(&input.message, result, 0), -1);
    input.message.tlvOffset = input.offsets.data();
    input.offsets[TLV_MAX] = INVALID_OFFSET;
    EXPECT_EQ(BuildFdInfoMap(&input.message, result, 0), -1);
    input.offsets[TLV_MAX] = 0;
    input.connection.receiverCtx.fds[0] = -1;
    EXPECT_EQ(BuildFdInfoMap(&input.message, result, 0), -1);
    input.connection.receiverCtx.fdCount = 0;
    EXPECT_EQ(BuildFdInfoMap(&input.message, result, 0), 0);
    EXPECT_TRUE(result.empty());
}

HWTEST_F(SubspawnAdapterTest, FdMapSkipsUnrelatedTlvs, TestSize.Level0)
{
    FdMapMessage input({"wrong-type", "wrong-name", "input"});
    auto *first = reinterpret_cast<AppSpawnTlv *>(input.buffer.data());
    first->tlvType = TLV_DAC_INFO;
    auto *second = reinterpret_cast<AppSpawnTlvExt *>(input.buffer.data() + input.offsets[TLV_MAX + 1]);
    ASSERT_EQ(strcpy_s(second->tlvName, sizeof(second->tlvName), "unrelated"), 0);
    std::map<std::string, int> result;
    ASSERT_EQ(BuildFdInfoMap(&input.message, result, 0), 0);
    EXPECT_EQ(result, (std::map<std::string, int>{{"input", TEST_FD_BASE}}));
}

HWTEST_F(SubspawnAdapterTest, ColdFdMapValidatesEnvironmentAndRestoresValidDescriptors, TestSize.Level0)
{
    FdMapMessage input({"input", "output"});
    std::string firstKey = std::string(APP_FDENV_PREFIX) + "input";
    std::string secondKey = std::string(APP_FDENV_PREFIX) + "output";
    SaveEnvironment(firstKey);
    SaveEnvironment(secondKey);
    unsetenv(firstKey.c_str());
    setenv(secondKey.c_str(), "52", 1);
    std::map<std::string, int> result;
    EXPECT_EQ(BuildFdInfoMap(&input.message, result, 1), 0);
    EXPECT_EQ(result, (std::map<std::string, int>{{"output", 52}}));
    for (const char *value : {"", "abc", "0", "-1", "7x", "51"}) {
        SCOPED_TRACE(value);
        setenv(firstKey.c_str(), value, 1);
        result.clear();
        ASSERT_EQ(BuildFdInfoMap(&input.message, result, 1), 0);
        EXPECT_EQ(result.count("input"), std::string(value) == "51" ? 1U : 0U);
        EXPECT_EQ(result.at("output"), 52);
    }
    EXPECT_EQ(result.at("input"), 51);
}

HWTEST_F(SubspawnAdapterTest, ChildProcessorStartsBusinessChildWithMappedFds, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_SUB_SPAWN);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_APP_SPAWN, true, true, true);
    ASSERT_NE(property, nullptr);
    AppSpawnConnection connection = {};
    connection.receiverCtx.fdCount = 1;
    connection.receiverCtx.fds[0] = 42;
    property->message->connection = &connection;
    for (bool enabled : {true, false}) {
        g_subspawnMock.checkExit = enabled;
        ASSERT_EQ(SubspawnRunChildProcessor(&mgr.content, &property->client), 0);
        EXPECT_EQ(g_subspawnMock.childFds, (std::map<std::string, int>{{"input", 42}}));
        EXPECT_EQ(g_subspawnMock.checkExitAtStart, enabled ? std::to_string(getpid()) : "");
        EXPECT_EQ(getenv(APPSPAWN_CHECK_EXIT), nullptr);
    }
    EXPECT_EQ(g_subspawnMock.envClearCalls, 2);
    EXPECT_EQ(g_subspawnMock.startChildCalls, 2);
    property->message->connection = nullptr;
}

HWTEST_F(SubspawnAdapterTest, ChildProcessorRejectsNullClientAndSkipsUnsupportedMethod, TestSize.Level0)
{
    AppSpawnTestHelper helper;
    AppSpawnMgr mgr;
    InitSubspawnMgr(mgr, MODE_FOR_SUB_SPAWN);
    EXPECT_EQ(SubspawnRunChildProcessor(&mgr.content, nullptr), -1);
    AppSpawningCtx *property = CreateSubspawnProperty(helper, MSG_APP_SPAWN);
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(SubspawnRunChildProcessor(&mgr.content, &property->client), 0);
    EXPECT_EQ(g_subspawnMock.startChildCalls, 0);
    EXPECT_EQ(g_subspawnMock.envClearCalls, 0);
    EXPECT_EQ(getenv(APPSPAWN_CHECK_EXIT), nullptr);
}
}  // namespace OHOS
