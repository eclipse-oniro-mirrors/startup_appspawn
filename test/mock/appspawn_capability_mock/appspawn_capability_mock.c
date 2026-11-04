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
#include "appspawn_capability_mock.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/prctl.h>

// Combine two 32-bit capability v3 data words into a 64-bit test bitmap.
#define CAPSET_WORD_BITS 32
#define CAPABILITY_MASK_BITS 64

// Enabled only while a capability test owns the capture state.
static CapabilityTestState *g_capabilityTestState = NULL;

void SetCapabilityTestState(CapabilityTestState *state)
{
    g_capabilityTestState = state;
}

/*
 * Capture E/P/I before returning the configured result, so injected failures remain observable.
 * Nonzero results set EPERM; missing data sets EINVAL. Disabled capture succeeds.
 */
int CapabilityCapsetMock(cap_user_header_t hdrp, const cap_user_data_t datap)
{
    UNUSED(hdrp);
    if (g_capabilityTestState == NULL) {
        return 0;
    }
    if (datap == NULL) {
        errno = EINVAL;
        return -1;
    }
    g_capabilityTestState->capsetCalls++;
    g_capabilityTestState->effective = (uint64_t)datap[0].effective |
        ((uint64_t)datap[1].effective << CAPSET_WORD_BITS);
    g_capabilityTestState->permitted = (uint64_t)datap[0].permitted |
        ((uint64_t)datap[1].permitted << CAPSET_WORD_BITS);
    g_capabilityTestState->inheritable = (uint64_t)datap[0].inheritable |
        ((uint64_t)datap[1].inheritable << CAPSET_WORD_BITS);
    if (g_capabilityTestState->capsetResult != 0) {
        errno = EPERM;
    }
    return g_capabilityTestState->capsetResult;
}

/*
 * Capture attempted PR_CAP_AMBIENT_RAISE calls, including injected EPERM failures.
 * Unhandled operations or disabled capture succeed without calling the kernel.
 * Out-of-bitmap capability indexes return -1/EINVAL. The bound only guards shifts;
 * kernel capability limits and permitted/inheritable prerequisites are not emulated.
 */
int CapabilityPrctlMock(int option, ...)
{
    if (g_capabilityTestState == NULL || option != PR_CAP_AMBIENT) {
        return 0;
    }
    va_list args;
    va_start(args, option);
    int operation = va_arg(args, int);
    int cap = -1;
    if (operation == PR_CAP_AMBIENT_RAISE) {
        cap = va_arg(args, int);
    }
    va_end(args);
    if (operation != PR_CAP_AMBIENT_RAISE) {
        return 0;
    }
    g_capabilityTestState->ambientCalls++;
    if (cap < 0 || cap >= CAPABILITY_MASK_BITS) {
        errno = EINVAL;
        return -1;
    }
    g_capabilityTestState->ambient |= (uint64_t)1 << cap;
    if (g_capabilityTestState->ambientResult != 0) {
        errno = EPERM;
    }
    return g_capabilityTestState->ambientResult;
}

// Network isolation is outside the capability unit tests.
void DisallowInternet(void)
{
}
