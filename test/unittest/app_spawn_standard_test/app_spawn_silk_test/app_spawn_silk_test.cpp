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

#include "appspawn_utils.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "appspawn_silk.h"

extern struct SilkConfig g_silkConfig;
APPSPAWN_STATIC bool ParseSilkConfig(const cJSON *root, struct SilkConfig *config);
#ifdef __cplusplus
}
#endif

using namespace testing;
using namespace testing::ext;

class ParseSilkConfigTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

/**
 * @brief 模拟测试，正常解析json文件并且正常使能silk
 *
 */
HWTEST_F(ParseSilkConfigTest, Parse_Silk_Config_001, TestSize.Level0)
{
    const char *silkJsonStr = "{\"enabled_app_list\":[\"com.taobao.taobao\",\"com.tencent.mm\"]}";
    cJSON *root =  cJSON_Parse(silkJsonStr);
    const char* TEST_ENABLE_SILK_PROCESS_NAME = "com.taobao.taobao";
    bool ret = ParseSilkConfig(root, &g_silkConfig);
    cJSON_Delete(root);
    ASSERT_EQ(ret, true);
    ASSERT_EQ(g_silkConfig.configCursor, 2);
    ret = LoadSilkLibrary(TEST_ENABLE_SILK_PROCESS_NAME);
    ASSERT_EQ(ret, true);
    ASSERT_EQ(g_silkConfig.configItems, NULL);
    ASSERT_EQ(g_silkConfig.configCursor, 0);
}

/**
 * @brief 模拟测试，processName为NULL的情况
 *
 */
HWTEST_F(ParseSilkConfigTest, Parse_Silk_Config_002, TestSize.Level0)
{
    const char *silkJsonStr = "{\"enabled_app_list\":[\"com.taobao.taobao\",\"com.tencent.mm\"]}";
    cJSON *root =  cJSON_Parse(silkJsonStr);
    const char* TEST_ENABLE_SILK_PROCESS_NAME = NULL;
    bool ret = ParseSilkConfig(root, &g_silkConfig);
    cJSON_Delete(root);
    ASSERT_EQ(ret, true);
    ASSERT_EQ(g_silkConfig.configCursor, 2);
    ret = LoadSilkLibrary(TEST_ENABLE_SILK_PROCESS_NAME);
    ASSERT_EQ(ret, false);
    ASSERT_EQ(g_silkConfig.configItems, NULL);
    ASSERT_EQ(g_silkConfig.configCursor, 0);
}

HWTEST_F(ParseSilkConfigTest, Parse_Silk_Config_003, TestSize.Level0)
{
    const char *silkJsonStr = "{\"enabled_app_list0\":[\"com.taobao.taobao\",\"com.tencent.mm\"]}";
    cJSON *root =  cJSON_Parse(silkJsonStr);
    const char* testEnableSilkProcessName = NULL;
    bool ret = ParseSilkConfig(root, &g_silkConfig);
    cJSON_Delete(root);
    ASSERT_EQ(ret, false);
}

/**
 * @brief ParseSilkConfig处理超长 appName（> 256）时跳过该项
 *        覆盖len > SILK_JSON_NAME_MAX 的continue 分支（line 84-85）
 */
HWTEST_F(ParseSilkConfigTest, Parse_Silk_Config_LongName_001, TestSize.Level0)
{
    std::string longName(257, 'x');
    std::string jsonStr = "{\"enabled_app_list\":[\"" + longName + "\",\"com.valid.app\"]}";
    cJSON *root = cJSON_Parse(jsonStr.c_str());
    ASSERT_NE(root, nullptr);
    bool ret = ParseSilkConfig(root, &g_silkConfig);
    cJSON_Delete(root);
    EXPECT_EQ(ret, true);
    EXPECT_EQ(g_silkConfig.configCursor, 1);
    if (g_silkConfig.configCursor == 1) {
        EXPECT_STREQ(g_silkConfig.configItems[0], "com.valid.app");
    }
    for (int i = 0; i < g_silkConfig.configCursor; i++) {
        free(g_silkConfig.configItems[i]);
        g_silkConfig.configItems[i] = nullptr;
    }
    free(g_silkConfig.configItems);
    g_silkConfig.configItems = nullptr;
    g_silkConfig.configCursor = 0;
}

/**
 * @brief 超长 appName 位于数组末位时，末位continue后循环结束
 *        isSuccess保持false，FreeAllSilkConfig全量释放并返回false
 *        覆盖appspawn_silk.c:100-102末位失败+104-106释放返回分支
 */
HWTEST_F(ParseSilkConfigTest, Parse_Silk_Config_LongName_002, TestSize.Level0)
{
    std::string longName(257, 'x');
    std::string jsonStr = "{\"enabled_app_list\":[\"com.valid.app\",\"" + longName + "\"]}";
    cJSON *root = cJSON_Parse(jsonStr.c_str());
    ASSERT_NE(root, nullptr);
    bool ret = ParseSilkConfig(root, &g_silkConfig);
    cJSON_Delete(root);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(g_silkConfig.configItems, nullptr);
	EXPECT_EQ(g_silkConfig.configCursor, 0);
}

/**
 * @brief 边界值：appName恰好256字符（len=257 > SILK_JSON_NAME_MAX）应被跳过
 *        放在末位时与LongName_002同样返回false
 */
HWTEST_F(ParseSilkConfigTest, Parse_Silk_Config_LongName_003, TestSize.Level0)
{
    std::string boundaryName(256, 'x');
    std::string jsonStr = "{\"enabled_app_list\":[\"com.valid.app\",\"" + boundaryName + "\"]}";
    cJSON *root = cJSON_Parse(jsonStr.c_str());
    ASSERT_NE(root, nullptr);
    bool ret = ParseSilkConfig(root, &g_silkConfig);
    cJSON_Delete(root);
    EXPECT_EQ(ret, false);
    EXPECT_EQ(g_silkConfig.configItems, nullptr);
	EXPECT_EQ(g_silkConfig.configCursor, 0);
}

/**
 * @brief 边界值：appName 255字符（len=256 <= SILK_JSON_NAME_MAX）应被接受
 *        放在末位时末位成功拷贝，isSuccess = true, 返回true
 */
HWTEST_F(ParseSilkConfigTest, Parse_Silk_Config_LongName_004, TestSize.Level0)
{
    std::string validName(255, 'x');
    std::string jsonStr = "{\"enabled_app_list\":[\"com.valid.app\",\"" + validName + "\"]}";
    cJSON *root = cJSON_Parse(jsonStr.c_str());
    ASSERT_NE(root, nullptr);
    bool ret = ParseSilkConfig(root, &g_silkConfig);
    cJSON_Delete(root);
    EXPECT_EQ(ret, true);
	EXPECT_EQ(g_silkConfig.configCursor, 2);
	for (int i = 0; i < g_silkConfig.configCursor; i++) {
        free(g_silkConfig.configItems[i]);
        g_silkConfig.configItems[i] = nullptr;
    }
    free(g_silkConfig.configItems);
    g_silkConfig.configItems = nullptr;
    g_silkConfig.configCursor = 0;
}
