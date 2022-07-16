/**
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
 *
 */

#ifndef _SVCBATCH_H_INCLUDED_
#define _SVCBATCH_H_INCLUDED_

/**
 * Disable or reduce the frequency of...
 *   C4100: unreferenced formal parameter
 *   C4702: unreachable code
 *   C4244: int to char/short - precision loss
 */
#if defined(_MSC_VER)
# pragma warning(disable: 4100 4244 4702)
# define CPP_INT64_C(_v)    (_v##I64)
# define CPP_UINT64_C(_v)   (_v##UI64)
#else
# define CPP_INT64_C(_v)    (_v##LL)
# define CPP_UINT64_C(_v)   (_v##ULL)
#endif

/**
 * Version info
 */
#define SVCBATCH_MAJOR_VERSION  1
#define SVCBATCH_MINOR_VERSION  2
#define SVCBATCH_PATCH_VERSION  1
#if defined(_VENDOR_NUM)
# define SVCBATCH_MICRO_VERSION _VENDOR_NUM
#else
# define SVCBATCH_MICRO_VERSION 0
#endif
#define SVCBATCH_ABI_VERSION    0x20220120

/**
 * Set to zero for non dev versions
 */
#define SVCBATCH_ISDEV_VERSION  1

#define SVCBATCH_NAME           "SvcBatch"
#define SVCBATCH_APPNAME        "SvcBatch Service"
#define SHUTDOWN_APPNAME        "SvcBatch Shutdown"
#define SVCBATCH_LOGNAME       L"SvcBatch.log"
#define SHUTDOWN_LOGNAME       L"SvcBatch.shutdown.log"
#define SVCBATCH_RUNNAME       L"SvcBatch.run"

/**
 * Maximum number of SvcBatch.log.N files
 */
#define SVCBATCH_MAX_LOGS       9
#define SVCBATCH_LOG_BASE       L"Logs"

/**
 * Custom SCM control code that
 * sends CTRL_BREAK_EVENT to the child processes.
 *
 * This option has to be enabled on install
 * by adding /b switch.
 *
 * eg. C:\>sc control SvcBatchServiceName 233
 * will cause java.exe to dump thread stack
 * if running inside batch file.
 *
 * Programs that do not handle CTRL_BREAK_EVENT
 * will cause SvcBatch to fail or hang
 */
#define SVCBATCH_CTRL_BREAK     233
/**
 * This signal will rotate log files
 * in the same way as on service startup
 *
 * eg. C:\>sc control SvcBatchServiceName 234
 *
 * Check documentation for more details
 */
#define SVCBATCH_CTRL_ROTATE    234


#define SVCBATCH_START_HINT     5000
#define SVCBATCH_STOP_HINT      20000
#define SVCBATCH_STOP_CHECK     10000
#define SVCBATCH_STOP_WAIT      30000
#define SVCBATCH_STOP_STEP      5000
#define SVCBATCH_STOP_SYNC      2000
#define SVCBATCH_PENDING_WAIT   1000
#define SVCBATCH_PENDING_INIT   200
#define SVCBATCH_MIN_LOGSIZE    CPP_INT64_C(32768)
#if defined(_RUN_API_TESTS)
#define SVCBATCH_MIN_LOGRTIME   2
#else
#define SVCBATCH_MIN_LOGRTIME   30
#endif
#define SVCBATCH_LOGROTATE_INIT 2000
#define SVCBATCH_LOGROTATE_STEP 20000
#define SVCBATCH_LOGROTATE_DEF  ONE_DAY * CPP_INT64_C(-30)
#define SVCBATCH_LOGFLUSH_SIZE  4096

#define MS_IN_DAY               86400000
#define MS_IN_SECOND            1000
#define MS_IN_MINUTE            60000
#define MS_IN_HOUR              3600000

#define BBUFSIZ                 512
#define SBUFSIZ                 1024
#define MBUFSIZ                 2048
#define HBUFSIZ                 8192
#define ONE_MINUTE              CPP_INT64_C(600000000)
#define ONE_HOUR                CPP_INT64_C(36000000000)
#define ONE_DAY                 CPP_INT64_C(864000000000)
#define KILOBYTES(_x)           ((_x) * CPP_INT64_C(1024))
#define MEGABYTES(_x)           ((_x) * CPP_INT64_C(1048576))

