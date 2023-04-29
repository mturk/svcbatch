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

#if defined(_MSC_VER)
/**
 * Disable or reduce the frequency of...
 *   C4100: unreferenced formal parameter
 *   C4702: unreachable code
 *   C4244: int to char/short - precision loss
 */
# pragma warning(disable: 4100 4244 4702)
#endif

#if defined(_MSC_VER)
# define CPP_INT64_C(_v)    (_v##I64)
# define CPP_UINT64_C(_v)   (_v##UI64)
#else
# define CPP_INT64_C(_v)    (_v##LL)
# define CPP_UINT64_C(_v)   (_v##ULL)
#endif

/**
 * Helper macros for properly quoting a value as a
 * string in the C preprocessor
 */
#define CPP_TOSTR_HELPER(n)     #n
#define CPP_TOSTR(n)            CPP_TOSTR_HELPER(n)

#define CPP_TOWCS_HELPER(n)     L ## #n
#define CPP_TOWCS(n)            CPP_TOWCS_HELPER(n)

#define CPP_WIDEN_HELPER(n)     L ## n
#define CPP_WIDEN(n)            CPP_WIDEN_HELPER(n)



/**
 * Version info
 */
#define SVCBATCH_MAJOR_VERSION  2
#define SVCBATCH_MINOR_VERSION  0
#define SVCBATCH_PATCH_VERSION  1
#if defined(VERSION_MICRO)
#define SVCBATCH_MICRO_VERSION  VERSION_MICRO
#else
#define SVCBATCH_MICRO_VERSION  0
#endif
/**
 * Set to zero for release versions
 */
#define SVCBATCH_ISDEV_VERSION  1


#define SVCBATCH_NAME           "SvcBatch"
#define SVCBATCH_APPNAME        "SvcBatch Service"
#define SHUTDOWN_APPNAME        "SvcBatch Shutdown"
#define SVCBATCH_LOGNAME       L"SvcBatch.log"
#define SHUTDOWN_LOGNAME       L"SvcBatch.shutdown.log"
#define SVCBATCH_LOGSDIR       L"Logs"
#define SHUTDOWN_IPCNAME       L"Local\\se-"
#define SVCBATCH_PIPEPFX       L"\\\\.\\pipe\\sp-"

/**
 * Maximum number of SvcBatch.log.N files
 */
#define SVCBATCH_MAX_LOGS       9

/**
 * Maximum number of arguments
 * for shutdown and external log application
 */
#define SVCBATCH_MAX_ARGS       16

/**
 * Maximum length for the path and
 * file names. Although the C Runtime
 * supports path lengths up to 32768 characters
 * in length, we set the limit to more
 * realistic number.
 */
#define SVCBATCH_PATH_MAX       4096

#define SVCBATCH_NAME_MAX       256
#define SVCBATCH_RSRC_LEN       64

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

/**
 * Timing definitions
 */
#define SVCBATCH_START_HINT     5000
#define SVCBATCH_STOP_HINT      20000
#define SVCBATCH_STOP_CHECK     10000
#define SVCBATCH_STOP_WAIT      30000
#define SVCBATCH_STOP_STEP      5000

#define SVCBATCH_SHUT_HINT      5000
#define SVCBATCH_SHUT_CHECK     5000
#define SVCBATCH_SHUT_WAIT      10000
#define SVCBATCH_SHUT_STEP      2500

/**
 * Minimum rotate size in kilobytes
 */
#define SVCBATCH_MIN_ROTATE_S   1
/**
 * Minimum time between two log
 * rotations in minutes
 */
#define SVCBATCH_MIN_ROTATE_T   2

#define MS_IN_DAY               86400000
#define MS_IN_SECOND            1000
#define MS_IN_MINUTE            60000
#define MS_IN_HOUR              3600000
#define ONE_MINUTE              CPP_INT64_C(600000000)
#define ONE_HOUR                CPP_INT64_C(36000000000)
#define ONE_DAY                 CPP_INT64_C(864000000000)
#define KILOBYTES(_x)           (CPP_INT64_C(_x) * CPP_INT64_C(1024))
#define MEGABYTES(_x)           (CPP_INT64_C(_x) * CPP_INT64_C(1048576))

/**
 * Misc buffer size definitions
 */
#define TBUFSIZ                 64
#define RBUFSIZ                 128
#define NBUFSIZ                 256
#define BBUFSIZ                 512
#define SBUFSIZ                 1024
#define MBUFSIZ                 2048
#define FBUFSIZ                 8192
#define HBUFSIZ                 16384
#define EBUFSIZ                 32768

