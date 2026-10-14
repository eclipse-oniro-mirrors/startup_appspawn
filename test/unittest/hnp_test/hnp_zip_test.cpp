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

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "zlib.h"
#include "contrib/minizip/zip.h"

#include "hnp_base.h"
#include "securec.h"

using namespace testing;
using namespace testing::ext;

extern "C" void SetUnzGetInfoFail(int fail);

namespace OHOS {
class HnpZipTest : public testing::Test {
public:
    static void SetUpTestCase() {}

    static void TearDownTestCase() {}

    void SetUp() {}

    void TearDown() {}
};

static const char *TEST_ZIP_FILE = "./hnp_test_longname.zip";
static const char *TEST_UNZIP_DIR = "./hnp_test_unzip";

static int CreateZipWithEntryName(const char *zipPath, const char *fileName)
{
    zipFile zf = zipOpen(zipPath, APPEND_STATUS_CREATE);
    if (zf == nullptr) {
        return -1;
    }

    int zret = zipOpenNewFileInZip3(zf, fileName, nullptr, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED,
        Z_BEST_COMPRESSION, 0, -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, nullptr, 0);
    if (zret != ZIP_OK) {
        zipClose(zf, nullptr);
        return -1;
    }
    if (zipWriteInFileInZip(zf, "test", sizeof("test")) != ZIP_OK) {
        zipCloseFileInZip(zf);
        zipClose(zf, nullptr);
        return -1;
    }
    zipCloseFileInZip(zf);
    zipClose(zf, nullptr);
    return 0;
}

static int CreateLongNameZip(void)
{
    size_t nameLen = MAX_FILE_PATH_LEN;
    char *fileName = static_cast<char *>(malloc(nameLen + 1));
    if (fileName == nullptr) {
        return -1;
    }
    if (memset_s(fileName, nameLen + 1, 'a', nameLen) != EOK) {
        free(fileName);
        return -1;
    }
    fileName[nameLen] = '\0';

    int ret = CreateZipWithEntryName(TEST_ZIP_FILE, fileName);
    free(fileName);
    return ret;
}

static void RemoveTestArtifacts(void)
{
    if (remove(TEST_ZIP_FILE) != 0 && errno != ENOENT) {
        GTEST_LOG_(WARNING) << "remove " << TEST_ZIP_FILE << " failed";
    }
    if (HnpDeleteFolder(TEST_UNZIP_DIR) != 0) {
        GTEST_LOG_(WARNING) << "delete " << TEST_UNZIP_DIR << " failed";
    }
}

/**
* @tc.name: Hnp_Zip_001
* @tc.desc:  Verify HnpUnZip return error when zip entry name is too long.
* @tc.type: FUNC
* @tc.require:
*/
HWTEST_F(HnpZipTest, Hnp_Zip_001, TestSize.Level1)
{
    ASSERT_EQ(CreateLongNameZip(), 0);

    HnpSignMapInfo signMaps[1] = {};
    int count = 0;
    EXPECT_EQ(HnpUnZip(TEST_ZIP_FILE, TEST_UNZIP_DIR, "prefix", signMaps, &count),
        HNP_ERRNO_BASE_UNZIP_GET_INFO_FAILED);
    EXPECT_EQ(count, 0);
    RemoveTestArtifacts();
}

/**
* @tc.name: Hnp_Zip_002
* @tc.desc:  Verify HnpCfgGetFromZip return error when zip entry name is too long.
* @tc.type: FUNC
* @tc.require:
*/
HWTEST_F(HnpZipTest, Hnp_Zip_002, TestSize.Level1)
{
    ASSERT_EQ(CreateLongNameZip(), 0);

    HnpCfgInfo cfg = {};
    EXPECT_EQ(HnpCfgGetFromZip(TEST_ZIP_FILE, &cfg), HNP_ERRNO_BASE_UNZIP_GET_INFO_FAILED);
    RemoveTestArtifacts();
}

/**
* @tc.name: Hnp_Zip_003
* @tc.desc:  Verify HnpUnZip succeed with normal zip entry name.
* @tc.type: FUNC
* @tc.require:
*/
HWTEST_F(HnpZipTest, Hnp_Zip_003, TestSize.Level1)
{
    ASSERT_EQ(CreateZipWithEntryName(TEST_ZIP_FILE, "test.txt"), 0);
    ASSERT_EQ(mkdir(TEST_UNZIP_DIR, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH), 0);

    HnpSignMapInfo signMaps[1] = {};
    int count = 0;
    EXPECT_EQ(HnpUnZip(TEST_ZIP_FILE, TEST_UNZIP_DIR, "prefix", signMaps, &count), 0);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(access("./hnp_test_unzip/test.txt", F_OK), 0);
    RemoveTestArtifacts();
}

/**
* @tc.name: Hnp_Zip_004
* @tc.desc:  Verify HnpUnZip return error when unzGetCurrentFileInfo fails.
* @tc.type: FUNC
* @tc.require:
*/
HWTEST_F(HnpZipTest, Hnp_Zip_004, TestSize.Level1)
{
    ASSERT_EQ(CreateZipWithEntryName(TEST_ZIP_FILE, "test.txt"), 0);

    SetUnzGetInfoFail(1);
    HnpSignMapInfo signMaps[1] = {};
    int count = 0;
    EXPECT_EQ(HnpUnZip(TEST_ZIP_FILE, TEST_UNZIP_DIR, "prefix", signMaps, &count),
        HNP_ERRNO_BASE_UNZIP_GET_INFO_FAILED);
    EXPECT_EQ(count, 0);
    SetUnzGetInfoFail(0);
    RemoveTestArtifacts();
}

/**
* @tc.name: Hnp_Zip_005
* @tc.desc:  Verify HnpCfgGetFromZip return error when unzGetCurrentFileInfo fails.
* @tc.type: FUNC
* @tc.require:
*/
HWTEST_F(HnpZipTest, Hnp_Zip_005, TestSize.Level1)
{
    ASSERT_EQ(CreateZipWithEntryName(TEST_ZIP_FILE, "test.txt"), 0);

    SetUnzGetInfoFail(1);
    HnpCfgInfo cfg = {};
    EXPECT_EQ(HnpCfgGetFromZip(TEST_ZIP_FILE, &cfg), HNP_ERRNO_BASE_UNZIP_GET_INFO_FAILED);
    SetUnzGetInfoFail(0);
    RemoveTestArtifacts();
}
}  // namespace OHOS