/**
 * Helper macros
 */
#define IS_INVALID_HANDLE(_h)   (((_h) == NULL) || ((_h) == INVALID_HANDLE_VALUE))
#define IS_EMPTY_WCS(_s)        (((_s) == NULL) || (*(_s) == L'\0'))
#define DSIZEOF(_s)             (DWORD)(sizeof(_s))

#define SAFE_CLOSE_HANDLE(_h)                                       \
    if (((_h) != NULL) && ((_h) != INVALID_HANDLE_VALUE))           \
        CloseHandle((_h));                                          \
    (_h) = NULL

#define XENDTHREAD(_r)          _endthreadex(_r); return (_r)
#define WAIT_OBJECT_1          (WAIT_OBJECT_0 + 1)
#define WAIT_OBJECT_2          (WAIT_OBJECT_0 + 2)
#define WAIT_OBJECT_3          (WAIT_OBJECT_0 + 3)


/**
 * Helper macros for properly quoting a value as a
 * string in the C preprocessor
 */
#define CPP_TOSTR_HELPER(n)     #n
#define CPP_TOSTR(n)            CPP_TOSTR_HELPER(n)

#define CPP_WIDEN_HELPER(n)     L ## n
#define CPP_WIDEN(n)            CPP_WIDEN_HELPER(n)


/**
 * Construct build stamp
 */
#if defined(_MSC_VER)
# define SVCBATCH_BUILD_CC      "msc " CPP_TOSTR(_MSC_FULL_VER) "."     \
                                CPP_TOSTR(_MSC_BUILD)
#elif defined(__GNUC__)
# define SVCBATCH_BUILD_CC      "gcc " CPP_TOSTR(__GNUC__) "."          \
                                CPP_TOSTR(__GNUC_MINOR__) "."           \
                                CPP_TOSTR(__GNUC_PATCHLEVEL__)
#else
# define SVCBATCH_BUILD_CC      "unknown"
#endif
#define SVCBATCH_BUILD_STAMP    "(" __DATE__ " " __TIME__ " " SVCBATCH_BUILD_CC ")"

#if defined(_VENDOR_SFX)
# define SVCBATCH_VENDOR_SFX    CPP_TOSTR(_VENDOR_SFX)
#else
# define SVCBATCH_VENDOR_SFX    ""
#endif

#if SVCBATCH_ISDEV_VERSION
# define SVCBATCH_VERSION_SFX   SVCBATCH_VENDOR_SFX "-dev"
#else
# define SVCBATCH_VERSION_SFX   SVCBATCH_VENDOR_SFX
#endif

/**
 * Macro for .rc files using numeric csv representation
 */
#define SVCBATCH_VERSION_CSV    SVCBATCH_MAJOR_VERSION,                 \
                                SVCBATCH_MINOR_VERSION,                 \
                                SVCBATCH_PATCH_VERSION,                 \
                                SVCBATCH_MICRO_VERSION

#define SVCBATCH_VERSION_STR \
                                CPP_TOSTR(SVCBATCH_MAJOR_VERSION) "."   \
                                CPP_TOSTR(SVCBATCH_MINOR_VERSION) "."   \
                                CPP_TOSTR(SVCBATCH_PATCH_VERSION)       \
                                SVCBATCH_VERSION_SFX

#define SVCBATCH_DESCRIPTION \
    "Run batch files as Windows Services"

#define SVCBATCH_COPYRIGHT \
    "Copyright (c) 1964-2022 The Acme Corporation or its "              \
    "licensors, as applicable."

#define SVCBATCH_COMPANY_NAME "Acme Corporation"

#define SVCBATCH_LICENSE_SHORT \
    "Licensed under the Apache-2.0 License"

#define SVCBATCH_LICENSE \
    "Licensed under the Apache License, Version 2.0 (the ""License"");\n"       \
    "you may not use this file except in compliance with the License.\n"        \
    "You may obtain a copy of the License at\n\n"                               \
    "http://www.apache.org/licenses/LICENSE-2.0\n\n"                            \
    "Unless required by applicable law or agreed to in writing, software\n"     \
    "distributed under the License is distributed on an ""AS IS"" BASIS,\n"     \
    "WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"\
    "See the License for the specific language governing permissions and\n"     \
    "limitations under the License."


#endif /* _SVCBATCH_H_INCLUDED_ */
