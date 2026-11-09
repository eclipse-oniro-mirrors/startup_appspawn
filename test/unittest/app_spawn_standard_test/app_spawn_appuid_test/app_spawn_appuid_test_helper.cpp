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

#include "app_spawn_appuid_test_helper.h"

namespace OHOS {
namespace AppUidTest {
namespace {

/* Append one TLV (header + value) into buffer, advance currLen and tlvCount. */
int AppendAppUidTlv(std::vector<uint8_t> &buffer, uint32_t tlvType, const void *value, size_t valueSize,
    uint32_t &currLen, uint32_t &tlvCount)
{
    if (currLen > buffer.size() || valueSize > UINT16_MAX - sizeof(AppSpawnTlv) ||
        sizeof(AppSpawnTlv) + valueSize > buffer.size() - currLen) {
        return -1;
    }
    AppSpawnTlv tlv = {};
    tlv.tlvType = tlvType;
    tlv.tlvLen = static_cast<uint16_t>(sizeof(AppSpawnTlv) + valueSize);
    uint8_t *dest = buffer.data() + currLen;
    size_t room = buffer.size() - currLen;
    if (memcpy_s(dest, room, &tlv, sizeof(tlv)) != 0 ||
        memcpy_s(dest + sizeof(tlv), room - sizeof(tlv), value, valueSize) != 0) {
        return -1;
    }
    currLen += tlv.tlvLen;
    tlvCount++;
    return 0;
}

/* Fill the fixed message header shared by both builders. */
int InitAppUidMsg(AppSpawnMsg *msg, uint32_t msgType, const char *processName)
{
    msg->magic = APPSPAWN_MSG_MAGIC;
    msg->msgType = msgType;
    msg->msgLen = sizeof(AppSpawnMsg);
    msg->msgId = 1;
    msg->tlvCount = 0;
    return strcpy_s(msg->processName, sizeof(msg->processName), processName);
}

/* Append flags/domain/token TLVs shared by both builders. */
int AppendFixedAppUidTlvs(std::vector<uint8_t> &buffer, uint32_t &currLen, uint32_t &tlvCount)
{
    struct { uint32_t count; uint32_t flags[3]; } msgFlags = {3, {0, 0, 0}};  // count=3 blocks
    struct { uint32_t hapFlags; char apl[32]; } domainInfo = {0, "normal"};  // 32 APP_APL_MAX_LEN
    AppSpawnMsgAccessToken token = {APPUID_TEST_ACCESS_TOKEN};
    if (AppendAppUidTlv(buffer, TLV_MSG_FLAGS, &msgFlags, sizeof(msgFlags), currLen, tlvCount) != 0 ||
        AppendAppUidTlv(buffer, TLV_DOMAIN_INFO, &domainInfo, sizeof(domainInfo), currLen, tlvCount) != 0 ||
        AppendAppUidTlv(buffer, TLV_ACCESS_TOKEN_INFO, &token, sizeof(token), currLen, tlvCount) != 0) {
        return -1;
    }
    return 0;
}

/* Append bundle info TLV: {bundleIndex, bundleName}. */
int AppendBundleAppUidTlv(std::vector<uint8_t> &buffer, uint32_t &currLen, uint32_t &tlvCount,
    const char *processName)
{
    struct { uint32_t bundleIndex; char bundleName[256]; } bundleInfo = {};  // 256 APP_LEN_BUNDLE_NAME
    bundleInfo.bundleIndex = 0;
    if (strcpy_s(bundleInfo.bundleName, sizeof(bundleInfo.bundleName), processName) != 0) {
        return -1;
    }
    return AppendAppUidTlv(buffer, TLV_BUNDLE_INFO, &bundleInfo, sizeof(bundleInfo), currLen, tlvCount);
}

/* Append dac info TLV with the uid/appUid pair. */
int AppendDacAppUidTlv(std::vector<uint8_t> &buffer, uint32_t &currLen, uint32_t &tlvCount,
    uint32_t uid, uint32_t appUid)
{
    AppDacInfo dacInfo = {};
    dacInfo.uid = uid;
    dacInfo.appUid = appUid;
    dacInfo.gid = APPUID_TEST_DAC_GID;
    if (strcpy_s(dacInfo.userName, sizeof(dacInfo.userName), "appuid-test") != 0) {
        return -1;
    }
    return AppendAppUidTlv(buffer, TLV_DAC_INFO, &dacInfo, sizeof(dacInfo), currLen, tlvCount);
}

int AppendUserIdAppUidTlv(std::vector<uint8_t> &buffer, uint32_t &currLen, uint32_t &tlvCount)
{
    struct { uint16_t dataLen; uint16_t dataType; char name[APPSPAWN_TLV_NAME_LEN]; char value[4]; }
        userId = {4, 0, MSG_EXT_NAME_USERID, "100"};
    return AppendAppUidTlv(buffer, TLV_MAX, &userId, sizeof(userId), currLen, tlvCount);
}

/* Append all common TLVs (optionally checkpoint info) and finalize msg counters. */
int AppendAppUidTlvsAndFinalize(std::vector<uint8_t> &buffer, AppSpawnMsg *msg, const char *processName,
    uint32_t uid, uint32_t appUid, AppSpawnCheckpointInfo *cpInfo, bool withUserId = false)
{
    uint32_t currLen = sizeof(AppSpawnMsg);
    uint32_t tlvCount = 0;
    if (AppendFixedAppUidTlvs(buffer, currLen, tlvCount) != 0 ||
        AppendBundleAppUidTlv(buffer, currLen, tlvCount, processName) != 0 ||
        AppendDacAppUidTlv(buffer, currLen, tlvCount, uid, appUid) != 0) {
        return -1;
    }
    if (cpInfo != nullptr &&
        AppendAppUidTlv(buffer, TLV_CHECK_POINT_INFO, cpInfo, sizeof(*cpInfo), currLen, tlvCount) != 0) {
        return -1;
    }
    if (withUserId && AppendUserIdAppUidTlv(buffer, currLen, tlvCount) != 0) {
        return -1;
    }
    msg->msgLen = currLen;
    msg->tlvCount = tlvCount;
    return 0;
}

/* Parse the crafted buffer into a decoded message node. */
AppSpawnMsgNode *ParseAppUidMsgNode(std::vector<uint8_t> &buffer, AppSpawnMsg *msg)
{
    AppSpawnMsgNode *msgNode = nullptr;
    uint32_t recvLen = 0;
    uint32_t reminder = 0;
    if (GetAppSpawnMsgFromBuffer(buffer.data(), msg->msgLen, &msgNode, &recvLen, &reminder) != 0 ||
        msgNode == nullptr || DecodeAppSpawnMsg(msgNode) != 0) {
        DeleteAppSpawnMsg(&msgNode);
        return nullptr;
    }
    return msgNode;
}

/* Wrap a decoded message node into an AppSpawningCtx, set flag on demand. */
AppSpawningCtx *CreateAppUidProperty(AppSpawnMsgNode *msgNode, bool customSandbox)
{
    AppSpawningCtx *property = CreateAppSpawningCtx();
    if (property == nullptr) {
        DeleteAppSpawnMsg(&msgNode);
        return nullptr;
    }
    property->message = msgNode;
    if (customSandbox) {
        (void)SetAppSpawnMsgFlag(property->message, TLV_MSG_FLAGS, APP_FLAGS_CUSTOM_SANDBOX);
    }
    return property;
}

}  // namespace

/*
 * Build an AppSpawningCtx with hand-crafted TLVs.
 * customSandbox=true: uid=100 (userId-like 10x), appUid=20010052 (200xxxxx), CUSTOM_SANDBOX flag.
 * customSandbox=false: normal app, uid==appUid==20010029.
 */
AppSpawningCtx *BuildAppUidProperty(const char *processName, uint32_t uid, uint32_t appUid,
    bool customSandbox, bool withUserId)
{
    std::vector<uint8_t> buffer(APPUID_TEST_MSG_BUFFER_SIZE);
    AppSpawnMsg *msg = reinterpret_cast<AppSpawnMsg *>(buffer.data());
    if (InitAppUidMsg(msg, MSG_APP_SPAWN, processName) != 0 ||
        AppendAppUidTlvsAndFinalize(buffer, msg, processName, uid, appUid, nullptr, withUserId) != 0) {
        return nullptr;
    }
    AppSpawnMsgNode *msgNode = ParseAppUidMsgNode(buffer, msg);
    return msgNode == nullptr ? nullptr : CreateAppUidProperty(msgNode, customSandbox);
}

/*
 * Variant adding TLV_CHECK_POINT_INFO (imgPid/checkPointId/imgName) for
 * checkpoint worker/image process tests. imgName is optional (nullptr falls
 * back to the bundle name inside DoCheckpointProcess).
 */
AppSpawningCtx *BuildAppUidPropertyWithCheckpoint(const char *processName, uint32_t uid, uint32_t appUid,
    bool customSandbox, pid_t imgPid, uint64_t checkPointId)
{
    std::vector<uint8_t> buffer(APPUID_TEST_MSG_BUFFER_SIZE);
    AppSpawnMsg *msg = reinterpret_cast<AppSpawnMsg *>(buffer.data());
    AppSpawnCheckpointInfo cpInfo = {};  // imgName empty, falls back to bundle name
    cpInfo.imgPid = imgPid;
    cpInfo.checkPointId = checkPointId;
    cpInfo.imgName[0] = '\0';
    if (InitAppUidMsg(msg, MSG_SPAWN_WORKER_PROCESS, processName) != 0 ||
        AppendAppUidTlvsAndFinalize(buffer, msg, processName, uid, appUid, &cpInfo) != 0) {
        return nullptr;
    }
    AppSpawnMsgNode *msgNode = ParseAppUidMsgNode(buffer, msg);
    return msgNode == nullptr ? nullptr : CreateAppUidProperty(msgNode, customSandbox);
}

void CleanupAppUidProperty(AppSpawningCtx *property)
{
    if (property == nullptr) {
        return;
    }
    AppSpawnMsgNode *msgNode = property->message;
    property->message = nullptr;
    DeleteAppSpawningCtx(property);
    if (msgNode != nullptr) {
        DeleteAppSpawnMsg(&msgNode);
    }
}

}  // namespace AppUidTest
}  // namespace OHOS