#if defined(_DEBUG)
# define DBG_PRINTF(Fmt, ...)   dbgprintf(__FUNCTION__, Fmt, ##__VA_ARGS__)
# define DBG_PRINTS(Msg)        dbgprints(__FUNCTION__, Msg)
#else
# define DBG_PRINTF(Fmt, ...)   (void)0
# define DBG_PRINTS(Msg)        (void)0
#endif

#define xsyserror(_n, _e, _d)   svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_ERROR_TYPE,      _n, _e, _d)
#define xsyswarn(_n, _e, _d)    svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_WARNING_TYPE,    _n, _e, _d)
#define xsysinfo(_e, _d)        svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_INFORMATION_TYPE, 0, _e, _d)

/**
 * Process state
 */
#define SVCBATCH_PROCESS_STOPPED    0x00000001  /* The process is not running   */
#define SVCBATCH_PROCESS_STARTING   0x00000002  /* The process is starting      */
#define SVCBATCH_PROCESS_STOPPING   0x00000003  /* The process is stopping      */
#define SVCBATCH_PROCESS_RUNNING    0x00000004  /* The process is running       */
/**
 * Process types
 */
#define SVCBATCH_SERVICE_PROCESS    0x00000000   /* Main service process         */
#define SVCBATCH_SHUTDOWN_PROCESS   0x00000001   /* Shutdown process             */
#define SVCBATCH_SHELL_PROCESS      0x00000002   /* Shell (cmd.exe) process      */
#define SVCBATCH_USER_LOG_PROCESS   0x00000003   /* External log process         */
/**
 * Log state
 */
#define SVCBATCH_LOG_CLOSED         0x00000001   /* The log file is closed       */
#define SVCBATCH_LOG_OPENING        0x00000002   /* The SvcBatch is opening the log  file    */
#define SVCBATCH_LOG_CLOSING        0x00000003   /* The log file is closing state            */
#define SVCBATCH_LOG_OPENED         0x00000004   /* The log file is opened                   */
#define SVCBATCH_LOG_ROTATING       0x00000005   /* The SvcBatch is rotating log file        */
/**
 * Log types
 */
#define SVCBATCH_LOG_FILE           0x00000001   /* The log is standard file                 */
#define SVCBATCH_LOG_PIPE           0x00000002   /* The log is redirected to user process    */



/**
 * Helper macros
 */
#define WNUL                    L'\0'
#define IS_INVALID_HANDLE(_h)   (((_h) == NULL) || ((_h) == INVALID_HANDLE_VALUE))
#define IS_VALID_HANDLE(_h)     (((_h) != NULL) && ((_h) != INVALID_HANDLE_VALUE))
#define IS_EMPTY_WCS(_s)        (((_s) == NULL) || (*(_s) == WNUL))
#define IS_EMPTY_STR(_s)        (((_s) == NULL) || (*(_s) == '\0'))
#define DSIZEOF(_s)             (DWORD)(sizeof(_s))

#define SVCBATCH_CS_CREATE(_o)  if (_o) InitializeCriticalSection(&((_o)->csLock))
#define SVCBATCH_CS_DELETE(_o)  if (_o) DeleteCriticalSection(&((_o)->csLock))
#define SVCBATCH_CS_ENTER(_o)   if (_o) EnterCriticalSection(&((_o)->csLock))
#define SVCBATCH_CS_LEAVE(_o)   if (_o) LeaveCriticalSection(&((_o)->csLock))

/**
 * Assertion macros
 */
#define ASSERT_WSTR(_s, _r)                                 \
    if (((_s) == NULL) || (*(_s) == WNUL)) {                \
        SetLastError(ERROR_INVALID_PARAMETER);              \
        return (_r);                                        \
    } (void)0

#define ASSERT_CSTR(_s, _r)                                 \
    if (((_s) == NULL) || (*(_s) == 0)) {                   \
        SetLastError(ERROR_INVALID_PARAMETER);              \
        return (_r);                                        \
    } (void)0

#define ASSERT_ZERO(_v, _r)                                 \
    if ((_v) <= 0) {                                        \
        SetLastError(ERROR_INVALID_PARAMETER);              \
        return (_r);                                        \
    } (void)0

#define ASSERT_TRUE(_v, _r)                                 \
    if ((_v) != 0) {                                        \
        SetLastError(ERROR_INVALID_PARAMETER);              \
        return (_r);                                        \
    } (void)0

