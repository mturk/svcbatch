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
 *   C4057: indirection to slightly different base types
 *   C4100: unreferenced formal parameter
 *   C4244: int to char/short - precision loss
 *   C4702: unreachable code
 */
# pragma warning(disable: 4057 4100 4244 4702)
#endif

#if defined(_MSC_VER)
# define CPP_INT64_C(_v)    (_v##I64)
# define CPP_UINT64_C(_v)   (_v##UI64)
# define INT64_ZERO         0I64
# define UINT64_ZERO        0UI64
#else
# define CPP_INT64_C(_v)    (_v##LL)
# define CPP_UINT64_C(_v)   (_v##ULL)
# define INT64_ZERO         0LL
# define UINT64_ZERO        0ULL
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
#define SVCBATCH_MAJOR_VERSION  3
#define SVCBATCH_MINOR_VERSION  0
#define SVCBATCH_PATCH_VERSION  1
#define SVCBATCH_MICRO_VERSION  0

#define SVCBATCH_RELEASE_VER    (SVCBATCH_MAJOR_VERSION * 10000 + SVCBATCH_MINOR_VERSION * 100 + SVCBATCH_PATCH_VERSION)
/**
 * Set to zero for release versions
 */
#define SVCBATCH_ISDEV_VERSION  1


#define SVCBATCH_NAME           "SvcBatch"
#define SVCBATCH_APPNAME        "SvcBatch Service"
#define SHUTDOWN_APPNAME        "SvcBatch Shutdown"
#define SVCBATCH_LOGNAME       L"SvcBatch.log"
#define SVCBATCH_LOGSTOP       L"SvcBatch.stop.log"
#define SVCBATCH_LOGSDIR       L"Logs"
#define SVCBATCH_PIPEPFX       L"\\\\.\\pipe\\pp-"
#define SVCBATCH_MMAPPFX       L"\\\\Local\\mm-"

/**
 * Registry value name where SvcBatch store
 * service arguments
 */
#define SVCBATCH_SVCARGS       L"ImagePathArguments"

/**
 * Default arguments for cmd.exe
 *
 * /D     Disable execution of AutoRun commands from registry
 * /E:ON  Enable command extensions
 *        In a batch file, the SETLOCAL ENABLEEXTENSIONS
          or DISABLEEXTENSIONS arguments takes precedence
 *        over this switch.
 *
 * /V:OFF Disable delayed environment expansion.
 *        In a batch file the SETLOCAL ENABLEDELAYEDEXPANSION
 *        or DISABLEDELAYEDEXPANSION arguments takes precedence
 *        over this switch.
 *
 */
#define SVCBATCH_DEF_ARGS      L"/D /E:ON /V:OFF /C"

/**
 * Maximum number of the SvcBatch.log.N files
 */
#define SVCBATCH_MAX_LOGS       9

/**
 * Default number of the SvcBatch.log.N
 * and SvcBatch.shutdown.log.N files
 */
#define SVCBATCH_DEF_LOGS       2

/**
 * Maximum number of arguments for service and
 * shutdown batch file.
 */
#define SVCBATCH_MAX_ARGS       32

/**
 * Maximum length for the path and
 * file names. Although the C Runtime
 * supports path lengths up to 32768 characters
 * in length, we set the limit to more
 * realistic number.
 */
#define SVCBATCH_PATH_MAX       2048

/**
 * Maximum safe path length that
 * allows inserting \\?\ for long paths.
 * This value must be SVCBATCH_PATH_MAX - 8
 */
#define SVCBATCH_PATH_SIZ       2040

/**
 * Maximum length for the object names.
 *
 * Although some object names are limited
 * to MAX_PATH (260 characters), we limit that
 * to _MAX_FNAME (256) characters
 */
#define SVCBATCH_NAME_MAX       256

/**
 * Size of the pipe read buffer in bytes.
 */
#define SVCBATCH_PIPE_LEN       8192

/**
 * Maximum number of characters in one line
 * written to the file or event log
 */
#define SVCBATCH_LINE_MAX       2048

/**
 * Timing definitions
 */
