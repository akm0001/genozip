/*-----------------------------------------------------------*/
/* Block Sorting, Lossless Data Compression Library.         */
/* Platform specific functions and constants                 */
/*-----------------------------------------------------------*/

/*--

This file is a part of bsc and/or libbsc, a program and a library for
lossless, block-sorting data compression.

   Copyright (c) 2009-2012 Ilya Grebnov <ilya.grebnov@gmail.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

Please see the file LICENSE for full copyright information and file AUTHORS
for full list of contributors.

See also the bsc and libbsc web site:
  http://libbsc.com/ for more information.

--*/

#include <stdlib.h>
#include <string.h>
#include <memory.h>

#include "platform.h"

#include "libbsc.h"

static void * bsc_wrap_zero_malloc(void *vb, size_t size)
{
    void *address = bsc_malloc (vb, size);
    if(address != NULL)
    {
	memset(address, 0, size);
    }
    return address;
}

static void* (* bsc_malloc_fn)(void *vb, size_t size) = NULL;
static void* (* bsc_zero_malloc_fn)(void *vb, size_t size) = NULL;
static void  (* bsc_free_fn)(void *vb, void* address) = NULL;

void* bsc_malloc (void *vb, size_t size)
{
    return bsc_malloc_fn(vb, size);
}

void* bsc_zero_malloc(void *vb, size_t size)
{
    return bsc_zero_malloc_fn(vb, size);
}

void bsc_free (void *vb, void* address)
{
    return bsc_free_fn(vb, address);
}

int bsc_platform_init(int features,
                      void* (* malloc)(void *vb, size_t size), 
                      void* (* zero_malloc)(void *vb, size_t size), 
                      void (* free)(void *vb, void* address))
{
    /* If the caller provides a malloc function but not a zero_malloc
       function, we want to use malloc to implement zero_malloc.
       Otherwise we'll use the default function which may be slightly
       faster on some platforms. */
    if (zero_malloc != NULL)
    {
	bsc_zero_malloc_fn = zero_malloc;
    }
    else if (malloc != NULL)
    {
	bsc_zero_malloc_fn = bsc_wrap_zero_malloc;
    }

    if (malloc != NULL)
    {
	bsc_malloc_fn = malloc;
    }

    if (free != NULL)
    {
	bsc_free_fn = free;
    }
/*
#if defined(_WIN32)

    if (features & LIBBSC_FEATURE_LARGEPAGES)
    {
        HANDLE hToken = 0;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        {
            LUID luid;
            if (LookupPrivilegeValue(NULL, TEXT("SeLockMemoryPrivilege"), &luid))
            {
                TOKEN_PRIVILEGES tp;

                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

                AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), 0, 0);
            }

            CloseHandle(hToken);
        }

        {
            if (HMODULE hKernel = GetModuleHandle(TEXT("kernel32.dll")))
            {
                typedef SIZE_T (WINAPI * GetLargePageMinimumProcT)();

                GetLargePageMinimumProcT largePageMinimumProc = (GetLargePageMinimumProcT)GetProcAddress(hKernel, "GetLargePageMinimum");
                if (largePageMinimumProc != NULL)
                {
                    SIZE_T largePageSize = largePageMinimumProc();

                    if ((largePageSize & (largePageSize - 1)) != 0) largePageSize = 0;

                    g_LargePageSize = largePageSize;
                }
            }
        }
    }

#endif
*/
    return LIBBSC_NO_ERROR;
}

/*-----------------------------------------------------------*/
/* End                                          platform.cpp */
/*-----------------------------------------------------------*/