#define ASSERT_SPAN(_v, _m, _x, _r)                         \
    if (((_v) < (_m)) || ((_v) > (_x))) {                   \
        SetLastError(ERROR_NO_RANGES_PROCESSED);            \
        return (_r);                                        \
    } (void)0

#define ASSERT_NULL(_p, _r)                                 \
    if ((_p) == NULL) {                                     \
        SetLastError(ERROR_INVALID_ADDRESS);                \
        return (_r);                                        \
    } (void)0

#define ASSERT_SIZE(_s, _m, _r)                             \
    if ((_s) < (_m)) {                                      \
        SetLastError(ERROR_INSUFFICIENT_BUFFER);            \
        return (_r);                                        \
    } (void)0

#define ASSERT_HANDLE(_h, _r)                               \
    if (((_h) == NULL) || ((_h) == INVALID_HANDLE_VALUE)) { \
        SetLastError(ERROR_INVALID_HANDLE);                 \
        return (_r);                                        \
    } (void)0



#define SAFE_CLOSE_HANDLE(_h)                               \
    if (((_h) != NULL) && ((_h) != INVALID_HANDLE_VALUE))   \
        CloseHandle((_h));                                  \
    (_h) = NULL

#define SAFE_MEM_FREE(_m)                                   \
    if ((_m) != NULL)                                       \
        free((_m));                                         \
    (_m) = NULL

#define WAIT_OBJECT_1          (WAIT_OBJECT_0 + 1)
#define WAIT_OBJECT_2          (WAIT_OBJECT_0 + 2)
#define WAIT_OBJECT_3          (WAIT_OBJECT_0 + 3)


/**
 * Error macros
 */
#define SVCBATCH_FATAL(_e)      xfatalerr("    Fatal error in svcbatch.c"   \
                                          " at line #" CPP_TOSTR(__LINE__), (_e))


#if defined(_DEBUG)
# if defined(_MSC_VER)
#  define SVCBATCH_BUILD_CC     " (msc " CPP_TOSTR(_MSC_FULL_VER)  "."  \
                                CPP_TOSTR(_MSC_BUILD)  ")"
# elif defined(__GNUC__)
#  define SVCBATCH_BUILD_CC     " (gcc " CPP_TOSTR(__GNUC__)       "."  \
                                CPP_TOSTR(__GNUC_MINOR__)          "."  \
                                CPP_TOSTR(__GNUC_PATCHLEVEL__)     ")"
# else
#  define SVCBATCH_BUILD_CC     ""
# endif
#else
# define SVCBATCH_BUILD_CC      ""
#endif

#if defined(VERSION_SFX)
# define SVCBATCH_VERSION_SFX   CPP_TOSTR(VERSION_SFX)
#else
# define SVCBATCH_VERSION_SFX   ""
#endif

#if defined(_DEBUG)
# if SVCBATCH_ISDEV_VERSION
#  define SVCBATCH_VERSION_DBG  "_2.dbg"
# else
#  define SVCBATCH_VERSION_DBG  "_1.dbg"
# endif
#else
# if SVCBATCH_ISDEV_VERSION
#  define SVCBATCH_VERSION_DBG  "_1.dev"
# else
#  define SVCBATCH_VERSION_DBG  ""
# endif
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
                                CPP_TOSTR(SVCBATCH_PATCH_VERSION) "."   \
                                CPP_TOSTR(SVCBATCH_MICRO_VERSION)


#define SVCBATCH_VERSION_WCS \
                                CPP_TOWCS(SVCBATCH_MAJOR_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_MINOR_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_PATCH_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_MICRO_VERSION)       \
                                CPP_WIDEN(SVCBATCH_VERSION_SFX)         \
                                CPP_WIDEN(SVCBATCH_VERSION_DBG)


#define SVCBATCH_VERSION_TXT  \
                                SVCBATCH_VERSION_STR                    \
                                SVCBATCH_VERSION_SFX                    \
                                SVCBATCH_VERSION_DBG                    \
                                SVCBATCH_BUILD_CC

#define SVCBATCH_PROJECT_URL \
    "https://github.com/mturk/svcbatch"

#define SVCBATCH_DESCRIPTION \
    "Run batch files as Windows Services"

#define SVCBATCH_COPYRIGHT \
    "Copyright (c) 1964-2023 The Acme Corporation or its "              \
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