#define SVCBATCH_START_HINT     10000
#define SVCBATCH_STOP_HINT      20000
#define SVCBATCH_STOP_WAIT      5000
#define SVCBATCH_STOP_SYNC      2000
#define SVCBATCH_STOP_STEP      1000
#define SVCBATCH_STOP_TMIN      2
#define SVCBATCH_STOP_TMAX      180

/**
 * Default stop timeout in milliseconds
 */
#define SVCBATCH_STOP_TIMEOUT   10000

/**
 * Custom SCM control code that
 * will send a signal to rotate the log files
 * if log rotation is enabled
 *
 * eg. C:\>sc control SvcBatchServiceName 234
 *
 * Check documentation for more details
 */
#define SVCBATCH_CTRL_ROTATE    234

/**
 * Minimum rotate size in kilobytes
 */
#define SVCBATCH_MIN_ROTATE_SIZ 1

/**
 * Minimum time between two log
 * rotations in minutes
 */
#define SVCBATCH_MIN_ROTATE_INT 2
#define SVCBATCH_ROTATE_READY   120000

/**
 * Service manager default wait timeout
 * in seconds
 */
#define SVCBATCH_SCM_WAIT_DEF   30

/**
 * Maximum process tree kill depth
 */
#define SVCBATCH_MAX_KILLDEPTH  4


#define ONE_SECOND              1000
#define MS_IN_DAY               86400000
#define MS_IN_SECOND            CPP_INT64_C(1000)
#define MS_IN_MINUTE            CPP_INT64_C(60000)
#define MS_IN_HOUR              CPP_INT64_C(3600000)
#define ONE_MINUTE              CPP_INT64_C(600000000)
#define ONE_HOUR                CPP_INT64_C(36000000000)
#define ONE_DAY                 CPP_INT64_C(864000000000)
#define KILOBYTES(_x)           (CPP_INT64_C(_x) * CPP_INT64_C(1024))
#define MEGABYTES(_x)           (CPP_INT64_C(_x) * CPP_INT64_C(1048576))

/** Memory alignment */
#define MEM_ALIGN(size, boundary) \
    (((size) + ((boundary) - 1)) & ~((boundary) - 1))
#define MEM_ALIGN_DEFAULT(size) MEM_ALIGN(size, 8)

/**
 * Start of the custom error messages
 */
#define SVCBATCH_START_ERROR            90000
#define SVCBATCH_EEINVAL                (SVCBATCH_START_ERROR +  1)

/**
 * Process state
 */
#define SVCBATCH_PROCESS_STOPPED    0x00000001  /* The process is not running   */
#define SVCBATCH_PROCESS_STARTING   0x00000002  /* The process is starting      */
#define SVCBATCH_PROCESS_STOPPING   0x00000003  /* The process is stopping      */
#define SVCBATCH_PROCESS_RUNNING    0x00000004  /* The process is running       */

/**
 * Runtime options
 */
#define SVCBATCH_OPT_WRPIPE         0x00000001   /* Write data to stdin on run  */
#define SVCBATCH_OPT_LOCALTIME      0x00000002   /* Use local time              */
#define SVCBATCH_OPT_QUIET          0x00000004   /* Disable logging             */
#define SVCBATCH_OPT_CTRL_BREAK     0x00000008   /* Send CTRL_BREAK on stop     */
#define SVCBATCH_OPT_NOENV          0x00000010   /* Do not set private envvars  */
#define SVCBATCH_OPT_LONGPATHS      0x00000020   /* Use LongPathsEnabled        */
#define SVCBATCH_OPT_MASK           0x0000FFFF

#define SVCBATCH_OPT_ROTATE         0x00010000   /* Enable log rotation         */
#define SVCBATCH_OPT_TRUNCATE       0x00020000   /* Truncate log on rotation    */
#define SVCBATCH_OPT_ROTATE_BY_SIG  0x00100000   /* Rotate by signal            */
#define SVCBATCH_OPT_ROTATE_BY_SIZE 0x00200000   /* Rotate by size              */
#define SVCBATCH_OPT_ROTATE_BY_TIME 0x00400000   /* Rotate by time              */

#define SVCBATCH_FAIL_NONE      1   /* Do not set error if run ends without stop        */
#define SVCBATCH_FAIL_ERROR     2   /* Set service error if run endeded without stop    */
#define SVCBATCH_FAIL_EXIT      3   /* Call exit() on stop without scm CTRL_STOP        */

