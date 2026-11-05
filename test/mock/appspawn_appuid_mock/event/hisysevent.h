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

#ifndef APPUID_TEST_HISYSEVENT_H
#define APPUID_TEST_HISYSEVENT_H

#include <string>
#include "appspawn_appuid_mock.h"

namespace OHOS {
namespace HiviewDFX {
namespace HiSysEvent {
namespace Domain {
constexpr char STARTUP[] = "STARTUP";
}
enum class EventType { BEHAVIOR };
}

inline int HiSysEventWrite(const char *, const char *, HiSysEvent::EventType,
    const char *, const std::string &, const char *, int pid, const char *, int appUid,
    const char *, int status)
{
    AppUidMockRecordEvent(pid, appUid, status);
    return 0;
}
} // namespace HiviewDFX
} // namespace OHOS
#endif
