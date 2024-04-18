/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#ifndef APPSPAWN_MODULE_THREAD_MGR_H
#define APPSPAWN_MODULE_THREAD_MGR_H
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INVALID_THREAD_ID 0
#define INVALID_TASK_HANDLE 0

typedef uint32_t ThreadTaskHandle;
typedef void *ThreadMgr;
typedef struct TagThreadContext ThreadContext;

typedef void (*TaskFinishProcessor)(ThreadTaskHandle handle, const ThreadContext *context);
typedef void (*TaskExecutor)(ThreadTaskHandle handle, const ThreadContext *context);

int CreateThreadMgr(uint32_t maxThreadCount, ThreadMgr *mgr);
int DestroyThreadMgr(ThreadMgr instance);
int ThreadMgrAddTask(ThreadMgr mgr, ThreadTaskHandle *taskHandle);
int ThreadMgrAddExecutor(ThreadMgr mgr,
    ThreadTaskHandle taskHandle, TaskExecutor executor, const ThreadContext *context);
int ThreadMgrCancelTask(ThreadMgr mgr, ThreadTaskHandle taskHandle);
int TaskSyncExecute(ThreadMgr mgr, ThreadTaskHandle taskHandle);  // 同步执行
int TaskExecute(ThreadMgr mgr, ThreadTaskHandle taskHandle, TaskFinishProcessor process, const ThreadContext *context);

#ifdef __cplusplus
}
#endif
#endif  // APPSPAWN_MODULE_THREAD_MGR_H