/**
 * Helper macros
 */
#define WNUL                    L'\0'
#define CNUL                     '\0'
#define IS_INVALID_HANDLE(_h)   (((_h) == NULL) || ((_h) == INVALID_HANDLE_VALUE))
#define IS_VALID_HANDLE(_h)     (((_h) != NULL) && ((_h) != INVALID_HANDLE_VALUE))
#define IS_EMPTY_WCS(_s)        (((_s) == NULL) || (*(_s) == WNUL))
#define IS_EMPTY_STR(_s)        (((_s) == NULL) || (*(_s) == CNUL))
#define IS_VALID_WCS(_s)        (((_s) != NULL) && (*(_s) != WNUL))

#define IS_SET(_v, _o)          (((_v) & (_o)) == (_o))
#define IS_NOT(_v, _o)          (((_v) & (_o)) != (_o))

#define IS_OPT_SET(_o)          ((svcoptions & (_o)) == (_o))
#define IS_NOT_OPT(_o)          ((svcoptions & (_o)) != (_o))
#define OPT_SET(_o)             svcoptions |=  (_o)

#define DSIZEOF(_s)             (DWORD)(sizeof(_s))

#define SVCBATCH_CS_INIT(_o)    if (_o) InitializeCriticalSection(&((_o)->cs))
#define SVCBATCH_CS_CLOSE(_o)   if (_o) DeleteCriticalSection(&((_o)->cs))
#define SVCBATCH_CS_ENTER(_o)   if (_o) EnterCriticalSection(&((_o)->cs))
#define SVCBATCH_CS_LEAVE(_o)   if (_o) LeaveCriticalSection(&((_o)->cs))

/**
 * Assertion macros
 */
#define ASSERT_WSTR(_s, _r)                                 \
    if (((_s) == NULL) || (*(_s) == WNUL)) {                \
        SetLastError(ERROR_INVALID_PARAMETER);              \
        return (_r);                                        \
    } (void)0

#define ASSERT_CSTR(_s, _r)                                 \
    if (((_s) == NULL) || (*(_s) == CNUL)) {                \
        SetLastError(ERROR_INVALID_PARAMETER);              \
        return (_r);                                        \
    } (void)0

#define ASSERT_ZERO(_v, _r)                                 \
    if ((_v) <= 0) {                                        \
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

#define ASSERT_LESS(_v, _s, _r)                             \
    if ((_v) >= (_s)) {                                     \
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
        CloseHandle(_h);                                    \
    (_h) = NULL

#define SAFE_MEM_FREE(_m)                                   \
    xfree(_m);                                              \
    (_m) = NULL

#define WAIT_OBJECT_1          (WAIT_OBJECT_0 + 1)
#define WAIT_OBJECT_2          (WAIT_OBJECT_0 + 2)
#define WAIT_OBJECT_3          (WAIT_OBJECT_0 + 3)

#ifndef DWORD_MAX
#define DWORD_MAX               0xffffffffUL  /* maximum DWORD value */
#endif

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

#if defined(VERSION_SFX)
# define SVCBATCH_VERSION_SFX   CPP_TOSTR(VERSION_SFX)
#else
# define SVCBATCH_VERSION_SFX   SVCBATCH_VERSION_DBG
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

#define SVCBATCH_VERSION_VER \
                                CPP_TOWCS(SVCBATCH_MAJOR_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_MINOR_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_PATCH_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_MICRO_VERSION)


#define SVCBATCH_VERSION_EXP \
                                CPP_TOWCS(SVCBATCH_MAJOR_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_MINOR_VERSION) L"."  \
                                CPP_TOWCS(SVCBATCH_PATCH_VERSION)

#define SVCBATCH_VERSION_REL \
                                CPP_TOWCS(SVCBATCH_MICRO_VERSION)       \
                                CPP_WIDEN(SVCBATCH_VERSION_SFX)

#define SVCBATCH_VERSION_WCS \
                                SVCBATCH_VERSION_VER                    \
                                CPP_WIDEN(SVCBATCH_VERSION_SFX)


#define SVCBATCH_VERSION_TXT  \
                                SVCBATCH_VERSION_STR                    \
                                SVCBATCH_VERSION_SFX                    \
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
