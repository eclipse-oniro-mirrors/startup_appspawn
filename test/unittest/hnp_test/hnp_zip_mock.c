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

#include "zlib.h"
#include "contrib/minizip/unzip.h"

int __real_unzGetCurrentFileInfo(unzFile file, unz_file_info *pfile_info, char *szFileName,
    uLong fileNameBufferSize, void *extraField, uLong extraFieldBufferSize, char *szComment,
    uLong commentBufferSize);

static int g_unzGetInfoFail = 0;

void SetUnzGetInfoFail(int fail)
{
    g_unzGetInfoFail = fail;
}

int __wrap_unzGetCurrentFileInfo(unzFile file, unz_file_info *pfile_info, char *szFileName,
    uLong fileNameBufferSize, void *extraField, uLong extraFieldBufferSize, char *szComment,
    uLong commentBufferSize)
{
    if (g_unzGetInfoFail) {
        return UNZ_ERRNO;
    }
    return __real_unzGetCurrentFileInfo(file, pfile_info, szFileName, fileNameBufferSize, extraField,
        extraFieldBufferSize, szComment, commentBufferSize);
}
