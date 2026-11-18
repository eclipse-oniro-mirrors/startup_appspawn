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

#include <errno.h>
#include <stdlib.h>
#include "sandbox_dec_calloc_mock.h"

int g_callocCallCount = 0;
int g_mockCallocFailAfter = -1;

void *__real_calloc(size_t nmemb, size_t size);
void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (g_mockCallocFailAfter >= 0 && g_callocCallCount == g_mockCallocFailAfter) {
        g_callocCallCount++;
        errno = ENOMEM;
        return NULL;
    }
    g_callocCallCount++;
    return __real_calloc(nmemb, size);
}
