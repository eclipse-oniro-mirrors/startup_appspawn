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
#include "hnp_installer_test.h"
#include <gtest/gtest.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include "hnp_installer.h"
#include "securec.h"


using namespace testing;
using namespace testing::ext;


#ifdef __cplusplus
    extern "C" {
#endif

#ifdef __cplusplus
    }
#endif

namespace OHOS {
class HnpInstallerSingleTest : public testing::Test {
public:
    static void SetUpTestCase();
    static void TearDownTestCase();
    void SetUp();
    void TearDown();
};

void HnpInstallerSingleTest::SetUpTestCase()
{
    GTEST_LOG_(INFO) << "Hnp_Installer_TEST SetUpTestCase";
}

void HnpInstallerSingleTest::TearDownTestCase()
{
    GTEST_LOG_(INFO) << "Hnp_Installer_TEST TearDownTestCase";
}

void HnpInstallerSingleTest::SetUp()
{
    GTEST_LOG_(INFO) << "Hnp_Installer_TEST SetUp";
}

void HnpInstallerSingleTest::TearDown()
{
    GTEST_LOG_(INFO) << "Hnp_Installer_TEST TearDown";
}

static void MakeDirRecursive(const std::string &path, mode_t mode)
{
    size_t size = path.size();
    if (size == 0) {
        return;
    }

    size_t index = 0;
    do {
        size_t pathIndex = path.find_first_of('/', index);
        index = pathIndex == std::string::npos ? size : pathIndex + 1;
        std::string dir = path.substr(0, index);
        if (access(dir.c_str(), F_OK) < 0) {
            int ret = mkdir(dir.c_str(), mode);
            if (ret != 0) {
                return;
            }
        }
    } while (index < size);
}

static void CreateTestFile(const char *fileName, const char *data)
{
    char dir[PATH_MAX] = {0};
    if (strncpy_s(dir, sizeof(dir), fileName, strrchr(fileName, '/') - fileName) != EOK) {
        return;
    }
    MakeDirRecursive(dir, S_IRWXU | S_IXGRP | S_IXOTH);  // rwx--x--x
    FILE *tmpFile = fopen(fileName, "wr");
    if (tmpFile != nullptr) {
        fprintf(tmpFile, "%s", data);
        (void)fflush(tmpFile);
        fclose(tmpFile);
    }
}

HWTEST_F(HnpInstallerSingleTest, hnp_clearSoftLink_001, TestSize.Level0)
{
    GTEST_LOG_(INFO) << "hnp_clearSoftLink_001 start";

    const std::string binDir = HNP_DEFAULT_INSTALL_ROOT_PATH"/100/hnppublic/bin";
    const std::string fileDir = HNP_DEFAULT_INSTALL_ROOT_PATH"/100/hnppublic/test.org/bin";
    
    MakeDirRecursive(binDir, S_IRWXU | S_IXGRP | S_IXOTH);  // rwx--x--x
    MakeDirRecursive(fileDir, S_IRWXU | S_IXGRP | S_IXOTH);  // rwx--x--x

    const std::string lnkFile = HNP_DEFAULT_INSTALL_ROOT_PATH"/100/hnppublic/bin/test";
    const std::string sourceFile = "../test.org/bin/test";
    
    CreateTestFile(HNP_DEFAULT_INSTALL_ROOT_PATH"/100/hnppublic/test.org/bin/test", "test");
    symlink(sourceFile.c_str(), lnkFile.c_str());

    ClearSoftLink(100);
    int ret = access(lnkFile.c_str(), F_OK);
    EXPECT_EQ(ret, 0);

    remove(HNP_DEFAULT_INSTALL_ROOT_PATH"/100/hnppublic/test.org/bin/test");
    ClearSoftLink(100);

    ret = access(lnkFile.c_str(), F_OK);
    EXPECT_NE(ret, 0);
}

// ==================== HandleSignParameter tests ====================

/**
 * @brief HandleSignParameter with NULL signHnp
 *        Covers: signHnp == NULL -> return HNP_ERRNO_PARAM_INVALID
 */
HWTEST_F(HnpInstallerSingleTest, HandleSignParameter_NullSignHnp_001, TestSize.Level0)
{
    HapInstallInfo installInfo = {};
    installInfo.hnpRootPath = const_cast<char *>(HNP_DEFAULT_INSTALL_ROOT_PATH "/test");
    installInfo.signHnpSize = 0;
    installInfo.signHnpPaths = nullptr;

    int ret = HandleSignParameter(&installInfo, nullptr);
    EXPECT_NE(ret, 0);
}

/**
 * @brief HandleSignParameter with NULL installInfo
 *        Covers: installInfo == NULL -> return HNP_ERRNO_PARAM_INVALID
 */
HWTEST_F(HnpInstallerSingleTest, HandleSignParameter_NullInstallInfo_001, TestSize.Level0)
{
    int ret = HandleSignParameter(nullptr, "test.hnp");
    EXPECT_NE(ret, 0);
}

/**
 * @brief HandleSignParameter with path containing ".."
 *        Covers: strstr(signHnp, "..") != NULL -> return HNP_ERRNO_PARAM_INVALID
 */
HWTEST_F(HnpInstallerSingleTest, HandleSignParameter_DotDot_001, TestSize.Level0)
{
    HapInstallInfo installInfo = {};
    installInfo.hnpRootPath = const_cast<char *>(HNP_DEFAULT_INSTALL_ROOT_PATH "/test");
    installInfo.signHnpSize = 0;
    installInfo.signHnpPaths = nullptr;

    int ret = HandleSignParameter(&installInfo, "../test.hnp");
    EXPECT_NE(ret, 0);
}

/**
 * @brief HandleSignParameter with non-existent file
 *        Covers: realpath returns NULL -> return HNP_ERRNO_PARAM_INVALID
 */
HWTEST_F(HnpInstallerSingleTest, HandleSignParameter_NonExistFile_001, TestSize.Level0)
{
    HapInstallInfo installInfo = {};
    installInfo.hnpRootPath = const_cast<char *>(HNP_DEFAULT_INSTALL_ROOT_PATH "/test");
    installInfo.signHnpSize = 0;
    installInfo.signHnpPaths = nullptr;

    int ret = HandleSignParameter(&installInfo, "nonexistent_file.hnp");
    EXPECT_NE(ret, 0);
}

/**
 * @brief HandleSignParameter with valid file
 *        Covers: realpath succeeds, signHnpPaths updated, signHnpSize incremented
 */
HWTEST_F(HnpInstallerSingleTest, HandleSignParameter_Valid_001, TestSize.Level0)
{
    const char *testFile = HNP_DEFAULT_INSTALL_ROOT_PATH "/test/hnp_test_sign_valid.hnp";
    CreateTestFile(testFile, "test");

    HapInstallInfo installInfo = {};
    installInfo.hnpRootPath = const_cast<char *>(HNP_DEFAULT_INSTALL_ROOT_PATH "/test");
    installInfo.signHnpSize = 0;
    installInfo.signHnpPaths = nullptr;

    int ret = HandleSignParameter(&installInfo, "hnp_test_sign_valid.hnp");
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(installInfo.signHnpSize, 1);
    EXPECT_NE(installInfo.signHnpPaths, nullptr);
    EXPECT_NE(installInfo.signHnpPaths[0], nullptr);

    if (installInfo.signHnpPaths != nullptr) {
        for (int i = 0; i < installInfo.signHnpSize; i++) {
            free(installInfo.signHnpPaths[i]);
        }
        free(installInfo.signHnpPaths);
    }
    remove(testFile);
}

// ==================== BssInstall tests ====================

/**
 * @brief BssInstall with count=0 (no exec files)
 *        Covers: BssInstall called but BIN_SEC_PATH not accessible -> return 0
 */
HWTEST_F(HnpInstallerSingleTest, BssInstall_NoExecFiles_001, TestSize.Level0)
{
    // Skip if the real code_sign SDK exists on device to avoid side effects
    if (access("/system/lib64/libsps_binary_security_sdk.z.so", F_OK) == 0 ||
        access("/system/lib/libsps_binary_security_sdk.z.so", F_OK) == 0) {
        GTEST_SKIP() << "BIN_SEC_PATH exists on this device, skip to avoid real SDK side effects";
    }
    HnpSignMapInfo signInfos[1] = {};
    signInfos[0].isExec = 0;
    strcpy_s(signInfos[0].key, MAX_FILE_PATH_LEN, "test_key");
    strcpy_s(signInfos[0].value, MAX_FILE_PATH_LEN, HNP_DEFAULT_INSTALL_ROOT_PATH "/test/test_value");

    HapInstallInfo installInfo = {};
    installInfo.hapPackageName = const_cast<char *>("com.test.app");
    installInfo.appIdentifier = const_cast<char *>("app_id_123");
    installInfo.uid = 100;

    int ret = BssInstall(signInfos, 1, &installInfo, 0);
    // BIN_SEC_PATH not accessible -> access() fails -> return 0
    EXPECT_EQ(ret, 0);
}

/**
 * @brief BssInstall with exec files but no BIN_SEC_PATH
 *        Covers: access(BIN_SEC_PATH) != 0 -> return 0
 */
HWTEST_F(HnpInstallerSingleTest, BssInstall_ExecFilesNoSo_001, TestSize.Level0)
{
    // Skip if the real code_sign SDK exists on device to avoid side effects
    if (access("/system/lib64/libsps_binary_security_sdk.z.so", F_OK) == 0 ||
        access("/system/lib/libsps_binary_security_sdk.z.so", F_OK) == 0) {
        GTEST_SKIP() << "BIN_SEC_PATH exists on this device, skip to avoid real SDK side effects";
    }
    HnpSignMapInfo signInfos[1] = {};
    signInfos[0].isExec = 1;
    strcpy_s(signInfos[0].key, MAX_FILE_PATH_LEN, "test_key");
    strcpy_s(signInfos[0].value, MAX_FILE_PATH_LEN, HNP_DEFAULT_INSTALL_ROOT_PATH "/test/test_value");

    HapInstallInfo installInfo = {};
    installInfo.hapPackageName = const_cast<char *>("com.test.app");
    installInfo.appIdentifier = const_cast<char *>("app_id_123");
    installInfo.uid = 100;

    int ret = BssInstall(signInfos, 1, &installInfo, 1);
    // BIN_SEC_PATH not accessible -> access() fails -> return 0
    EXPECT_EQ(ret, 0);
}

// ==================== CheckIsSignHnp tests ====================

/**
 * @brief CheckIsSignHnp with NULL hnpFile
 *        Covers: hnpFile == NULL -> return false
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_NullHnpFile_001, TestSize.Level0)
{
    HapInstallInfo installInfo = {};
    installInfo.signHnpSize = 0;
    installInfo.signHnpPaths = nullptr;

    bool ret = CheckIsSignHnp(nullptr, &installInfo);
    EXPECT_FALSE(ret);
}

/**
 * @brief CheckIsSignHnp with NULL installInfo
 *        Covers: installInfo == NULL -> return false
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_NullInstallInfo_001, TestSize.Level0)
{
    bool ret = CheckIsSignHnp(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/test.hnp", nullptr);
    EXPECT_FALSE(ret);
}

/**
 * @brief CheckIsSignHnp with NULL signHnpPaths
 *        Covers: installInfo->signHnpPaths == NULL -> return false
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_NullSignHnpPaths_001, TestSize.Level0)
{
    HapInstallInfo installInfo = {};
    installInfo.signHnpSize = 0;
    installInfo.signHnpPaths = nullptr;

    bool ret = CheckIsSignHnp(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/test.hnp", &installInfo);
    EXPECT_FALSE(ret);
}

/**
 * @brief CheckIsSignHnp with signHnpSize <= 0
 *        Covers: installInfo->signHnpSize <= 0 -> return false
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_ZeroSize_001, TestSize.Level0)
{
    HapInstallInfo installInfo = {};
    installInfo.signHnpSize = 0;
    installInfo.signHnpPaths = nullptr;

    bool ret = CheckIsSignHnp(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/test.hnp", &installInfo);
    EXPECT_FALSE(ret);
}

/**
 * @brief CheckIsSignHnp with non-existent hnpFile
 *        Covers: realpath returns NULL -> continue (skip)
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_NonExistFile_001, TestSize.Level0)
{
    char *signPaths[1] = {const_cast<char *>(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/hnp_sign_test_file.hnp")};
    HapInstallInfo installInfo = {};
    installInfo.signHnpSize = 1;
    installInfo.signHnpPaths = signPaths;

    bool ret = CheckIsSignHnp(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/nonexistent_file_for_sign.hnp", &installInfo);
    EXPECT_FALSE(ret);
}

/**
 * @brief CheckIsSignHnp with matching realpath -> isSign = true
 *        Covers: strcmp match -> isSign = true; break
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_Match_001, TestSize.Level0)
{
    const char *testFile = HNP_DEFAULT_INSTALL_ROOT_PATH "/test/hnp_checksign_match.hnp";
    CreateTestFile(testFile, "test");

    char realPath[PATH_MAX] = {0};
    realpath(testFile, realPath);

    char *signPaths[1] = {strdup(realPath)};
    HapInstallInfo installInfo = {};
    installInfo.signHnpSize = 1;
    installInfo.signHnpPaths = signPaths;

    bool ret = CheckIsSignHnp(testFile, &installInfo);
    EXPECT_TRUE(ret);

    free(signPaths[0]);
    remove(testFile);
}

/**
 * @brief CheckIsSignHnp with non-matching realpath -> isSign = false
 *        Covers: strcmp mismatch -> loop ends, isSign remains false
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_NoMatch_001, TestSize.Level0)
{
    const char *testFile = HNP_DEFAULT_INSTALL_ROOT_PATH "/test/hnp_checksign_nomatch.hnp";
    CreateTestFile(testFile, "test");

    char *signPaths[1] = {strdup(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/different_path.hnp")};
    HapInstallInfo installInfo = {};
    installInfo.signHnpSize = 1;
    installInfo.signHnpPaths = signPaths;

    bool ret = CheckIsSignHnp(testFile, &installInfo);
    EXPECT_FALSE(ret);

    free(signPaths[0]);
    remove(testFile);
}

/**
 * @brief CheckIsSignHnp with NULL entry in signHnpPaths
 *        Covers: installInfo->signHnpPaths[i] == NULL -> continue
 */
HWTEST_F(HnpInstallerSingleTest, CheckIsSignHnp_NullEntry_001, TestSize.Level0)
{
    const char *testFile = HNP_DEFAULT_INSTALL_ROOT_PATH "/test/hnp_checksign_nullentry.hnp";
    CreateTestFile(testFile, "test");

    char *signPaths[2] = {nullptr, strdup(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/other.hnp")};
    HapInstallInfo installInfo = {};
    installInfo.signHnpSize = 2;
    installInfo.signHnpPaths = signPaths;

    bool ret = CheckIsSignHnp(testFile, &installInfo);
    EXPECT_FALSE(ret);

    free(signPaths[1]);
    remove(testFile);
}

// ==================== HnpInstall tests ====================

/**
 * @brief HnpInstall with non-existent hnp file (HnpUnZip fails)
 *        Covers: HnpUnZip returns error -> return ret (line 224-227)
 *        This exercises the entry of HnpInstall before the sign assignment loop.
 */
HWTEST_F(HnpInstallerSingleTest, HnpInstall_UnzipFail_001, TestSize.Level0)
{
    HnpInstallInfo hnpInfo = {};
    HapInstallInfo hapInfo = {};
    hapInfo.uid = 100;
    hapInfo.hapPackageName = const_cast<char *>("com.test.app");
    hapInfo.signHnpPaths = nullptr;
    hapInfo.signHnpSize = 0;
    hnpInfo.hapInstallInfo = &hapInfo;
    hnpInfo.isPublic = false;
    sprintf_s(hnpInfo.hnpVersionPath, MAX_FILE_PATH_LEN, HNP_DEFAULT_INSTALL_ROOT_PATH "/test/hnp_install_test_ver");
    sprintf_s(hnpInfo.hnpSignKeyPrefix, MAX_FILE_PATH_LEN, "hnp/arm64/test.hnp");

    HnpCfgInfo hnpCfg = {};
    HnpSignMapInfo signInfos[2] = {};
    int count = 0;

    int ret = HnpInstall(HNP_DEFAULT_INSTALL_ROOT_PATH "/test/nonexistent.hnp", &hnpInfo, &hnpCfg, signInfos, &count);
    // HnpUnZip will fail because file doesn't exist
    EXPECT_NE(ret, 0);
}

}