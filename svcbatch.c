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

#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <errno.h>
#include "svcbatch.h"

#if defined (_DEBUG)
#include <crtdbg.h>

static void     dbgprintf(LPCSTR, int, LPCSTR, ...);
static void     dbgprints(LPCSTR, int, LPCSTR);
static DWORD    dbgfopen(void);
static int      dbgsvcmode = 0;
static volatile HANDLE  dbgfile = NULL;
static CRITICAL_SECTION dbglock;

static const char *dbgsvcmodes[] = {
    "UNKNOWN",
    "SERVICE",
    "STOPSVC",
    "MANAGER"
};

# define DBG_FILE_NAME          L"_debug.log"
# define DBG_PRINTF(Fmt, ...)   dbgprintf(__FUNCTION__, __LINE__, Fmt, ##__VA_ARGS__)
# define DBG_PRINTS(Msg)        dbgprints(__FUNCTION__, __LINE__, Msg)
#else
# define DBG_PRINTF(Fmt, ...)   (void)0
# define DBG_PRINTS(Msg)        (void)0
#endif

#define xsyserrno(_i, _d, _p)   svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_ERROR_TYPE,       0, wcsmessages[_i], _d, _p)
#define xsyserror(_n, _d, _p)   svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_ERROR_TYPE,      _n, NULL, _d, _p)
#define xsyswarn(_n, _e, _d)    svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_WARNING_TYPE,    _n, _e, _d,  NULL)
#define xsysinfo(_e, _d)        svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_INFORMATION_TYPE, 0, _e, _d,  NULL)
#define xsvcstatus(_s, _p)      reportsvcstatus(__FUNCTION__, __LINE__, _s, _p)

#define SZ_STATUS_PROCESS_INFO  sizeof(SERVICE_STATUS_PROCESS)
#define SYSTEM_SVC_SUBKEY       L"SYSTEM\\CurrentControlSet\\Services"
#define IS_LEAP_YEAR(_y)        ((!(_y % 4)) ? (((_y % 400) && !(_y % 100)) ? 0 : 1) : 0)

typedef enum {
    SVCBATCH_WORKER_THREAD = 0,
    SVCBATCH_WRPIPE_THREAD,
    SVCBATCH_STOP_THREAD,
    SVCBATCH_ROTATE_THREAD,
    SVCBATCH_MAX_THREADS
} SVCBATCH_THREAD_ID;

typedef struct _SVCBATCH_THREAD {
    volatile HANDLE        thread;
    volatile LONG          started;
    LPTHREAD_START_ROUTINE startAddress;
    LPVOID                 parameter;
    DWORD                  id;
    DWORD                  exitCode;
#if defined(_DEBUG)
    ULONGLONG              duration;
    LPCSTR                 name;
#endif
} SVCBATCH_THREAD, *LPSVCBATCH_THREAD;

typedef struct _SVCBATCH_PIPE {
    OVERLAPPED  o;
    HANDLE      pipe;
    BYTE        buffer[SVCBATCH_PIPE_LEN];
    DWORD       read;
    DWORD       state;
} SVCBATCH_PIPE, *LPSVCBATCH_PIPE;

typedef struct _SVCBATCH_PROCESS {
    volatile LONG       state;
    PROCESS_INFORMATION pInfo;
    STARTUPINFOW        sInfo;
    DWORD               ppid;
    DWORD               exitCode;
    DWORD               argc;
    DWORD               optc;
    LPWSTR              commandLine;
    LPWSTR              application;
    LPWSTR              script;
    LPWSTR              directory;
    LPWSTR              name;
    LPCWSTR             args[SVCBATCH_MAX_ARGS];
    LPCWSTR             opts[SVCBATCH_MAX_ARGS];
} SVCBATCH_PROCESS, *LPSVCBATCH_PROCESS;

typedef struct _SVCBATCH_SERVICE {
    volatile LONG           state;
    SERVICE_STATUS_HANDLE   handle;
    SERVICE_STATUS          status;
    CRITICAL_SECTION        cs;

    LPCWSTR                 name;
    LPWSTR                  base;
    LPWSTR                  home;
    LPWSTR                  temp;
    LPWSTR                  uuid;
    LPWSTR                  work;
    LPWSTR                  logs;
} SVCBATCH_SERVICE, *LPSVCBATCH_SERVICE;

typedef struct _SVCBATCH_LOG {
    volatile LONG64     size;
    volatile HANDLE     fd;
    volatile LONG       state;
    volatile LONG       count;
    CRITICAL_SECTION    cs;
    LPCWSTR             logName;
    LPWSTR              logFile;

} SVCBATCH_LOG, *LPSVCBATCH_LOG;

/**
 * Length of the shared memory data.
 * Adjust this number so that SVCBATCH_IPC
 * structure aligns to 64K
 */
#define SVCBATCH_DATA_LEN   32612
typedef struct _SVCBATCH_IPC {
    DWORD   ppid;
    DWORD   options;
    DWORD   timeout;
    DWORD   killdepth;
    DWORD   maxlogs;

    DWORD   application;
    DWORD   script;
    DWORD   name;
    DWORD   temp;
    DWORD   work;
    DWORD   logs;
    DWORD   logn;

    DWORD   argc;
    DWORD   optc;
    DWORD   args[SVCBATCH_MAX_ARGS];
    DWORD   opts[SVCBATCH_MAX_ARGS];

    WCHAR   data[SVCBATCH_DATA_LEN];
} SVCBATCH_IPC, *LPSVCBATCH_IPC;

typedef struct _SVCBATCH_NAME_MAP {
    LPCWSTR name;
    DWORD   code;
} SVCBATCH_NAME_MAP, *LPSVCBATCH_NAME_MAP;

static int                   svcmainargc = 0;
static int                   srvcmaxlogs = SVCBATCH_DEF_LOGS;
static int                   stopmaxlogs = 0;
static LPCWSTR              *svcmainargv = NULL;

static LPSVCBATCH_SERVICE    service     = NULL;
static LPSVCBATCH_PROCESS    program     = NULL;
static LPSVCBATCH_PROCESS    cmdproc     = NULL;
static LPSVCBATCH_PROCESS    svcstop     = NULL;
static LPSVCBATCH_LOG        outputlog   = NULL;
static LPSVCBATCH_IPC        sharedmem   = NULL;

static volatile LONG         killdepth      = 0;
static LONGLONG              rotateinterval = CPP_INT64_C(0);
static LONGLONG              rotatesize     = CPP_INT64_C(0);
static LARGE_INTEGER         rotatetime     = {{ 0, 0 }};

static int       servicemode    = 0;
static DWORD     svcoptions     = SVCBATCH_OPT_ENV;
static DWORD     preshutdown    = 0;
static int       stoptimeout    = SVCBATCH_STOP_TIMEOUT;
static int       svcfailmode    = SVCBATCH_FAIL_ERROR;
static HANDLE    svcstopfile    = NULL;
static HANDLE    svcstopssig    = NULL;
static HANDLE    stopstarted    = NULL;
static HANDLE    svcstopdone    = NULL;
static HANDLE    workerended    = NULL;
static HANDLE    dologrotate    = NULL;
static HANDLE    sharedmmap     = NULL;
static LPCWSTR   stoplogname    = NULL;

static SVCBATCH_THREAD threads[SVCBATCH_MAX_THREADS];

static WCHAR        zerostring[]  = {  0,  0,  0,  0 };
static WCHAR        CRLFW[]       = { 13, 10,  0,  0 };
static BYTE         YYES[]        = { 89, 13, 10,  0 };

static LPCSTR  cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;
static LPCWSTR uenvprefix  = SVCBATCH_ENVNAME;
static LPCWSTR xenvprefix  = NULL;

static int     xwoptend    = 0;
static int     xwoptarr    = 0;
static int     xwoptind    = 1;
static LPCWSTR xwoptarg    = NULL;
static LPCWSTR xwoption    = NULL;

/**
 * Service Manager types
 *
 */
typedef enum {
    SVCBATCH_SCM_CREATE = 0,
    SVCBATCH_SCM_CONFIG,
    SVCBATCH_SCM_CONTROL,
    SVCBATCH_SCM_DELETE,
    SVCBATCH_SCM_HELP,
    SVCBATCH_SCM_START,
    SVCBATCH_SCM_STOP,
    SVCBATCH_SCM_VERSION
} SVCBATCH_SCM_CMD;

static const wchar_t *scmsvcaccounts[] = {
    L".\\LocalSystem",
    L"NT AUTHORITY\\LocalService",
    L"NT AUTHORITY\\NetworkService"
};

static const wchar_t *scmcommands[] = {
    L"Create",                  /* SVCBATCH_SCM_CREATE      */
    L"Config",                  /* SVCBATCH_SCM_CONFIG      */
    L"Control",                 /* SVCBATCH_SCM_CONTROL     */
    L"Delete",                  /* SVCBATCH_SCM_DELETE      */
    L"Help",                    /* SVCBATCH_SCM_HELP        */
    L"Start",                   /* SVCBATCH_SCM_START       */
    L"Stop",                    /* SVCBATCH_SCM_STOP        */
    L"Version",                 /* SVCBATCH_SCM_VERSION     */
    NULL
};

static const wchar_t *scmallowed[] = {
    L"qbdDnpPsu",          /* SVCBATCH_SCM_CREATE      */
    L"qbdDnpPsu",          /* SVCBATCH_SCM_CONFIG      */
    L"q",                  /* SVCBATCH_SCM_CONTROL     */
    L"q",                  /* SVCBATCH_SCM_DELETE      */
    L"x",                  /* SVCBATCH_SCM_HELP        */
    L".qw",                /* SVCBATCH_SCM_START       */
    L".qw",                /* SVCBATCH_SCM_STOP        */
    L"x",                  /* SVCBATCH_SCM_VERSION     */
    NULL
};


static const wchar_t *scmdoptions = L"bcefhkmnorstw";


/**
 * Long options ...
 *
 * <option><options><option name>
 *
 * option:          Any alphanumeric character
 * options:         '.' Option without argument
 *                  '+' Argument can be part of the option separated by ':' or '='
 *                      If the option does not end with ':' or '=', the argument is next option
 *                  ':' Argument must be part of the option separated by ':' or '='
 *                      If the option does not end with ':' or '=', or the argument is
 *                      is empty after skipping blanks, returns ENOENT
 *                  '?' Argument is optional and it must be part of the
 *                      current option, separated by ':' or '='
 *
 */

static const wchar_t *scmcoptions[] = {
    L"b+binpath",
    L"d+description",
    L"D+depend",
    L"n+displayname",
    L"p+password",
    L"P+privs",
    L"q.quiet",
    L"s+start",
    L"u+username",
    L"w?wait",
    NULL
};

static const SVCBATCH_NAME_MAP starttypemap[] = {
    { L"Automatic", SERVICE_AUTO_START      },
    { L"Auto",      SERVICE_AUTO_START      },
    { L"Manual",    SERVICE_DEMAND_START    },
    { L"Demand",    SERVICE_DEMAND_START    },
    { L"Disabled",  SERVICE_DISABLED        },
    { NULL,         0                       }
};

static const char *xgenerichelp =
    "\nUsage:\n  " SVCBATCH_NAME " [command] [service name] <option1> <option2>...\n"           \
    "\n    Commands:"                                                                           \
    "\n      Create.....Creates a service."                                                     \
    "\n      Config.....Changes the configuration of a service."                                \
    "\n      Control....Sends a control to a service."                                          \
    "\n      Delete.....Deletes a service."                                                     \
    "\n      Help.......Print this screen and exit."                                            \
    "\n                 Use Help [command] for command help."                                   \
    "\n      Start......Starts a service."                                                      \
    "\n      Stop.......Sends a STOP request to a service."                                     \
    "\n      Version....Print version information."                                             \
    "\n";

static const char *xcommandhelp[] = {
    /* Create */
    "\nDescription:\n  Creates a service entry in the registry and Service Database."           \
    "\nUsage:\n  " SVCBATCH_NAME " create [service name] <options ...> <[-] arguments ...>\n"   \
    "\n    Options:"                                                                            \
    "\n      --binPath      BinaryPathName to the .exe file."                                   \
    "\n      --depend       Dependencies (separated by / (forward slash))."                     \
    "\n      --description  Sets the description of a service."                                 \
    "\n      --displayName  Sets the service display name."                                     \
    "\n      --privs        Sets the required privileges of a service."                         \
    "\n      --start        Sets the service startup type."                                     \
    "\n                     <auto|manual|disabled> (default = manual)."                         \
    "\n      --username     The name of the account under which the service should run."        \
    "\n                     Default is LocalSystem account."                                    \
    "\n      --password     The password to the account name specified by the"                  \
    "\n                     username parameter."                                                \
    "\n",
    /* Config */
    "\nDescription:\n  Modifies a service entry in the registry and Service Database."          \
    "\nUsage:\n  " SVCBATCH_NAME " config [service name] <options ...> <[-] arguments ...>\n"   \
    "\n    Options:"                                                                            \
    "\n      --binPath      BinaryPathName to the .exe file."                                   \
    "\n      --depend       Dependencies (separated by / (forward slash))."                     \
    "\n      --description  Sets the description of a service."                                 \
    "\n      --displayName  Sets the service display name."                                     \
    "\n      --privs        Sets the required privileges of a service."                         \
    "\n      --start        Sets the service startup type."                                     \
    "\n                     <auto|manual|disabled> (default = manual)."                         \
    "\n      --username     The name of the account under which the service should run."        \
    "\n                     Default is LocalSystem account."                                    \
    "\n      --password     The password to the account name specified by the"                  \
    "\n                     username parameter."                                                \
    "\n",
    /* Control */
    "\nDescription:\n  Sends a CONTROL code to a service."                                      \
    "\nUsage:\n  " SVCBATCH_NAME " control [service name] <value>\n"                            \
    "\n    Value:"                                                                              \
    "\n      User defined control code (128 ... 255)"                                           \
    "\n",
    /* Delete */
    "\nDescription:\n  Deletes a service entry from the registry."                              \
    "\nUsage:\n  " SVCBATCH_NAME " delete [service name]\n"                                     \
    "\n",
    /* Help */
    "\nDescription:\n  Display command help."                                                   \
    "\nUsage:\n  " SVCBATCH_NAME " help <command>\n"                                            \
    "\n",
    /* Start */
    "\nDescription:\n  Starts a service running."                                               \
    "\nUsage:\n  " SVCBATCH_NAME " start [service name] <options ...> <arguments ...>\n"        \
    "\n    Options:"                                                                            \
    "\n      --wait[:seconds]   Wait up to seconds until the service"                           \
    "\n                         enters the RUNNING state."                                      \
    "\n",
    /* Stop */
    "\nDescription:\n  Sends a STOP control request to a service."                              \
    "\nUsage:\n  " SVCBATCH_NAME " stop [service name] <options ...> <reason> <comment>\n"      \
    "\n    Options:"                                                                            \
    "\n      --wait[:seconds]   Wait up to seconds for service to stop."                        \
    "\n",
    /* Version */
    "\nDescription:\n  Display version information."                                            \
    "\nUsage:\n  " SVCBATCH_NAME " version\n"                                                   \
    "\n",
    NULL,
};


/**
 * Message strings
 */
static const wchar_t *wcsmessages[] = {
    L"Unknown",
    L"Service stopped without SERVICE_CONTROL_STOP signal",                 /*  1 */
    L"The service is not in the RUNNING state",                             /*  2 */
    L"Reserved for the future use",                                         /*  3 */
    NULL,                                                                   /*  4 */
    NULL,                                                                   /*  5 */
    NULL,                                                                   /*  6 */
    NULL,                                                                   /*  7 */
    NULL,                                                                   /*  8 */
    NULL,                                                                   /*  9 */
    L"The %s command option is already defined",                            /* 10 */
    L"The %s command option value is empty",                                /* 11 */
    L"The %s command option value is invalid",                              /* 12 */
    L"The %s command option value is outside valid range",                  /* 13 */
    L"The %s command option value contains invalid characters",             /* 14 */
    L"The %s command option value is set to the invalid directory",         /* 15 */
    L"Too many arguments for the %s command option",                        /* 16 */
    L"Too many %s arguments",                                               /* 17 */
    L"The %s command was not initialized",                                  /* 18 */
    L"The %s is invalid",                                                   /* 19 */
    L"The %s contains invalid filename characters",                         /* 20 */
    L"The %s are mutually exclusive",                                       /* 21 */
    L"Unknown command option",                                              /* 22 */
    L"The /\\:;<>?*|\" are not valid service name characters",              /* 23 */
    L"The maximum service name length is 256 characters",                   /* 24 */
    L"Stop the service and call Delete again",                              /* 25 */
    L"The Control code is missing. Use control [service name] <value>",     /* 26 */

    NULL
};

#define SVCBATCH_MSG(_id) wcsmessages[_id]

static int xfatalerr(LPCSTR func, int err)
{

    OutputDebugStringA(">>> " SVCBATCH_NAME " " SVCBATCH_VERSION_STR);
    OutputDebugStringA(func);
    OutputDebugStringA("<<<\n\n");
    _exit(err);
    TerminateProcess(GetCurrentProcess(), err);

    return err;
}

static void *xmmalloc(size_t size)
{
    LONG64 *p;
    size_t  n;

    n = MEM_ALIGN_DEFAULT(size);
    p = (LONG64 *)malloc(n);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
        return NULL;
    }
    n = (n >> 3) - 1;
    *(p + n) = INT64_ZERO;
    return p;
}

static void *xmcalloc(size_t size)
{
    void   *p;
    size_t  n;

    n = MEM_ALIGN_DEFAULT(size);
    p = calloc(1, n);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    return p;
}

static void *xrealloc(void *mem, size_t size)
{
    LONG64 *p;
    size_t  n;

    if (size == 0) {
        if (mem)
            free(mem);
        return NULL;
    }
    n = MEM_ALIGN_DEFAULT(size);
    p = (LONG64 *)realloc(mem, n);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
        return NULL;
    }
    n = (n >> 3) - 1;
    *(p + n) = INT64_ZERO;
    return p;
}

static __inline LPWSTR  xwmalloc(size_t size)
{
    return (LPWSTR)xmmalloc(size * sizeof(WCHAR));
}

static __inline LPWSTR *xwaalloc(size_t size)
{
    return (LPWSTR *)xmmalloc(size * sizeof(LPWSTR));
}

static __inline void xfree(void *mem)
{
    if (mem)
        free(mem);
}

static __inline void xmemzero(void *mem, size_t number, size_t size)
{
    memset(mem, 0, number * size);
}

static __inline int xwcslen(LPCWSTR s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    else
        return (int)wcslen(s);
}

static __inline int xstrlen(LPCSTR s)
{
    if (IS_EMPTY_STR(s))
        return 0;
    else
        return (int)strlen(s);
}

static __inline int xtolower(int ch)
{
    if ((ch > 64) && (ch < 91))
        return ch + 32;
    else
        return ch;
}

static __inline int xtoupper(int ch)
{
    if ((ch > 96) && (ch < 123))
        return ch - 32;
    else
        return ch;
}

static __inline void xwcslower(LPWSTR str)
{
    for (; *str != 0; str++)
        *str = xtolower(*str);
}

static __inline void xwcsupper(LPWSTR str)
{
    for (; *str != 0; str++)
        *str = xtoupper(*str);
}

static __inline int xisblank(int ch)
{
    if ((ch == 32) || (ch == 9))
        return 1;
    else
        return 0;
}

static __inline int xisalpha(int ch)
{
    if (((ch > 64) && (ch < 91)) || ((ch > 96) && (ch < 123)))
        return 1;
    else
        return 0;
}

static __inline int xisdigit(int ch)
{
    if ((ch > 47) && (ch < 58))
        return 1;
    else
        return 0;
}

static __inline int xiswcschar(LPCWSTR s, WCHAR c)
{
    if ((s[0] == c) && (s[1] == WNUL))
        return 1;
    else
        return 0;
}

static __inline void xchrreplace(LPWSTR str, WCHAR a, WCHAR b)
{
    for (; *str != WNUL; str++) {
        if (*str == a)
            *str =  b;
    }
}

static __inline void xwinpathsep(LPWSTR str)
{
    for (; *str != WNUL; str++) {
        if (*str == L'/')
            *str = L'\\';
    }
}

static __inline void xunxpathsep(LPWSTR str)
{
    for (; *str != WNUL; str++) {
        if (*str == L'\\')
            *str = L'/';
    }
}

static __inline LPWSTR xwcschr(LPCWSTR str, int c)
{
    ASSERT_WSTR(str, NULL);

    while (*str) {
        if (*str == c)
            return (LPWSTR)str;
        str++;
    }
    return NULL;
}

static __inline LPWSTR xwcsrchr(LPCWSTR str, int c)
{
    LPCWSTR s = str;

    ASSERT_WSTR(s, NULL);
    while (*s)
        s++;
    while (--s >= str) {
        if (*s == c)
            return (LPWSTR)s;
    }
    return NULL;
}

static int xisvalidvarname(LPCWSTR n)
{
    for (; *n != WNUL; n++) {
        if (!isalnum(*n) && (*n != L'_'))
            return 0;
    }
    return 1;
}

static int xfixenvchars(LPWSTR s)
{
    int    c = 0;
    LPWSTR d;

    for (d = s; *s; s++, d++) {
        if (*s == L'@') {
            if (*(s + 1) == L'@')
                *d = *(s++);
            else
                *d = L'%';
        }
        else {
            *d = *s;
        }
        if (*d == L'%')
            c++;
    }
    *d = WNUL;
    return c;
}


static LPWSTR xwcsdup(LPCWSTR s)
{
    size_t n;
    LPWSTR d;

    if (IS_EMPTY_WCS(s))
        return NULL;
    n = wcslen(s);
    d = xwmalloc(n + 1);
    return wmemcpy(d, s, n);
}

static LPWSTR xwcsconcat(LPCWSTR s1, LPCWSTR s2)
{
    LPWSTR rs;
    int    l1;
    int    l2;

    l1 = xwcslen(s1);
    l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;
    rs = xwmalloc(l1 + l2 + 1);

    if (l1 > 0)
        wmemcpy(rs,  s1, l1);
    if (l2 > 0)
        wmemcpy(rs + l1, s2, l2);
    return rs;
}

static LPWSTR xwmakepath(LPCWSTR p, LPCWSTR n, LPCWSTR e)
{
    LPWSTR rs;
    int    lp;
    int    ln;
    int    le;
    int    c;
    int    x = 0;

    lp = xwcslen(p);
    ln = xwcslen(n);
    le = xwcslen(e);

    c = lp + ln + le;
    if (c == 0)
        return NULL;
    if (lp > 0) {
        c += 1;
        if ((c >= MAX_PATH) &&
            (p[0] != L'\\') &&
            (p[1] != L'\\'))
            c += 4;
    }
    rs = xwmalloc(c + 1);

    if (lp > 0) {
        if ((c >= MAX_PATH) &&
            (p[0] != L'\\') &&
            (p[1] != L'\\')) {
            wmemcpy(rs, L"\\\\?\\", 4);
            x += 4;
        }
        wmemcpy(rs + x, p, lp);
        x += lp;
        if (ln > 0)
            rs[x++] = L'\\';
    }
    if (ln > 0) {
        wmemcpy(rs + x, n, ln);
        x += ln;
        if (le > 0)
            wmemcpy(rs + x, e, le);
    }
    xwinpathsep(rs);
    return rs;
}

static LPWSTR xwcspbrk(LPCWSTR str, LPCWSTR set)
{
    LPCWSTR p = str;
    LPCWSTR q;

    ASSERT_WSTR(str, NULL);
    ASSERT_WSTR(set, NULL);
    while (*p) {
        q = set;
        while (*q) {
            if (*p == *q)
                return (LPWSTR)p;
            q++;
        }
        p++;
    }
    return NULL;
}

static LPWSTR xargvtomsz(int argc, LPCWSTR *argv, int *sz)
{
    int    i;
    int    len = 0;
    int    s[SVCBATCH_NAME_MAX];
    LPWSTR ep;
    LPWSTR bp;

    ASSERT_ZERO(argc, NULL);
    ASSERT_LESS(argc, SVCBATCH_NAME_MAX, NULL);

    for (i = 0; i < argc; i++) {
        s[i]  = xwcslen(argv[i]) + 1;
        len  += s[i];
    }
    bp = xwmalloc(++len);
    ep = bp;
    for (i = 0; i < argc; i++) {
        if (s[i] > 1)
            wmemcpy(ep, argv[i], s[i]);
        ep += s[i];
    }
    *ep = WNUL;
    *sz = len * 2;
    return bp;
}

/**
 * Simple atoi with range between 0 and SVCBATCH_INT_MAX.
 * Leading white space characters are ignored.
 * Returns negative value on error.
 */
static int xwcstoi(LPCWSTR sp, LPWSTR *ep)
{
    int rv = 0;
    int dc = 0;

    if (ep != NULL)
        *ep = zerostring;
    ASSERT_WSTR(sp, -1);
    while(xisblank(*sp))
        sp++;
    if (ep != NULL)
        *ep = (LPWSTR)sp;
    while(xisdigit(*sp)) {
        int dv = *sp - L'0';

        if (dv || rv) {
            rv *= 10;
            rv += dv;
        }
        if (rv > SVCBATCH_INT_MAX) {
            SetLastError(ERROR_INVALID_DATA);
            return -1;
        }
        dc++;
        sp++;
    }
    if (dc == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (ep != NULL)
        *ep = (LPWSTR)sp;
    return rv;
}


static LPCWSTR xntowcs(DWORD n)
{
    static WCHAR b[TBUFSIZ];
    LPWSTR s;

    s = b + TBUFSIZ;
    *(--s) = WNUL;
    do {
        *(--s) = L'0' + (WCHAR)(n % 10);
        n /= 10;
    } while (n);

    return s;
}


/**
 * Appends src to string dst of size siz where
 * siz is the full size of dst.
 * At most siz-1 characters will be copied.
 *
 * Always NUL terminates (unless siz == 0).
 * Returns siz if wcslen(initial dst) + wcslen(src),
 * is larger then siz.
 *
 */
static int xwcslcat(LPWSTR dst, int siz, LPCWSTR src)
{
    LPCWSTR s = src;
    LPWSTR  d = dst;
    int     n = siz;
    int     c;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);

    while ((n-- != 0) && (*d != WNUL))
        d++;
    c = (int)(d - dst);
    if (IS_EMPTY_WCS(src))
        return c;
    n = siz - c;
    if (n < 2)
        return siz;
    while ((n-- != 1) && (*s != WNUL))
        *d++ = *s++;

    *d = WNUL;
    if (*s != WNUL)
        d++;
    return (int)(d - dst);
}

static int xwcsncat(LPWSTR dst, int siz, int pos, LPCWSTR src)
{
    LPCWSTR s = src;
    LPWSTR  d = dst + pos;
    int     n;
    int     c;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);

    c = (int)(d - dst);
    if (IS_EMPTY_WCS(src))
        return c;
    n = siz - c;
    if (n < 2)
        return siz;
    while ((n-- != 1) && (*s != WNUL))
        *d++ = *s++;

    *d = WNUL;
    if (*s != WNUL)
        d++;
    return (int)(d - dst);
}

static int xwcslcpy(LPWSTR dst, int siz, LPCWSTR src)
{
    LPCWSTR s = src;
    LPWSTR  d = dst;
    int     n = siz;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);
    *d = WNUL;
    ASSERT_WSTR(src, 0);

    while ((n-- != 1) && (*s != WNUL))
        *d++ = *s++;

    *d = WNUL;
    if (*s != WNUL)
        d++;
    return (int)(d - dst);
}

static int xvsnwprintf(LPWSTR dst, int siz,
                       LPCWSTR fmt, va_list ap)
{
    int c = siz - 1;
    int n;

    ASSERT_WSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 4, 0);

    dst[0] = WNUL;
    n = _vsnwprintf(dst, c, fmt, ap);
    if (n < 0)
        n = 0;
    if (n > c)
        n = c;
    dst[n] = WNUL;
    return n;
}

static int xsnwprintf(LPWSTR dst, int siz, LPCWSTR fmt, ...)
{
    int     rv;
    va_list ap;

    ASSERT_WSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 4, 0);

    va_start(ap, fmt);
    rv = xvsnwprintf(dst, siz, fmt, ap);
    va_end(ap);
    return rv;

}

static int xvsnprintf(char *dst, int siz,
                      LPCSTR fmt, va_list ap)
{
    int c = siz - 1;
    int n;

    ASSERT_CSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 4, 0);

    dst[0] = '\0';
    n = _vsnprintf(dst, c, fmt, ap);
    if (n < 0)
        n = 0;
    if (n > c)
        n = c;
    dst[n] = '\0';
    return n;
}

static int xsnprintf(char *dst, int siz, LPCSTR fmt, ...)
{
    int     rv;
    va_list ap;

    ASSERT_CSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 4, 0);

    va_start(ap, fmt);
    rv = xvsnprintf(dst, siz, fmt, ap);
    va_end(ap);
    return rv;

}

static int xwstartswith(LPCWSTR src, LPCWSTR str)
{
    int pos = 0;
    int sa, sb;

    if (IS_EMPTY_WCS(src))
        return 0;
    while (*src) {
        if (*str == WNUL)
            return pos;
        sa = xtolower(*src++);
        sb = xtolower(*str++);
        if (sa != sb)
            return 0;
        pos++;
    }
    return *str ? 0 : pos;
}

static int xwcsequals(const wchar_t *str, const wchar_t *src)
{
    int sa;

    if (IS_EMPTY_WCS(str))
        return 0;
    while ((sa = xtolower(*str++)) == xtolower(*src++)) {
        if (sa == 0)
            return 1;
    }
    return 0;
}

static LPWSTR xgetenv(LPCWSTR s)
{
    WCHAR  e[BBUFSIZ];
    DWORD  n;
    LPWSTR d = NULL;

    if (IS_EMPTY_WCS(s))
        return NULL;
    n = GetEnvironmentVariableW(s, e, BBUFSIZ);
    if (n == 0)
        return NULL;
    d = xwmalloc(n);
    if (n >= BBUFSIZ)
        GetEnvironmentVariableW(s, d, n);
    else
        wmemcpy(d, e, n);
    return d;
}

static LPWSTR xexpandenv(LPWSTR str)
{
    LPWSTR  buf = NULL;
    DWORD   bsz = BBUFSIZ;
    DWORD   len;

    if (xfixenvchars(str) < 2)
        return str;
    while (buf == NULL) {
        buf = xwmalloc(bsz);
        len = ExpandEnvironmentStringsW(str, buf, bsz);
        if (len == 0) {
            xfree(buf);
            return NULL;
        }
        if (len > bsz) {
            xfree(buf);
            buf = NULL;
            bsz = len + 1;
        }
    }
    if (xwcschr(buf, L'%')) {
        xfree(buf);
        SetLastError(ERROR_BAD_ENVIRONMENT);
        return NULL;
    }
    return buf;
}

static LPWSTR xexpandenvstr(LPCWSTR str)
{
    LPWSTR  buf;
    LPWSTR  exp;

    buf = xwcsdup(str);
    ASSERT_WSTR(buf, NULL);

    exp = xexpandenv(buf);
    if (exp != buf)
        xfree(buf);
    return exp;
}

static DWORD xsetenvvar(LPCWSTR n, LPWSTR v)
{
    DWORD  r = 0;
    LPWSTR e;

    ASSERT_WSTR(n, ERROR_BAD_ENVIRONMENT);
    if (IS_EMPTY_WCS(v)) {
        if (!SetEnvironmentVariableW(n, NULL))
            return GetLastError();
        else
            return 0;
    }
    e = xexpandenv(v);
    if ((e == NULL) || !SetEnvironmentVariableW(n, e))
        r = GetLastError();
    if (e != v)
        xfree(e);
    return r;
}

static DWORD xsetusrenv(LPCWSTR n, WCHAR e)
{
    int     i;
    WCHAR   d[SVCBATCH_PATH_MAX];
    LPCWSTR v = NULL;
    LPCWSTR p = xenvprefix;

    switch (e) {
        case L'B':
            v = service->base;
        break;
        case L'H':
            v = service->home;
        break;
        case L'L':
            v = service->logs;
        break;
        case L'T':
            v = service->temp;
        break;
        case L'W':
            v = service->work;
        break;
        case L'A':
            v = program->application;
        break;
        case L'D':
            v = program->directory;
        break;
        case L'N':
            v = service->name;
            p = NULL;
        break;
        case L'P':
            v = program->name;
            p = NULL;
        break;
        case L'U':
            v = service->uuid;
            p = NULL;
        break;
        case L'I':
            v = xntowcs(program->pInfo.dwProcessId);
            p = NULL;
        break;
        case L'V':
            v = SVCBATCH_VERSION_VER;
            p = NULL;
        break;
        default:
            return ERROR_INVALID_DATA;
        break;
    }
    if (p != NULL) {
        i = xwcsncat(d, SVCBATCH_PATH_MAX, 0, p);
        /**
         * Strip leading \\?\ for short paths
         * but not \\?\UNC\* paths
         */
        if ((v[0] == L'\\') &&
            (v[1] == L'\\') &&
            (v[2] == L'?' ) &&
            (v[3] == L'\\') &&
            (v[5] == L':')) {
            d[i] = xtolower(v[4]);
            i = xwcsncat(d, SVCBATCH_PATH_MAX, i + 1, v + 6);
            xunxpathsep(d + 2);
            v = d;
        }
        else if (xisalpha(v[0]) &&
                (v[1] == L':')) {
            d[i] = xtolower(v[0]);
            i = xwcsncat(d, SVCBATCH_PATH_MAX, i + 1, v + 2);
            xunxpathsep(d + 2);
            v = d;
        }
    }
    if (!SetEnvironmentVariableW(n, v))
        return GetLastError();
    else
        return 0;
}

static DWORD xsetsvcenv(LPCWSTR p, LPCWSTR n, LPCWSTR v)
{
    int    i;
    WCHAR  b[SVCBATCH_NAME_MAX];
    WCHAR  d[SVCBATCH_PATH_MAX];

    ASSERT_NULL(n, ERROR_BAD_ENVIRONMENT);

    i = xwcsncat(b, SVCBATCH_NAME_MAX, 0, p);
    i = xwcsncat(b, SVCBATCH_NAME_MAX, i, n);
    if (i >= SVCBATCH_NAME_MAX)
        return ERROR_INVALID_PARAMETER;
    if (xenvprefix) {
        i = xwcslcpy(d, SVCBATCH_PATH_MAX, xenvprefix);
        /**
         * Strip leading \\?\ for short paths
         * but not \\?\UNC\* paths
         */
        if ((v[0] == L'\\') &&
            (v[1] == L'\\') &&
            (v[2] == L'?' ) &&
            (v[3] == L'\\') &&
            (v[5] == L':')) {
            d[i++] = xtolower(v[4]);
            i = xwcsncat(d, SVCBATCH_PATH_MAX, i, v + 6);
            xunxpathsep(d + 2);
            v = d;
        }
        else if (xisalpha(v[0]) &&
                (v[1] == L':')) {
            d[i++] = xtolower(v[0]);
            i = xwcsncat(d, SVCBATCH_PATH_MAX, i, v + 2);
            xunxpathsep(d + 2);
            v = d;
        }
    }
    if (!SetEnvironmentVariableW(b, v))
        return GetLastError();
    else
        return 0;
}

static LPWSTR xappendarg(int nq, LPWSTR s1, LPCWSTR s2)
{
    LPCWSTR c;
    LPWSTR  e;
    LPWSTR  d;

    int l1, l2, nn;

    l2 = xwcslen(s2);
    if (l2 == 0)
        return s1;
    if (l2 <  3)
        nq = 0;
    if (nq) {
        nq = 0;
        if (*s2 == L'"') {
            /**
             * Presume the user provided properly
             * quoted argument.
             */
        }
        else if (xwcspbrk(s2, L" \t\"")) {
            for (c = s2; ; c++, nq++) {
                int b = 0;

                while (*c == L'\\') {
                    b++;
                    c++;
                }

                if (*c == WNUL) {
                    nq += b * 2;
                    break;
                }
                else {
                    if (*c == L'"')
                        nq += b * 2 + 1;
                    else
                        nq += b;
                }
            }
            l2 = nq + 2;
        }
    }
    l1 = xwcslen(s1);
    nn = l1 + l2 + 2;
    e  = (LPWSTR)xrealloc(s1, nn * sizeof(WCHAR));
    d  = e;

    if(l1) {
        d += l1;
        *(d++) = L' ';
    }
    if (nq) {
        *(d++) = L'"';
        for (c = s2; ; c++, d++) {
            int b = 0;

            while (*c == '\\') {
                b++;
                c++;
            }

            if (*c == WNUL) {
                if (b) {
                    wmemset(d, L'\\', b * 2);
                    d += b * 2;
                }
                break;
            }
            else {
                if (*c == L'"') {
                    wmemset(d, L'\\', b * 2 + 1);
                    d += b * 2 + 1;
                }
                else {
                    if (b) {
                        wmemset(d, L'\\', b);
                        d += b;
                    }
                }
                *d = *c;
            }
        }
        *(d++) = L'"';
    }
    else {
        wmemcpy(d, s2, l2);
        d += l2;
    }
    *d = WNUL;
    return e;
}

static DWORD xnamemap(LPCWSTR src, SVCBATCH_NAME_MAP const *map, DWORD def)
{
    int i;

    if (IS_EMPTY_WCS(src))
        return def;
    for (i = 0; map[i].name != NULL; i++) {
        if (xwcsequals(src, map[i].name))
            return map[i].code;
    }
    return def;
}

static LPCWSTR xcodemap(SVCBATCH_NAME_MAP const *map, DWORD c)
{
    int i;

    for (i = 0; map[i].name != NULL; i++) {
        if (map[i].code == c)
            return map[i].name;
    }
    return zerostring;
}

static int xlongopt(int nargc, LPCWSTR *nargv,
                    LPCWSTR *options, LPCWSTR allowed)
{
    LPCWSTR *poption;
    int      option;

    xwoptarg = NULL;
    if (xwoptind >= nargc) {
        /* No more arguments */
        return EOF;
    }
    xwoption = nargv[xwoptind];
    if (xwoption[0] != L'-') {
        /* Not an option */
        return EOF;
    }
    if (xwoption[1] != L'-') {
        /* The single '-'  is command delimiter */
        if (xwoption[1] == WNUL)
            xwoptind++;
        return EOF;
    }
    if (xwoption[2] == WNUL) {
        /* The single '--' is command delimiter */
        xwoptind++;
        return EOF;
    }
    poption = options;

    while (*poption) {
        int optmod;
        int optsep = 0;
        LPCWSTR optsrc;
        LPCWSTR optopt = NULL;
        LPCWSTR optstr = xwoption + 2;

        optsrc = *poption;
        optmod = optsrc[1];
        if (optmod == '.') {
            if (xwcsequals(optstr, optsrc + 2))
                optopt = zerostring;
        }
        else {
            int endpos = xwstartswith(optstr, optsrc + 2);
            if (endpos) {
                LPCWSTR oo = optstr + endpos;
                /* Check for --option , --option: or --option= */
                if (*oo == WNUL) {
                    optopt = zerostring;
                }
                else if ((*oo == L':') || (*oo == L'=')) {
                    optsep = *oo;
                    optopt =  oo + 1;
                }
            }
        }
        if (optopt == NULL) {
            poption++;
            continue;
        }
        /* Found long option */
        option = *optsrc;
        if (xwcschr(allowed, option) == NULL) {
            /**
             * The --option is not enabled for the
             * current command.
             *
             */
            if (*allowed == L'.')
                return EOF;
            else
                return EACCES;
        }
        if (optmod == '.') {
            /* No arguments needed */
            xwoptind++;
            return option;
        }
        /* Skip blanks */
        while (xisblank(*optopt))
            optopt++;
        if (*optopt) {
            /* Argument is part of the option */
            xwoptarg = optopt;
            xwoptind++;
            return option;
        }
        if (optsep) {
            /* Empty in place argument */
            return ENOENT;
        }
        if (optmod == '?') {
            /* No optional argument */
            xwoptind++;
            return option;
        }
        if (optmod == ':') {
            /* No required argument */
            return ENOENT;
        }
        if (nargc > xwoptind)
            optopt = nargv[++xwoptind];
        while (xisblank(*optopt))
            optopt++;
        if (*optopt == WNUL)
            return ENOENT;
        xwoptind++;
        xwoptarg = optopt;
        return option;
    }
    /* Option not found */
    return EINVAL;
}

static int xwgetopt(int nargc, LPCWSTR *nargv, LPCWSTR opts)
{
    LPCWSTR  optpos;
    LPCWSTR  optarg;
    int      option;
    int      optsep = 0;
    int      i = 0;

    xwoptarg = NULL;
    if (xwoptind >= nargc)
        return EOF;
    xwoption = nargv[xwoptind];
    if (xwoptarr) {
        if (xwoptend) {
            if (xiswcschar(xwoption, L']')) {
                xwoptind++;
                return ']';
            }
            else {
                xwoptarg = xwoption;
                xwoptind++;
                return xwoptarr;
            }
        }
        else {
            if (xiswcschar(xwoption, L'[')) {
                xwoptind++;
                return '[';
            }
        }
    }
    xwoptarr = 0;
    xwoptend = 0;
    if (xwoption[i++] != L'-')
        return EOF;
    if (xwoption[i] == WNUL) {
        /* The single '-' is command delimiter */
        xwoptind++;
        return EOF;
    }
    option = xtolower(xwoption[i++]);
    optpos = xwcschr(opts, option);
    if (optpos == NULL)
        return EINVAL;

    optsep = xwoption[i];
    if ((optsep == L':') || (optsep == L'=')) {
        optarg = xwoption + i + 1;
        while (xisblank(*optarg))
            ++optarg;
        if (*optarg == WNUL) {
            /* Missing argument */
            return ENOENT;
        }
        else {
            xwoptarg = optarg;
            xwoptind++;
            return option;
        }
    }
    if (optsep) {
        /* Extra data */
        return EINVAL;
    }
    optarg = zerostring;
    if (nargc > xwoptind)
        optarg = nargv[++xwoptind];
    while (xisblank(*optarg))
        optarg++;
    if (*optarg == WNUL)
        return ENOENT;
    xwoptind++;
    xwoptarg = optarg;
    return option;
}

static LPWSTR xuuidstring(LPWSTR b)
{
    static WORD   w = 0;
    static WCHAR  s[SVCBATCH_UUID_MAX];
    unsigned char d[20];
    const WCHAR   xb16[] = L"0123456789abcdef";
    int  i, x;

    if (BCryptGenRandom(NULL, d + 2, 16,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return NULL;
    if (w == 0)
        w = LOWORD(program->pInfo.dwProcessId);
    if (b == NULL)
        b = s;
    d[0] = HIBYTE(w);
    d[1] = LOBYTE(w);
    for (i = 0, x = 0; i < 18; i++) {
        if (i == 2 || i == 6 || i == 8 || i == 10 || i == 12)
            b[x++] = '-';
        b[x++] = xb16[d[i] >> 4];
        b[x++] = xb16[d[i] & 0x0F];
    }
    b[x] = WNUL;
    return b;
}

static int getdayofyear(int y, int m, int d)
{
    static const int dayoffset[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int r;

    r = dayoffset[m - 1] + d;
    if (IS_LEAP_YEAR(y) && (r > 59))
        r++;
    return r;
}


#if defined(_DEBUG)

/**
 * Runtime debugging functions
 */

static const char *threadnames[] = {
    "workerthread",
    "wrpipethread",
    "stopthread  ",
    "rotatethread",
    NULL
};

static void dbgflock(HANDLE f)
{
    DWORD len = DWORD_MAX;
    OVERLAPPED  off;

    memset(&off, 0, sizeof(off));
    LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK, 0, len, len, &off);
}

static void dbgfunlock(HANDLE f)
{
    DWORD len = DWORD_MAX;
    OVERLAPPED  off;

    memset(&off, 0, sizeof(off));
    UnlockFileEx(f, 0, len, len, &off);
}

static void dbgprintf(LPCSTR funcname, int line, LPCSTR format, ...)
{
    static char sep = ':';
    int     n = SVCBATCH_LINE_MAX - 4;
    int     i = 0;
#if (_DEBUG > 1)
    int     o[8];
#endif
    char    b[SVCBATCH_LINE_MAX];
    SYSTEMTIME tm;
    HANDLE  h;

    GetLocalTime(&tm);
    b[i++] = '[';
    i += xsnprintf(b + i, n, "%lu", GetCurrentProcessId());
#if (_DEBUG > 1)
    o[0] = i;
#endif
    b[i++] = sep;
    i += xsnprintf(b + i, n, "%lu", GetCurrentThreadId());
#if (_DEBUG > 1)
    o[1] = i;
#endif
    b[i++] = sep;
    b[i++] = tm.wMonth  / 10 + '0';
    b[i++] = tm.wMonth  % 10 + '0';
    b[i++] = tm.wDay    / 10 + '0';
    b[i++] = tm.wDay    % 10 + '0';
    b[i++] = tm.wYear   % 100 / 10 + '0';
    b[i++] = tm.wYear   % 10 + '0';
    b[i++] = '/';
    b[i++] = tm.wHour   / 10 + '0';
    b[i++] = tm.wHour   % 10 + '0';
    b[i++] = tm.wMinute / 10 + '0';
    b[i++] = tm.wMinute % 10 + '0';
    b[i++] = tm.wSecond / 10 + '0';
    b[i++] = tm.wSecond % 10 + '0';
    b[i++] = '.';
    b[i++] = tm.wMilliseconds / 100 + '0';
    b[i++] = tm.wMilliseconds % 100 / 10 + '0';
    b[i++] = tm.wMilliseconds % 10 + '0';
#if (_DEBUG > 1)
    o[2] = i;
#endif
    b[i++] = sep;
    i += xsnprintf(b + i, n - i, "%s",
                   dbgsvcmodes[dbgsvcmode]);
#if (_DEBUG > 1)
    o[3] = i;
#endif
    b[i++] = sep;
    i += xsnprintf(b + i, n - i, "%s(%d)",
                   funcname, line);
#if (_DEBUG > 1)
    o[4] = i;
#endif
    b[i++] = ']';
    if (format) {
        va_list ap;

        va_start(ap, format);
        b[i++] = ' ';
        i += xvsnprintf(b + i, n - i, format, ap);
        va_end(ap);
    }
#if (_DEBUG > 1)
    o[5] = i;
#endif
    EnterCriticalSection(&dbglock);
    h = InterlockedExchangePointer(&dbgfile, NULL);
    if (IS_VALID_HANDLE(h)) {
        DWORD wr;
        LARGE_INTEGER dd = {{ 0, 0 }};

        dbgflock(h);
        SetFilePointerEx(h, dd, NULL, FILE_END);
        b[i++] = '\r';
        b[i++] = '\n';
        WriteFile(h, b, i, &wr, NULL);
        FlushFileBuffers(h);
        dbgfunlock(h);
        InterlockedExchangePointer(&dbgfile, h);
    }
    LeaveCriticalSection(&dbglock);
#if (_DEBUG > 1)
    {
        char s[SVCBATCH_LINE_MAX];

        for (i = 0; i < 6; i++)
            b[o[i]] = CNUL;
        xsnprintf(s, SVCBATCH_LINE_MAX, "%-4s %4s %s %-22s %s", b + 1,
                  b + o[0] + 1, b + o[2] + 1,
                  b + o[3] + 1, b + o[4] + 1);
        OutputDebugStringA(s);
    }
#endif
}

static void dbgprints(LPCSTR funcname, int line, LPCSTR string)
{
    if (string == NULL)
        dbgprintf(funcname, line, NULL, NULL);
    else
        dbgprintf(funcname, line, "%s", string);
}


static void xiphandler(LPCWSTR e,
                       LPCWSTR w, LPCWSTR f,
                       unsigned int n, uintptr_t r)
{
    dbgprints(__FUNCTION__, __LINE__,
              "invalid parameter handler called");
}

#endif

static int xwinapierror(LPWSTR buf, int siz, DWORD err)
{
    int n;

    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       err,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       buf,
                       siz - 1,
                       NULL);
    if (n) {
        int i;
        do {
            buf[n--] = WNUL;
        } while ((n > 0) && ((buf[n] == 46) || (buf[n] < 33)));
        i = n++;
        while (i-- > 0) {
            if (buf[i] < 32)
                buf[i] = 32;
        }
    }
    else {
        n = xsnwprintf(buf, siz,
                       L"Unrecognized system error code: %u", err);
    }
    return n;
}

static BOOL setupeventlog(void)
{
    static BOOL ssrv = FALSE;
    static volatile LONG eset = 0;
    static const WCHAR emsg[] = L"%SystemRoot%\\System32\\netmsg.dll\0";
    DWORD c;
    HKEY  k;

    if (InterlockedIncrement(&eset) > 1)
        return ssrv;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        SYSTEM_SVC_SUBKEY \
                        L"\\EventLog\\Application\\" CPP_WIDEN(SVCBATCH_NAME),
                        0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_READ | KEY_WRITE,
                        NULL, &k, &c) != ERROR_SUCCESS)
        return FALSE;
    if (c == REG_CREATED_NEW_KEY) {
        DWORD dw = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
                   EVENTLOG_INFORMATION_TYPE;
        if (RegSetValueExW(k, L"EventMessageFile", 0, REG_EXPAND_SZ,
                          (const BYTE *)emsg, DSIZEOF(emsg)) != ERROR_SUCCESS)
            goto finished;
        if (RegSetValueExW(k, L"TypesSupported", 0, REG_DWORD,
                          (const BYTE *)&dw, 4) != ERROR_SUCCESS)
            goto finished;
    }
    ssrv = TRUE;
finished:
    RegCloseKey(k);
    return ssrv;
}

static DWORD svcsyserror(LPCSTR fn, int line, WORD typ, DWORD ern, LPCWSTR err, LPCWSTR eds, LPCWSTR erp)
{
    WCHAR   buf[SVCBATCH_LINE_MAX];
    LPWSTR  dsc;
    LPWSTR  erb;
    LPCWSTR svc;
    LPCWSTR msg[10];
    int     c = 0;
    int     i = 0;
    int     n;
    int   siz = SVCBATCH_LINE_MAX;

    if (service && service->name)
        svc = service->name;
    else
        svc = CPP_WIDEN(SVCBATCH_NAME);
    c = xsnwprintf(buf, BBUFSIZ, L"The %s service", svc);
    msg[i++] = buf;
    if (typ == EVENTLOG_ERROR_TYPE)
        msg[i++] = L"reported the following error:";
    buf[c++] = WNUL;
    buf[c++] = WNUL;
    msg[i++] = buf + c;
    buf[c++] = L'\r';
    buf[c++] = L'\n';
    siz = SVCBATCH_LINE_MAX - c;
    dsc = buf + c;
    if (err) {
        if (xwcschr(err, L'%')) {
            n = xsnwprintf(dsc, siz, err, eds);
        }
        else {
            n = xwcsncat(dsc, siz, 0, err);
            if (eds) {
                n = xwcsncat(dsc, siz, n, L": ");
                n = xwcsncat(dsc, siz, n, eds);
            }
        }
        c += n;
    }
    else {
        n = xwcsncat(dsc, siz, 0, eds);
        if (erp) {
            n = xwcsncat(dsc, siz, n, L": ");
            n = xwcsncat(dsc, siz, n, erp);
        }
        c += n;
    }
    buf[c++] = WNUL;
    buf[c++] = WNUL;
    siz = SVCBATCH_LINE_MAX - c;

    if (ern == 0) {
        ern = ERROR_INVALID_PARAMETER;
#if defined(_DEBUG)
        dbgprintf(fn, line, "%S", dsc);
#endif
    }
    else {
        msg[i++] = buf + c;
        buf[c++] = L'\r';
        buf[c++] = L'\n';
        erb = buf + c;
        siz = SVCBATCH_LINE_MAX - c;

        n   = xsnwprintf(erb,  siz, L"error(%lu) ", ern);
        c  += xwinapierror(erb + n, siz - n, ern);
#if defined(_DEBUG)
        dbgprintf(fn, line, "%S, %S", dsc, erb);
#endif
        c += n;
        buf[c++] = WNUL;
        buf[c++] = WNUL;
        siz = SVCBATCH_LINE_MAX - c;
    }
    if (typ == EVENTLOG_ERROR_TYPE) {
        erb = buf + c;
        xsnwprintf(erb, siz,
                   L"\r\n" CPP_WIDEN(SVCBATCH_NAME) L" " SVCBATCH_VERSION_WCS \
                   L" %S %d", fn, line);
        msg[i++] = erb;
    }
    msg[i++] = CRLFW;
    while (i < 10)
        msg[i++] = NULL;

    if (setupeventlog()) {
        HANDLE es = RegisterEventSourceW(NULL, CPP_WIDEN(SVCBATCH_NAME));
        if (IS_VALID_HANDLE(es)) {
            /**
             * Generic message: '%1 %2 %3 %4 %5 %6 %7 %8 %9'
             * The event code in netmsg.dll is 3299
             */
            ReportEventW(es, typ, 0, 3299, NULL, 9, 0, msg, NULL);
            DeregisterEventSource(es);
        }
    }
    return ern;
}

static void closeprocess(LPSVCBATCH_PROCESS p)
{
    InterlockedExchange(&p->state, SVCBATCH_PROCESS_STOPPED);

    DBG_PRINTF("%.4lu %lu %S", p->pInfo.dwProcessId, p->exitCode, p->application);
    SAFE_CLOSE_HANDLE(p->pInfo.hProcess);
    SAFE_CLOSE_HANDLE(p->pInfo.hThread);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdError);
    SAFE_MEM_FREE(p->commandLine);
}

static DWORD waitprocess(LPSVCBATCH_PROCESS p, DWORD w)
{
    ASSERT_NULL(p, ERROR_INVALID_PARAMETER);

    if (p->state > SVCBATCH_PROCESS_STOPPED) {
        if (w > 0)
            WaitForSingleObject(p->pInfo.hProcess, w);
        GetExitCodeProcess(p->pInfo.hProcess, &p->exitCode);
    }
    return p->exitCode;
}

static DWORD getproctree(LPHANDLE pa, DWORD pid)
{
    DWORD  r = 0;
    HANDLE h;
    HANDLE p;
    PROCESSENTRY32W e;

    h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (IS_INVALID_HANDLE(h))
        return 0;
    e.dwSize = DSIZEOF(PROCESSENTRY32W);
    if (!Process32FirstW(h, &e)) {
        CloseHandle(h);
        return 0;
    }
    do {
        if (e.th32ParentProcessID == pid) {
            p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, FALSE, e.th32ProcessID);

            DBG_PRINTF("[%.4lu] %S", e.th32ProcessID, e.szExeFile);
            if (IS_INVALID_HANDLE(p)) {
                xsyswarn(GetLastError(), L"OpenProcess", e.szExeFile);
            }
            else {
                DWORD x;
                if (GetExitCodeProcess(p, &x) && (x == STILL_ACTIVE))
                    pa[r++] = p;
                else
                    CloseHandle(p);
            }
        }
        if (r == TBUFSIZ) {
            DBG_PRINTS("overflow");
            break;
        }
    } while (Process32NextW(h, &e));
    CloseHandle(h);
    return r;
}

static DWORD killproctree(DWORD pid, int rc, DWORD rv)
{
    DWORD  c;
    DWORD  n;
    DWORD  r = 0;
    HANDLE pa[TBUFSIZ];

    DBG_PRINTF("[%d] proc %.4lu", rc, pid);
    c = getproctree(pa, pid);
    for (n = 0; n < c; n++) {
        if (rc > 0)
            r += killproctree(GetProcessId(pa[n]), rc - 1, rv);
        DBG_PRINTF("[%d] kill %.4lu", rc, GetProcessId(pa[n]));
        TerminateProcess(pa[n], rv);
        CloseHandle(pa[n]);
    }
    DBG_PRINTF("[%d] done %.4lu %lu", rc, pid, c + r);
    return c + r;
}

static void killprocess(LPSVCBATCH_PROCESS proc, DWORD rv)
{

    DBG_PRINTF("proc %.4lu", proc->pInfo.dwProcessId);

    if (proc->state == SVCBATCH_PROCESS_STOPPED)
        goto finished;
    InterlockedExchange(&proc->state, SVCBATCH_PROCESS_STOPPING);

    if (killdepth && killproctree(proc->pInfo.dwProcessId, killdepth, rv)) {
        if (waitprocess(proc, SVCBATCH_STOP_STEP) != STILL_ACTIVE)
            goto finished;
    }
    DBG_PRINTF("kill %.4lu", proc->pInfo.dwProcessId);
    proc->exitCode = rv;
    TerminateProcess(proc->pInfo.hProcess, proc->exitCode);

finished:
    InterlockedExchange(&proc->state, SVCBATCH_PROCESS_STOPPED);
    DBG_PRINTF("done %.4lu", proc->pInfo.dwProcessId);
}

static void cleanprocess(LPSVCBATCH_PROCESS proc)
{

    DBG_PRINTF("proc %.4lu", proc->pInfo.dwProcessId);

    if (killdepth)
        killproctree(proc->pInfo.dwProcessId, killdepth, ERROR_ARENA_TRASHED);
    DBG_PRINTF("done %.4lu", proc->pInfo.dwProcessId);
}

static DWORD xmdparent(LPWSTR path)
{
    DWORD  rc = 0;
    LPWSTR s;

    s = xwcsrchr(path, L'\\');
    if (s == NULL)
        return ERROR_BAD_PATHNAME;
    *s = WNUL;
    if (!CreateDirectoryW(path, NULL))
        rc = GetLastError();
    if (rc == ERROR_PATH_NOT_FOUND) {
        /**
         * One or more intermediate directories do not exist
         */
        rc = xmdparent(path);
        if (rc == 0) {
            if (!CreateDirectoryW(path, NULL))
                rc = GetLastError();
        }
    }
    *s = L'\\';
    return rc;
}

static DWORD xcreatedir(LPCWSTR path)
{
    DWORD rc = 0;

    if (CreateDirectoryW(path, NULL))
        return 0;
    else
        rc = GetLastError();
    if (rc == ERROR_PATH_NOT_FOUND) {
        WCHAR pp[SVCBATCH_PATH_MAX];
        /**
         * One or more intermediate directories do not exist
         */
        xwcslcpy(pp, SVCBATCH_PATH_MAX, path);
        xwinpathsep(pp);
        rc = xmdparent(pp);
        if (rc == 0) {
            /**
             * All intermediate paths are created.
             * Create the final directory in the path.
             */
            if (!CreateDirectoryW(path, NULL))
                rc = GetLastError();
        }
    }
    if (rc == ERROR_ALREADY_EXISTS)
        rc = 0;
    return rc;
}

static DWORD WINAPI xrunthread(LPVOID param)
{
    LPSVCBATCH_THREAD p = (LPSVCBATCH_THREAD)param;

#if defined(_DEBUG)
    p->duration = GetTickCount64();
#endif
    p->exitCode = (*p->startAddress)(p->parameter);
#if defined(_DEBUG)
    p->duration = GetTickCount64() - p->duration;
    DBG_PRINTF("%s ended %lu", p->name, p->exitCode);
#endif
    InterlockedExchange(&p->started, 0);
    ExitThread(p->exitCode);

    return p->exitCode;
}

static BOOL xcreatethread(SVCBATCH_THREAD_ID id,
                          int suspended,
                          LPTHREAD_START_ROUTINE threadfn,
                          LPVOID param)
{
    if (InterlockedCompareExchange(&threads[id].started, 1, 0)) {
        /**
         * Already started
         */
         SetLastError(ERROR_BUSY);
         return FALSE;
    }
#if defined(_DEBUG)
    threads[id].name         = threadnames[id];
#endif
    threads[id].id           = id;
    threads[id].startAddress = threadfn;
    threads[id].parameter    = param;
    threads[id].thread       = CreateThread(NULL, 0, xrunthread, &threads[id],
                                            suspended ? CREATE_SUSPENDED : 0, NULL);
    if (threads[id].thread == NULL) {
        threads[id].exitCode = GetLastError();
        InterlockedExchange(&threads[id].started, 0);
        return FALSE;
    }
    return TRUE;
}

static LPCWSTR skipdotslash(LPCWSTR s)
{
    LPCWSTR p = s;

    if (*p++ == L'.') {
        if (*p == WNUL)
            return NULL;
        if ((*p == L'\\') || (*p == L'/')) {
            p++;
            return (*p == WNUL) ? NULL : p;
        }
    }
    return s;
}

static BOOL isdotslash(LPCWSTR p)
{
    if ((p != NULL) && (p[0] == L'.') && ((p[1] == L'\\') || (p[1] == L'/')))
        return TRUE;
    else
        return FALSE;
}

static BOOL isabsolutepath(LPCWSTR p)
{
    if ((p != NULL) && (*p != WNUL)) {
        if ((p[0] == L'\\') || (xisalpha(p[0]) && (p[1] == L':')))
            return TRUE;
    }
    return FALSE;
}

static BOOL isrelativepath(LPCWSTR p)
{
    if ((p != NULL) && (*p != WNUL)) {
        if ((p[0] == L'\\') || (xisalpha(p[0]) && (p[1] == L':')))
            return FALSE;
        else
            return TRUE;
    }
    return FALSE;
}

static DWORD xfixmaxpath(LPWSTR buf, DWORD len)
{
    if (len > 5) {
        if (len < MAX_PATH) {
            /**
             * Strip leading \\?\ for short paths
             * but not \\?\UNC\* paths
             */
            if ((buf[0] == L'\\') &&
                (buf[1] == L'\\') &&
                (buf[2] == L'?')  &&
                (buf[3] == L'\\') &&
                (buf[5] == L':')) {
                wmemmove(buf, buf + 4, len - 3);
                len -= 4;
            }
        }
        else {
            /**
             * Prepend \\?\ to long paths
             * but not if already starts with \
             */
            if (buf[0] != L'\\') {
                wmemmove(buf + 4, buf, len + 1);
                wmemcpy(buf, L"\\\\?\\", 4);
                len += 4;
            }
        }
    }
    return len;
}

static LPWSTR xgetfullpath(LPCWSTR path, LPWSTR dst, DWORD siz)
{
    DWORD len;

    ASSERT_WSTR(path, NULL);
    len = GetFullPathNameW(path, siz, dst, NULL);
    if ((len == 0) || (len >= siz))
        return NULL;
    xwinpathsep(dst);
    xfixmaxpath(dst, len);
    return dst;
}

static LPWSTR xgetfinalpath(int isdir, LPCWSTR path)
{
    HANDLE fh;
    WCHAR  buf[SVCBATCH_PATH_MAX];
    DWORD  len;
    DWORD  atr = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    DWORD  acc = GENERIC_READ;
    DWORD  siz = SVCBATCH_PATH_MAX - 4;

    len = GetFullPathNameW(path, siz, buf, NULL);
    if ((len == 0) || (len >= siz))
        return NULL;
    xfixmaxpath(buf, len);
    if (isdir > 1)
        acc |= GENERIC_WRITE;
    fh = CreateFileW(buf, acc, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, atr, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, buf, siz, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= siz))
        return NULL;

    xfixmaxpath(buf, len);
    return xwcsdup(buf);
}

static LPWSTR xgetdirpath(LPCWSTR path, LPWSTR dst, DWORD siz)
{
    HANDLE fh;
    DWORD  len;

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, dst, siz, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= siz))
        return NULL;

    xfixmaxpath(dst, len);
    return dst;
}

static LPWSTR xsearchexe(LPCWSTR name)
{
    WCHAR  buf[SVCBATCH_PATH_MAX];
    DWORD  len;
    DWORD  siz = SVCBATCH_PATH_MAX - 4;
    HANDLE fh;

    DBG_PRINTF("search %S", name);
    len = SearchPathW(NULL, name, L".exe", siz, buf, NULL);
    if ((len == 0) || (len >= siz))
        return NULL;

    fh = CreateFileW(buf, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, buf, siz, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= siz))
        return NULL;

    xfixmaxpath(buf, len);
    DBG_PRINTF("found %S", buf);
    return xwcsdup(buf);
}

static void setsvcstatusexit(DWORD e)
{
    SVCBATCH_CS_ENTER(service);
    service->status.dwServiceSpecificExitCode = e;
    SVCBATCH_CS_LEAVE(service);
}

static void reportsvcstatus(LPCSTR fn, int line, DWORD status, DWORD param)
{
    static volatile LONG cpcnt = 0;

    if (!servicemode)
        return;
    SVCBATCH_CS_ENTER(service);
    if (InterlockedExchange(&service->state, SERVICE_STOPPED) == SERVICE_STOPPED)
        goto finished;
    service->status.dwControlsAccepted = 0;
    service->status.dwCheckPoint       = 0;
    service->status.dwWaitHint         = 0;

    if (status == SERVICE_RUNNING) {
        service->status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                             SERVICE_ACCEPT_SHUTDOWN |
                                             preshutdown;
        service->status.dwWin32ExitCode    = NO_ERROR;
        InterlockedExchange(&cpcnt, 0);
    }
    else if (status == SERVICE_STOPPED) {
        if (service->status.dwCurrentState != SERVICE_STOP_PENDING) {
            if (svcfailmode == SVCBATCH_FAIL_EXIT) {
                svcsyserror(fn, line, EVENTLOG_ERROR_TYPE, 0, NULL,
                            SVCBATCH_MSG(1), NULL);
                SVCBATCH_CS_LEAVE(service);
                exit(ERROR_INVALID_LEVEL);
            }
            else {
                if ((svcfailmode == SVCBATCH_FAIL_NONE) &&
                    (service->status.dwCurrentState == SERVICE_RUNNING)) {
                    svcsyserror(fn, line, EVENTLOG_INFORMATION_TYPE, 0, NULL,
                                SVCBATCH_MSG(1), NULL);
                    param = 0;
                    service->status.dwWin32ExitCode = NO_ERROR;
                }
                else {
                    /* SVCBATCH_FAIL_ERROR */
                    svcsyserror(fn, line, EVENTLOG_ERROR_TYPE, 0, NULL,
                                SVCBATCH_MSG(1), NULL);
                    if (param == 0) {
                        if (service->status.dwCurrentState == SERVICE_RUNNING)
                            param = ERROR_PROCESS_ABORTED;
                        else
                            param = ERROR_SERVICE_START_HANG;
                    }
                }
            }
        }
        if (param != 0)
            service->status.dwServiceSpecificExitCode  = param;
        if (service->status.dwServiceSpecificExitCode != 0)
            service->status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }
    else {
        service->status.dwCheckPoint = InterlockedIncrement(&cpcnt);
        service->status.dwWaitHint   = param;
    }
    service->status.dwCurrentState = status;
    InterlockedExchange(&service->state, status);
    if (!SetServiceStatus(service->handle, &service->status)) {
        xsyserror(GetLastError(), L"SetServiceStatus", NULL);
        InterlockedExchange(&service->state, SERVICE_STOPPED);
    }
finished:
    SVCBATCH_CS_LEAVE(service);
}

static BOOL createiopipe(LPHANDLE rd, LPHANDLE wr, DWORD mode)
{
    DWORD i;
    WCHAR name[SVCBATCH_UUID_MAX];
    SECURITY_ATTRIBUTES sa;

    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    i = xwcslcpy(name, SVCBATCH_UUID_MAX, SVCBATCH_PIPEPFX);
    xuuidstring(name + i);

    *rd = CreateNamedPipeW(name,
                           PIPE_ACCESS_INBOUND | mode,
                           PIPE_TYPE_BYTE,
                           1,
                           0,
                           SVCBATCH_PIPE_LEN,
                           0,
                           &sa);
    if (IS_INVALID_HANDLE(*rd))
        return FALSE;

    *wr = CreateFileW(name,
                      GENERIC_WRITE,
                      0,
                      &sa,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      NULL);
    if (IS_INVALID_HANDLE(*wr))
        return FALSE;
    else
        return TRUE;
}

static BOOL createnulpipe(LPHANDLE ph)
{
    SECURITY_ATTRIBUTES sa;

    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    *ph = CreateFileW(L"NUL",
                      GENERIC_READ | GENERIC_WRITE,
                      0,
                      &sa,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      NULL);
    if (IS_INVALID_HANDLE(*ph))
        return FALSE;
    else
        return TRUE;
}

static DWORD createiopipes(LPSTARTUPINFOW si,
                           LPHANDLE iwrs,
                           LPHANDLE ords,
                           DWORD mode)
{
    HANDLE cp = program->pInfo.hProcess;
    HANDLE rd = NULL;
    HANDLE wr = NULL;

    si->cb      = DSIZEOF(STARTUPINFOW);
    si->dwFlags = STARTF_USESTDHANDLES;

    if (iwrs) {
        /**
         * Create stdin pipe, with write side
         * of the pipe as non inheritable.
         */
        if (!createiopipe(&rd, &wr, 0))
            return GetLastError();
        if (!DuplicateHandle(cp, wr, cp,
                             iwrs, FALSE, 0,
                             DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
            return GetLastError();
    }
    else {
        if (!createnulpipe(&rd))
            return GetLastError();
    }
    si->hStdInput = rd;
    if (ords) {
        /**
         * Create stdout/stderr pipe, with read side
         * of the pipe as non inheritable
         */
        if (!createiopipe(&rd, &wr, mode))
            return GetLastError();
        if (!DuplicateHandle(cp, rd, cp,
                             ords, FALSE, 0,
                             DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
            return GetLastError();
    }
    else {
        if (!createnulpipe(&wr))
            return GetLastError();
    }
    si->hStdOutput = wr;
    si->hStdError  = wr;

    return 0;
}

static BOOL canrotatelogs(LPSVCBATCH_LOG log)
{
    BOOL rv = FALSE;

    SVCBATCH_CS_ENTER(log);
    if (log->state == 0) {
        if (log->size) {
            InterlockedExchange(&log->state, 1);
            rv = TRUE;
        }
    }
    SVCBATCH_CS_LEAVE(log);
    return rv;
}

static DWORD createlogsdir(LPCWSTR outdir)
{
    WCHAR  b[SVCBATCH_PATH_MAX];
    LPWSTR p;
    int    n = SVCBATCH_PATH_MAX - 4;

    if (isrelativepath(outdir)) {
        int i;

        i = xwcsncat(b, n, 0, service->work);
        b[i++] = L'\\';
        i = xwcsncat(b, n, i, outdir);
        if (i >= n) {
            xsyserror(0, L"GetPath", outdir);
            return ERROR_BAD_PATHNAME;
        }
        xfixmaxpath(b, i);
    }
    else {
        p = xgetfullpath(outdir, b, n);
        if (p == NULL) {
            xsyserror(0, L"GetFullPath", outdir);
            return ERROR_BAD_PATHNAME;
        }
    }
    p = xgetdirpath(b, b, n);
    if (p == NULL) {
        DWORD rc = GetLastError();

        if (rc > ERROR_PATH_NOT_FOUND)
            return xsyserror(rc, L"GetDirPath", b);

        rc = xcreatedir(b);
        if (rc != 0)
            return xsyserror(rc, L"CreateDirectory", b);
        p = xgetdirpath(b, b, n);
        if (p == NULL)
            return xsyserror(GetLastError(), L"GetDirPath", b);
    }
    service->logs = xwcsdup(b);
    return 0;
}

static DWORD rotateprevlogs(LPSVCBATCH_LOG log, BOOL ssp)
{
    DWORD rc;
    int   i;
    int   n;
    int   x;
    WCHAR lognn[SVCBATCH_PATH_MAX];
    WCHAR logpn[SVCBATCH_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA ad;

    if (GetFileAttributesExW(log->logFile, GetFileExInfoStandard, &ad)) {
        if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
            DBG_PRINTF("empty log %S", log->logFile);
            return 0;
        }
    }
    else {
        rc = GetLastError();
        if (rc != ERROR_FILE_NOT_FOUND)
            return xsyserror(rc, log->logFile, NULL);
        else
            return 0;
    }
    x = xwcslcpy(lognn, SVCBATCH_PATH_MAX - 4, log->logFile);
    x = xwcsncat(lognn, SVCBATCH_PATH_MAX - 4, x, L".0");
    if (x >= (SVCBATCH_PATH_MAX - 4))
        return xsyserror(ERROR_BAD_PATHNAME, lognn, NULL);
    xfixmaxpath(lognn, x);
    if (!MoveFileExW(log->logFile, lognn, MOVEFILE_REPLACE_EXISTING))
        return xsyserror(GetLastError(), log->logFile, lognn);
    if (ssp)
        xsvcstatus(SERVICE_START_PENDING, 0);

    n = srvcmaxlogs;
    wmemcpy(logpn, lognn, x + 1);
    x--;
    for (i = 1; i < srvcmaxlogs; i++) {
        lognn[x] = L'0' + i;

        if (!GetFileAttributesExW(lognn, GetFileExInfoStandard, &ad))
            break;
        if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0))
            break;
    }
    n = i;
    /**
     * Rotate previous log files
     */
    for (i = n; i > 0; i--) {
        logpn[x] = L'0' + i - 1;
        lognn[x] = L'0' + i;
        if (GetFileAttributesExW(logpn, GetFileExInfoStandard, &ad)) {
            if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
                DBG_PRINTF("skipping empty %S", logpn);
            }
            else {
                if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING))
                    return xsyserror(GetLastError(), logpn, lognn);
                if (ssp)
                    xsvcstatus(SERVICE_START_PENDING, 0);
            }
        }
        else {
            rc = GetLastError();
            if (rc != ERROR_FILE_NOT_FOUND)
                return xsyserror(rc, logpn, NULL);
        }
    }

    return 0;
}

static int xwcsftime(LPWSTR dst, int siz, LPCWSTR fmt, int *vc)
{
    LPCWSTR s = fmt;
    LPWSTR  d = dst;
    int     n = siz;
    int     v = 0;
    SYSTEMTIME tm;

    ASSERT_CSTR(s, 0);
    ASSERT_NULL(d, 0);
    ASSERT_SIZE(n, 2, 0);

    if (IS_SET(SVCBATCH_OPT_LOCALTIME))
        GetLocalTime(&tm);
    else
        GetSystemTime(&tm);

    while (*s) {
        *d = WNUL;

        ASSERT_SIZE(n, 2, siz);
        if (*s == L'@') {
            int i = 0;
            int w;
            s++;
            switch (*s) {
                case L'@':
                    d[i++] = L'@';
                break;
                case L'y':
                    d[i++] = tm.wYear % 100 / 10 + L'0';
                    d[i++] = tm.wYear % 10 + L'0';
                    v++;
                break;
                case L'Y':
                    ASSERT_SIZE(n, 4, siz);
                    d[i++] = tm.wYear / 1000 + L'0';
                    d[i++] = tm.wYear % 1000 / 100 + L'0';
                    d[i++] = tm.wYear % 100 / 10 + L'0';
                    d[i++] = tm.wYear % 10 + L'0';
                    v++;
                break;
                case L'd':
                    d[i++] = tm.wDay  / 10 + L'0';
                    d[i++] = tm.wDay % 10 + L'0';
                    v++;
                break;
                case L'm':
                    d[i++] = tm.wMonth / 10 + L'0';
                    d[i++] = tm.wMonth % 10 + L'0';
                    v++;
                break;
                case L'H':
                    d[i++] = tm.wHour / 10 + L'0';
                    d[i++] = tm.wHour % 10 + L'0';
                    v++;
                break;
                case L'M':
                    d[i++] = tm.wMinute / 10 + L'0';
                    d[i++] = tm.wMinute % 10 + L'0';
                    v++;
                break;
                case L'S':
                    d[i++] = tm.wSecond / 10 + L'0';
                    d[i++] = tm.wSecond % 10 + L'0';
                    v++;
                break;
                case L'j':
                    w = getdayofyear(tm.wYear, tm.wMonth, tm.wDay);
                    d[i++] = w / 100 + L'0';
                    d[i++] = w % 100 / 10 + L'0';
                    d[i++] = w % 10 + L'0';
                    v++;
                break;
                case L'F':
                    ASSERT_SIZE(n, 10, siz);
                    d[i++] = tm.wYear / 1000 + L'0';
                    d[i++] = tm.wYear % 1000 / 100 + L'0';
                    d[i++] = tm.wYear % 100 / 10 + L'0';
                    d[i++] = tm.wYear % 10 + L'0';
                    d[i++] = L'-';
                    d[i++] = tm.wMonth / 10 + L'0';
                    d[i++] = tm.wMonth % 10 + L'0';
                    d[i++] = L'-';
                    d[i++] = tm.wDay  / 10 + L'0';
                    d[i++] = tm.wDay % 10 + L'0';
                    v++;
                break;
                case L'w':
                    d[i++] = L'0' + tm.wDayOfWeek;
                    v++;
                break;
                /** Custom formatting codes */
                case L's':
                    ASSERT_SIZE(n,  3, siz);
                    d[i++] = tm.wMilliseconds / 100 + L'0';
                    d[i++] = tm.wMilliseconds % 100 / 10 + L'0';
                    d[i++] = tm.wMilliseconds % 10 + L'0';
                    v++;
                break;
                case L'N':
                    i = xwcslcpy(d, n, service->name);
                break;
                case L'P':
                    i = xwcslcpy(d, n, program->name);
                break;
                default:
                    SetLastError(ERROR_INVALID_PARAMETER);
                   *dst = WNUL;
                    return 0;
                break;
            }
            d += i;
            n -= i;
        }
        else {
            *d++ = *s;
            n--;
        }
        s++;
    }
    *d = WNUL;
    if (vc)
        *vc = v;
    return (int)(d - dst);
}

static DWORD openlogfile(LPSVCBATCH_LOG log, BOOL ssp)
{
    HANDLE  fh = NULL;
    DWORD   rc;
    DWORD   cd = CREATE_ALWAYS;
    WCHAR   nb[SVCBATCH_NAME_MAX];
    LPCWSTR np = log->logName;
    int     rp = srvcmaxlogs;
    int     i  = 0;

    if (xwcschr(np, L'@')) {
        if (xwcsftime(nb, SVCBATCH_NAME_MAX, np, &i) == 0)
            return xsyserror(GetLastError(), np, NULL);
        if (i)
            rp = 0;
        DBG_PRINTF("%d %S -> %S", rp, np, nb);
        np = nb;
    }
    xfree(log->logFile);
    log->logFile = xwmakepath(service->logs, np, NULL);
    if (rp) {
        rc = rotateprevlogs(log, ssp);
        if (rc)
            return rc;
    }
    fh = CreateFileW(log->logFile,
                     GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ, NULL, cd,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(fh))
        return xsyserror(rc, log->logFile, NULL);
#if defined(_DEBUG)
    DBG_PRINTF("%S", log->logFile);
#endif
    InterlockedExchange64(&log->size, 0);
    InterlockedExchangePointer(&log->fd, fh);

    return 0;
}

static DWORD rotatelogs(LPSVCBATCH_LOG log)
{
    DWORD  rc = 0;
    HANDLE h  = NULL;

    SVCBATCH_CS_ENTER(log);
    InterlockedExchange(&log->state, 1);

    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h == NULL) {
        rc = ERROR_FILE_NOT_FOUND;
        goto finished;
    }
    InterlockedIncrement(&log->count);
    FlushFileBuffers(h);
    CloseHandle(h);
    rc = openlogfile(log, FALSE);
    if (rc)
        setsvcstatusexit(rc);

finished:
    InterlockedExchange64(&log->size, 0);
    SVCBATCH_CS_LEAVE(log);
    return rc;
}

static DWORD closelogfile(LPSVCBATCH_LOG log)
{
    HANDLE h;

    if (log == NULL)
        return ERROR_FILE_NOT_FOUND;

    SVCBATCH_CS_ENTER(log);
    DBG_PRINTF("%d %S", log->state, log->logFile);
    InterlockedExchange(&log->state, 1);

    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }

    SVCBATCH_CS_LEAVE(log);
    SVCBATCH_CS_CLOSE(log);
    SAFE_MEM_FREE(log);
    return 0;
}

static void resolvetimeout(int hh, int mm, int ss, int od)
{
    SYSTEMTIME     st;
    FILETIME       ft;
    ULARGE_INTEGER ui;

    rotateinterval = od ? ONE_DAY : ONE_HOUR;
    if (IS_SET(SVCBATCH_OPT_LOCALTIME) && od)
        GetLocalTime(&st);
    else
        GetSystemTime(&st);

    SystemTimeToFileTime(&st, &ft);
    ui.HighPart  = ft.dwHighDateTime;
    ui.LowPart   = ft.dwLowDateTime;
    ui.QuadPart += rotateinterval;
    ft.dwHighDateTime = ui.HighPart;
    ft.dwLowDateTime  = ui.LowPart;
    FileTimeToSystemTime(&ft, &st);
    if (od)
        st.wHour = hh;
    st.wMinute = mm;
    st.wSecond = ss;
    SystemTimeToFileTime(&st, &ft);
    rotatetime.HighPart = ft.dwHighDateTime;
    rotatetime.LowPart  = ft.dwLowDateTime;

    OPT_SET(SVCBATCH_OPT_ROTATE_BY_TIME);
}

static BOOL resolverotate(LPCWSTR param)
{
    LPWSTR  ep;
    LPCWSTR rp = param;

    ASSERT_WSTR(rp, FALSE);

    rotateinterval      = CPP_INT64_C(0);
    rotatetime.QuadPart = CPP_INT64_C(0);

    if (*rp == L'@') {
        rp++;
        if (xwcschr(rp, L':')) {
            int hh, mm, ss;

            hh = xwcstoi(rp, &ep);
            if ((hh < 0) || (hh > 23))
                return FALSE;
            if (*ep != L':')
                return FALSE;
            rp = ep + 1;
            mm = xwcstoi(rp, &ep);
            if ((mm < 0) || (mm > 59))
                return FALSE;
            if (*ep != L':')
                return FALSE;
            rp = ep + 1;
            ss = xwcstoi(rp, &ep);
            if ((ss < 0) || (ss > 59))
                return FALSE;

            DBG_PRINTF("rotate each day at %.2d:%.2d:%.2d",
                       hh, mm, ss);
            resolvetimeout(hh, mm, ss, 1);
            rp = ep;
            if (*rp != L'+')
                return TRUE;

        }
        else {
            int mm = xwcstoi(rp, &ep);

            if (mm < 0) {
                DBG_PRINTF("invalid rotate value %S", rp);
                return FALSE;
            }
            if (mm == 0) {
                DBG_PRINTS("rotate at midnight");
                resolvetimeout(0, 0, 0, 1);
            }
            else if (mm == 60) {
                DBG_PRINTS("rotate on each full hour");
                resolvetimeout(0, 0, 0, 0);
            }
            else {
                if (mm < SVCBATCH_MIN_ROTATE_INT) {
                    DBG_PRINTF("rotate time %d is less then %d minutes",
                               mm, SVCBATCH_MIN_ROTATE_INT);
                    return FALSE;
                }
                rotateinterval = mm * ONE_MINUTE * CPP_INT64_C(-1);
                DBG_PRINTF("rotate each %d minutes", mm);
                rotatetime.QuadPart = rotateinterval;
                OPT_SET(SVCBATCH_OPT_ROTATE_BY_TIME);
            }
            rp = ep;
            if (*rp != L'+')
                return TRUE;
        }
    }
    if (*rp == L'+') {
        rp++;
        if (xwcspbrk(rp, L"BKMG")) {
            int      val;
            LONGLONG siz;
            LONGLONG mux = CPP_INT64_C(0);

            val = xwcstoi(rp, &ep);
            if (val < 1)
                return FALSE;
            switch (*ep) {
                case L'B':
                    mux = CPP_INT64_C(1);
                break;
                case L'K':
                    mux = KILOBYTES(1);
                break;
                case L'M':
                    mux = MEGABYTES(1);
                break;
                case L'G':
                    mux = MEGABYTES(1024);
                break;
                default:
                    return FALSE;
                break;
            }
            siz = val * mux;
            if (siz < KILOBYTES(SVCBATCH_MIN_ROTATE_SIZ)) {
                DBG_PRINTF("rotate size is less then %dK",
                           SVCBATCH_MIN_ROTATE_SIZ);
                return FALSE;
            }
            else {
                DBG_PRINTF("rotate if larger then %d %C", val, *ep);
                rotatesize = siz;
                OPT_SET(SVCBATCH_OPT_ROTATE_BY_SIZE);
                return TRUE;
            }
        }
    }
    DBG_PRINTF("invalid rotate format %S", param);
    return TRUE;
}

static DWORD addshmemdata(LPWSTR d, LPDWORD x, LPCWSTR s)
{
    DWORD i = *x;

    if (IS_EMPTY_WCS(s))
        return 0;
    if (i >= SVCBATCH_DATA_LEN)
        return 0;
    *x = xwcsncat(d, SVCBATCH_DATA_LEN, i, s) + 1;
    return i;
}

static DWORD runshutdown(void)
{
    WCHAR rb[SVCBATCH_UUID_MAX];
    DWORD i;
    DWORD rc = 0;
    DWORD x  = 4;

    DBG_PRINTS("started");
    i = xwcslcpy(rb, SVCBATCH_UUID_MAX, SVCBATCH_MMAPPFX);
    xuuidstring(rb + i);
    sharedmmap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                    PAGE_READWRITE, 0,
                                    DSIZEOF(SVCBATCH_IPC), rb + 2);
    if (sharedmmap == NULL)
        return GetLastError();
    sharedmem = (LPSVCBATCH_IPC)MapViewOfFile(sharedmmap,
                                              FILE_MAP_ALL_ACCESS,
                                              0, 0, DSIZEOF(SVCBATCH_IPC));
    if (sharedmem == NULL)
        return GetLastError();
    sharedmem->ppid      = program->pInfo.dwProcessId;
    sharedmem->options   = svcoptions & 0x000000FF;
    sharedmem->timeout   = stoptimeout;
    sharedmem->killdepth = killdepth;
    sharedmem->maxlogs   = stopmaxlogs;

    sharedmem->application = addshmemdata(sharedmem->data, &x, cmdproc->application);
    sharedmem->script      = addshmemdata(sharedmem->data, &x, svcstop->script);
    sharedmem->name = addshmemdata(sharedmem->data, &x, service->name);
    sharedmem->temp = addshmemdata(sharedmem->data, &x, service->temp);
    sharedmem->work = addshmemdata(sharedmem->data, &x, service->work);
    sharedmem->logs = addshmemdata(sharedmem->data, &x, service->logs);
    sharedmem->logn = addshmemdata(sharedmem->data, &x, stoplogname);

    sharedmem->argc      = svcstop->argc;
    sharedmem->optc      = cmdproc->optc;
    for (i = 0; i < svcstop->argc; i++)
        sharedmem->args[i] = addshmemdata(sharedmem->data, &x, svcstop->args[i]);
    for (i = 0; i < cmdproc->optc; i++)
        sharedmem->opts[i] = addshmemdata(sharedmem->data, &x, cmdproc->opts[i]);
    if (x >= SVCBATCH_DATA_LEN)
        return ERROR_INSUFFICIENT_BUFFER;
    DBG_PRINTF("shared memory size %lu", DSIZEOF(SVCBATCH_IPC));
    rc = createiopipes(&svcstop->sInfo, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }

    svcstop->application = program->application;
    svcstop->commandLine = xappendarg(1, NULL, svcstop->application);
    svcstop->commandLine = xappendarg(0, svcstop->commandLine, rb);
    DBG_PRINTF("cmdline %S", svcstop->commandLine);

    svcstop->sInfo.dwFlags     = STARTF_USESHOWWINDOW;
    svcstop->sInfo.wShowWindow = SW_HIDE;
    if (!CreateProcessW(svcstop->application,
                        svcstop->commandLine,
                        NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
                        NULL, NULL,
                        &svcstop->sInfo,
                        &svcstop->pInfo)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", svcstop->application);
        goto finished;
    }
    InterlockedExchange(&svcstop->state, SVCBATCH_PROCESS_RUNNING);
    SAFE_CLOSE_HANDLE(svcstop->pInfo.hThread);
    SAFE_CLOSE_HANDLE(svcstop->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(svcstop->sInfo.hStdError);

    DBG_PRINTF("waiting %lu ms for shutdown process %lu",
               stoptimeout, svcstop->pInfo.dwProcessId);
    rc = WaitForSingleObject(svcstop->pInfo.hProcess, stoptimeout);
    if (rc == WAIT_OBJECT_0)
        GetExitCodeProcess(svcstop->pInfo.hProcess, &rc);

finished:
    svcstop->exitCode = rc;
    closeprocess(svcstop);
    DBG_PRINTF("done %lu", rc);
    return rc;
}

static DWORD cmdshutdown(void)
{
    DWORD i;
    DWORD rc;

    DBG_PRINTS("started");
    rc = createiopipes(&svcstop->sInfo, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }

    svcstop->commandLine = xappendarg(1, NULL, svcstop->application);
    for (i = 0; i < svcstop->argc; i++)
        svcstop->commandLine = xappendarg(1, svcstop->commandLine, svcstop->args[i]);
    DBG_PRINTF("cmdline %S", svcstop->commandLine);

    svcstop->sInfo.dwFlags     = STARTF_USESHOWWINDOW;
    svcstop->sInfo.wShowWindow = SW_HIDE;
    if (!CreateProcessW(svcstop->application,
                        svcstop->commandLine,
                        NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
                        NULL, NULL,
                        &svcstop->sInfo,
                        &svcstop->pInfo)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", svcstop->application);
        goto finished;
    }
    InterlockedExchange(&svcstop->state, SVCBATCH_PROCESS_RUNNING);
    SAFE_CLOSE_HANDLE(svcstop->pInfo.hThread);
    SAFE_CLOSE_HANDLE(svcstop->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(svcstop->sInfo.hStdError);

    DBG_PRINTF("waiting %lu ms for shutdown process %lu",
               stoptimeout, svcstop->pInfo.dwProcessId);
    rc = WaitForSingleObject(svcstop->pInfo.hProcess, stoptimeout);
    if (rc == WAIT_OBJECT_0)
        GetExitCodeProcess(svcstop->pInfo.hProcess, &rc);
    else
        killprocess(svcstop, rc);
finished:
    svcstop->exitCode = rc;
    closeprocess(svcstop);
    DBG_PRINTF("done %lu", rc);
    return rc;
}

static DWORD WINAPI stopthread(void *ssp)
{
    DWORD rc = 0;
    DWORD ws = WAIT_OBJECT_1;
    int   ri = stoptimeout;
    ULONGLONG rs;

    ResetEvent(svcstopdone);
    SetEvent(stopstarted);

    if (ssp == NULL)
        xsvcstatus(SERVICE_STOP_PENDING, stoptimeout + SVCBATCH_STOP_HINT);
    DBG_PRINTS("started");
    if (outputlog) {
        SVCBATCH_CS_ENTER(outputlog);
        InterlockedExchange(&outputlog->state, 1);
        SVCBATCH_CS_LEAVE(outputlog);
    }
    if (svcstop) {
        rs = GetTickCount64();
        DBG_PRINTS("creating shutdown process");
        if (svcstop->application)
            rc = cmdshutdown();
        else
            rc = runshutdown();
        ri = (int)(GetTickCount64() - rs);
        DBG_PRINTF("shutdown finished with %lu in %d ms", rc, ri);
        xsvcstatus(SERVICE_STOP_PENDING, 0);
        ri = stoptimeout - ri;
        if (ri < SVCBATCH_STOP_SYNC)
            ri = SVCBATCH_STOP_SYNC;
        if (rc == 0) {
            DBG_PRINTF("waiting %d ms for worker", ri);
            ws = WaitForSingleObject(workerended, ri);
            if (ws != WAIT_OBJECT_0) {
                ri = (int)(GetTickCount64() - rs);
                ri = stoptimeout - ri;
                if (ri < SVCBATCH_STOP_SYNC)
                    ri = SVCBATCH_STOP_SYNC;
            }
        }
    }
    if (IS_SET(SVCBATCH_OPT_STOP_FILE)) {
        LPWSTR fn;

        rs = GetTickCount64();
        fn = xwmakepath(service->logs, L"ss-", service->uuid);
        DBG_PRINTF("stop file %S", fn);
        svcstopfile = CreateFileW(fn, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                  NULL);
        if (svcstopfile != INVALID_HANDLE_VALUE) {
            DWORD wr;

            WriteFile(svcstopfile, YYES, 1, &wr, NULL);
            FlushFileBuffers(svcstopfile);
            DBG_PRINTF("waiting %d ms for worker", ri);
            ws = WaitForSingleObject(workerended, ri);
            if (ws != WAIT_OBJECT_0) {
                ri = (int)(GetTickCount64() - rs);
                ri = stoptimeout - ri;
                if (ri < SVCBATCH_STOP_SYNC)
                    ri = SVCBATCH_STOP_SYNC;
            }
        }
        xfree(fn);
    }
    if (IS_SET(SVCBATCH_OPT_STOP_SIGNAL)) {
        rs = GetTickCount64();

        DBG_PRINTS("setting stop signal");
        SetEvent(svcstopssig);
        DBG_PRINTF("waiting %d ms for worker", ri);
        ws = WaitForSingleObject(workerended, ri);
        if (ws != WAIT_OBJECT_0) {
            ri = (int)(GetTickCount64() - rs);
            ri = stoptimeout - ri;
            if (ri < SVCBATCH_STOP_SYNC)
                ri = SVCBATCH_STOP_SYNC;
        }
    }
    if (ws != WAIT_OBJECT_0) {
        xsvcstatus(SERVICE_STOP_PENDING, 0);
        SetConsoleCtrlHandler(NULL, TRUE);
        if (IS_SET(SVCBATCH_OPT_CTRL_BREAK)) {
            DBG_PRINTS("generating CTRL_BREAK_EVENT");
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, cmdproc->pInfo.dwProcessId);
        }
        else {
            DBG_PRINTS("generating CTRL_C_EVENT");
            GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        }
        DBG_PRINTF("waiting %d ms for worker", ri);
        ws = WaitForSingleObject(workerended, ri);
        SetConsoleCtrlHandler(NULL, FALSE);
    }

    xsvcstatus(SERVICE_STOP_PENDING, 0);
    if (ws != WAIT_OBJECT_0) {
        DBG_PRINTS("worker process is still running ... terminating");
        killprocess(cmdproc, ws);
    }
    else {
        DBG_PRINTS("worker process ended");
        cleanprocess(cmdproc);
    }
    xsvcstatus(SERVICE_STOP_PENDING, 0);
    SetEvent(svcstopdone);
    DBG_PRINTS("done");
    return rc;
}

static void createstopthread(DWORD rv)
{
    if (servicemode)
        xcreatethread(SVCBATCH_STOP_THREAD, 0, stopthread, NULL);
    if (rv)
        setsvcstatusexit(rv);
}

static void stopshutdown(DWORD rt)
{
    DWORD ws;

    DBG_PRINTS("started");
    SetConsoleCtrlHandler(NULL, TRUE);
    if (IS_SET(SVCBATCH_OPT_CTRL_BREAK)) {
        DBG_PRINTS("generating CTRL_BREAK_EVENT");
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, cmdproc->pInfo.dwProcessId);
    }
    else {
        DBG_PRINTS("generating CTRL_C_EVENT");
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    }
    ws = WaitForSingleObject(workerended, rt);
    SetConsoleCtrlHandler(NULL, FALSE);

    if (ws != WAIT_OBJECT_0) {
        DBG_PRINTS("worker process is still running ... terminating");
        killprocess(cmdproc, ws);
    }
    else {
        DBG_PRINTS("worker process ended");
        cleanprocess(cmdproc);
    }
    DBG_PRINTS("done");
}

static DWORD logwrdata(LPSVCBATCH_LOG log, BYTE *buf, DWORD len)
{
    DWORD  rc = 0;
    DWORD  wr = 0;
    HANDLE h;

    SVCBATCH_CS_ENTER(log);
    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h == NULL) {
        SVCBATCH_CS_LEAVE(log);
        DBG_PRINTS("logfile closed");
        return ERROR_NO_MORE_FILES;
    }
    if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0))
        InterlockedAdd64(&log->size, wr);
    else
        rc = GetLastError();

    InterlockedExchangePointer(&log->fd, h);
    SVCBATCH_CS_LEAVE(log);
    if (rc)
        return xsyserror(rc, L"LogWrite", NULL);
#if defined(_DEBUG) && (_DEBUG > 2)
    DBG_PRINTF("wrote   %4lu bytes", wr);
#endif
    if (IS_SET(SVCBATCH_OPT_ROTATE_BY_SIZE)) {
        if (log->size >= rotatesize) {
            if (canrotatelogs(log)) {
                DBG_PRINTS("rotating by size");
                SetEvent(dologrotate);
            }
        }
    }
    return 0;
}

static DWORD WINAPI wrpipethread(void *pipe)
{
    DWORD rc = 0;
    DWORD wr;

    DBG_PRINTS("started");
    if (WriteFile(pipe, YYES, 3, &wr, NULL) && (wr != 0)) {
        if (!FlushFileBuffers(pipe))
            rc = GetLastError();
    }
    else {
        rc = GetLastError();
    }
    CloseHandle(pipe);
#if defined(_DEBUG)
    if (rc) {
        if (rc == ERROR_BROKEN_PIPE)
            DBG_PRINTS("pipe closed");
        else if (rc == ERROR_OPERATION_ABORTED)
            DBG_PRINTS("aborted");
        else
            DBG_PRINTF("error %lu", rc);
    }
    DBG_PRINTS("done");
#endif
    return rc;
}

static DWORD WINAPI rotatethread(void *unused)
{
    HANDLE wh[4];
    HANDLE wt = NULL;
    DWORD  rc = 0;
    DWORD  nw = 3;
    DWORD  rw = SVCBATCH_ROTATE_READY;

    DBG_PRINTF("started");

    wh[0] = workerended;
    wh[1] = stopstarted;
    wh[2] = dologrotate;
    wh[3] = NULL;

    InterlockedExchange(&outputlog->state, 1);
    if (IS_SET(SVCBATCH_OPT_ROTATE_BY_TIME)) {
        wt = CreateWaitableTimer(NULL, TRUE, NULL);
        if (IS_INVALID_HANDLE(wt)) {
            rc = xsyserror(GetLastError(), L"CreateWaitableTimer", NULL);
            goto failed;
        }
        if (!SetWaitableTimer(wt, &rotatetime, 0, NULL, NULL, FALSE)) {
            rc = xsyserror(GetLastError(), L"SetWaitableTimer", NULL);
            goto failed;
        }
        wh[nw++] = wt;
    }
    while (rc == 0) {
        DWORD wc;

        wc = WaitForMultipleObjects(nw, wh, FALSE, rw);
        switch (wc) {
            case WAIT_OBJECT_0:
                rc = 1;
                DBG_PRINTS("workerended signaled");
            break;
            case WAIT_OBJECT_1:
                rc = 1;
                DBG_PRINTS("stopstarted signaled");
            break;
            case WAIT_OBJECT_2:
                DBG_PRINTS("dologrotate signaled");
                rc = rotatelogs(outputlog);
                if (rc == 0) {
                    if (IS_VALID_HANDLE(wt) && (rotateinterval < 0)) {
                        CancelWaitableTimer(wt);
                        SetWaitableTimer(wt, &rotatetime, 0, NULL, NULL, FALSE);
                    }
                    ResetEvent(dologrotate);
                    rw = SVCBATCH_ROTATE_READY;
                }
            break;
            case WAIT_OBJECT_3:
                DBG_PRINTS("rotate timer signaled");
                if (canrotatelogs(outputlog)) {
                    DBG_PRINTS("rotate by time");
                    rc = rotatelogs(outputlog);
                    rw = SVCBATCH_ROTATE_READY;
                }
                else {
                    DBG_PRINTS("rotate is busy ... canceling timer");
                }
                if (rc == 0) {
                    CancelWaitableTimer(wt);
                    if (rotateinterval > 0)
                        rotatetime.QuadPart += rotateinterval;
                    SetWaitableTimer(wt, &rotatetime, 0, NULL, NULL, FALSE);
                    ResetEvent(dologrotate);
                }
            break;
            case WAIT_TIMEOUT:
                DBG_PRINTS("rotate ready");
                SVCBATCH_CS_ENTER(outputlog);
                InterlockedExchange(&outputlog->state, 0);
                if (IS_SET(SVCBATCH_OPT_ROTATE_BY_SIZE)) {
                    if (outputlog->size >= rotatesize) {
                        InterlockedExchange(&outputlog->state, 1);
                        DBG_PRINTS("rotating by size");
                        SetEvent(dologrotate);
                    }
                }
                SVCBATCH_CS_LEAVE(outputlog);
                rw = INFINITE;
            break;
            default:
                rc = wc;
            break;
        }
    }
    if (rc > 1)
        createstopthread(rc);
    goto finished;

failed:
    setsvcstatusexit(rc);
    if (WaitForSingleObject(workerended, SVCBATCH_STOP_SYNC) == WAIT_TIMEOUT)
        createstopthread(rc);

finished:
    SAFE_CLOSE_HANDLE(wt);
    DBG_PRINTS("done");
    return rc > 1 ? rc : 0;
}

static DWORD WINAPI workerthread(void *unused)
{
    DWORD    i;
    HANDLE   wr = NULL;
    LPHANDLE rp = NULL;
    LPHANDLE wp = NULL;
    DWORD    rc = 0;
    DWORD    cf = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    HANDLE   rd = NULL;
    LPSVCBATCH_PIPE op = NULL;

    DBG_PRINTS("started");
    xsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_STARTING);

    if (outputlog)
        rp = &rd;
    if (IS_SET(SVCBATCH_OPT_WRPIPE))
        wp = &wr;
    rc = createiopipes(&cmdproc->sInfo, wp, rp, FILE_FLAG_OVERLAPPED);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        setsvcstatusexit(rc);
        cmdproc->exitCode = rc;
        goto finished;
    }
    cmdproc->commandLine = xappendarg(1, NULL, cmdproc->application);
    for (i = 0; i < cmdproc->optc; i++)
        cmdproc->commandLine = xappendarg(0, cmdproc->commandLine, cmdproc->opts[i]);
    cmdproc->commandLine = xappendarg(1, cmdproc->commandLine, cmdproc->script);
    for (i = 0; i < cmdproc->argc; i++)
        cmdproc->commandLine = xappendarg(1, cmdproc->commandLine, cmdproc->args[i]);
    if (outputlog) {
        op = (LPSVCBATCH_PIPE)xmcalloc(sizeof(SVCBATCH_PIPE));
        op->pipe = rd;
        op->o.hEvent = CreateEventEx(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(op->o.hEvent)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"CreateEvent", NULL);
            cmdproc->exitCode = rc;
            goto finished;
        }
    }
    xsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    DBG_PRINTF("cmdline %S", cmdproc->commandLine);
    if (IS_SET(SVCBATCH_OPT_CTRL_BREAK))
        cf |= CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(cmdproc->application,
                        cmdproc->commandLine,
                        NULL, NULL, TRUE, cf, NULL,
                        service->work,
                       &cmdproc->sInfo,
                       &cmdproc->pInfo)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        xsyserror(rc, L"CreateProcess", cmdproc->application);
        cmdproc->exitCode = rc;
        goto finished;
    }
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(cmdproc->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(cmdproc->sInfo.hStdError);

    if (IS_SET(SVCBATCH_OPT_WRPIPE)) {
        if (!xcreatethread(SVCBATCH_WRPIPE_THREAD,
                           1, wrpipethread, wr)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"WriteThread", NULL);
            TerminateProcess(cmdproc->pInfo.hProcess, rc);
            cmdproc->exitCode = rc;
            goto finished;
        }
    }
    if (IS_SET(SVCBATCH_OPT_ROTATE)) {
        if (!xcreatethread(SVCBATCH_ROTATE_THREAD,
                           1, rotatethread, NULL)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"RotateThread", NULL);
            TerminateProcess(cmdproc->pInfo.hProcess, rc);
            cmdproc->exitCode = rc;
            goto finished;
        }
    }
    ResumeThread(cmdproc->pInfo.hThread);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_RUNNING);
    if (IS_SET(SVCBATCH_OPT_WRPIPE))
        ResumeThread(threads[SVCBATCH_WRPIPE_THREAD].thread);

    SAFE_CLOSE_HANDLE(cmdproc->pInfo.hThread);
    xsvcstatus(SERVICE_RUNNING, 0);
    DBG_PRINTF("running process %lu", cmdproc->pInfo.dwProcessId);
    if (IS_SET(SVCBATCH_OPT_ROTATE))
        ResumeThread(threads[SVCBATCH_ROTATE_THREAD].thread);
    if (outputlog) {
        HANDLE wh[2];
        DWORD  nw = 2;

        wh[0] = cmdproc->pInfo.hProcess;
        wh[1] = op->o.hEvent;
        do {
            DWORD ws;

            ws = WaitForMultipleObjects(nw, wh, FALSE, INFINITE);
            switch (ws) {
                case WAIT_OBJECT_0:
                    nw = 0;
                    DBG_PRINTS("process signaled");
                break;
                case WAIT_OBJECT_1:
                    if (op->state == ERROR_IO_PENDING) {
                        if (!GetOverlappedResult(op->pipe, (LPOVERLAPPED)op,
                                                &op->read, FALSE)) {
                            op->state = GetLastError();
                        }
                        else {
                            op->state = 0;
                            rc = logwrdata(outputlog, op->buffer, op->read);
                        }
                    }
                    else {
                        if (ReadFile(op->pipe, op->buffer, DSIZEOF(op->buffer),
                                    &op->read, (LPOVERLAPPED)op) && op->read) {
                            op->state = 0;
                            rc = logwrdata(outputlog, op->buffer, op->read);
                            SetEvent(op->o.hEvent);
                        }
                        else {
                            op->state = GetLastError();
                            if (op->state != ERROR_IO_PENDING)
                                rc = op->state;
                        }
                    }
                    if (rc) {
                        SAFE_CLOSE_HANDLE(op->pipe);
                        ResetEvent(op->o.hEvent);
                        nw = 1;
#if defined(_DEBUG)
                        if ((rc == ERROR_BROKEN_PIPE) || (rc == ERROR_NO_DATA))
                            DBG_PRINTS("pipe closed");
                        else if (rc == ERROR_NO_MORE_FILES)
                            DBG_PRINTS("log file closed");
                        else
                            DBG_PRINTF("error %lu", rc);
#endif
                    }
                break;
                default:
                    DBG_PRINTF("wait failed %lu with %lu", ws, GetLastError());
                    nw = 0;
                break;

            }
        } while (nw);
    }
    else {
        WaitForSingleObject(cmdproc->pInfo.hProcess, INFINITE);
    }
    DBG_PRINTS("stopping");
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_STOPPING);
    if (IS_SET(SVCBATCH_OPT_WRPIPE)) {
        if (WaitForSingleObject(threads[SVCBATCH_WRPIPE_THREAD].thread, SVCBATCH_STOP_STEP)) {
            DBG_PRINTS("wrpipethread is still active ... calling CancelSynchronousIo");
            CancelSynchronousIo(threads[SVCBATCH_WRPIPE_THREAD].thread);
        }
    }
    if (!GetExitCodeProcess(cmdproc->pInfo.hProcess, &rc))
        rc = GetLastError();
    if (rc) {
        if ((rc != 0x000000FF) && (rc != 0xC000013A)) {
            /**
             * Discard common error codes
             * 255 is exit code when CTRL_C is send to cmd.exe
             */
            cmdproc->exitCode = rc;
        }
    }
    DBG_PRINTF("finished process %lu with %lu",
               cmdproc->pInfo.dwProcessId,
               cmdproc->exitCode);

finished:
    if (op != NULL) {
        SAFE_CLOSE_HANDLE(op->pipe);
        SAFE_CLOSE_HANDLE(op->o.hEvent);
        free(op);
    }
    closeprocess(cmdproc);

    DBG_PRINTS("done");
    SetEvent(workerended);
    return cmdproc->exitCode;
}

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    BOOL rv = TRUE;

#if defined(_DEBUG)
    switch (ctrl) {
        case CTRL_CLOSE_EVENT:
            DBG_PRINTS("signaled CTRL_CLOSE_EVENT");
        break;
        case CTRL_SHUTDOWN_EVENT:
            DBG_PRINTS("signaled CTRL_SHUTDOWN_EVENT");
        break;
        case CTRL_C_EVENT:
            DBG_PRINTS("signaled CTRL_C_EVENT");
        break;
        case CTRL_BREAK_EVENT:
            DBG_PRINTS("signaled CTRL_BREAK_EVENT");
        break;
        case CTRL_LOGOFF_EVENT:
            DBG_PRINTS("signaled CTRL_LOGOFF_EVENT");
        break;
        default:
            DBG_PRINTF("unknown control %lu", ctrl);
            rv = FALSE;
        break;
    }
#endif
    return rv;
}

static DWORD WINAPI servicehandler(DWORD ctrl, DWORD _xe, LPVOID _xd, LPVOID _xc)
{
    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
            /* fall through */
        case SERVICE_CONTROL_SHUTDOWN:
            /* fall through */
            InterlockedExchange(&killdepth, 0);
        case SERVICE_CONTROL_STOP:
            DBG_PRINTF("service control %lu", ctrl);
            SVCBATCH_CS_ENTER(service);
            if (service->state == SERVICE_RUNNING) {
                xsvcstatus(SERVICE_STOP_PENDING, stoptimeout + SVCBATCH_STOP_HINT);
                xcreatethread(SVCBATCH_STOP_THREAD, 0, stopthread, INVALID_HANDLE_VALUE);
            }
            SVCBATCH_CS_LEAVE(service);
        break;
        case SVCBATCH_CTRL_BREAK:
            if (IS_SET(SVCBATCH_OPT_HAS_CTRL_BREAK)) {
                DBG_PRINTS("service SVCBATCH_CTRL_BREAK signaled");
                /**
                 * Danger Zone!!!
                 *
                 * Send CTRL_BREAK_EVENT to the child process.
                 * This is useful if batch file is running java
                 * CTRL_BREAK signal tells JDK to dump thread stack
                 *
                 */
                GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
            }
            else {
                DBG_PRINTS("ctrl+break is disabled");
                return ERROR_INVALID_SERVICE_CONTROL;
            }
        break;
        case SVCBATCH_CTRL_ROTATE:
            if (IS_SET(SVCBATCH_OPT_ROTATE_BY_SIG)) {
                /**
                 * Signal to rotatethread that
                 * user send custom service control
                 */
                if (canrotatelogs(outputlog)) {
                    DBG_PRINTS("signaling SVCBATCH_CTRL_ROTATE");
                    SetEvent(dologrotate);
                }
                else {
                    DBG_PRINTS("rotatelogs is busy");
                    return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
                }
            }
            else {
                DBG_PRINTS("log rotation is disabled");
                return ERROR_INVALID_SERVICE_CONTROL;
            }
        break;
        case SERVICE_CONTROL_INTERROGATE:
            DBG_PRINTS("SERVICE_CONTROL_INTERROGATE");
        break;
        default:
            DBG_PRINTF("unknown control 0x%08X", ctrl);
            return ERROR_CALL_NOT_IMPLEMENTED;
        break;
    }
    return 0;
}

static void threadscleanup(void)
{
    int    i;
    HANDLE h;

    DBG_PRINTS("started");
    for(i = 0; i < SVCBATCH_MAX_THREADS; i++) {
        h = InterlockedExchangePointer(&threads[i].thread, NULL);
        if (h) {
            if (threads[i].started) {
#if defined(_DEBUG)
                threads[i].duration = GetTickCount64() - threads[i].duration;
#endif
                threads[i].exitCode = ERROR_DISCARDED;
                TerminateThread(h, threads[i].exitCode);
            }
#if defined(_DEBUG)
            DBG_PRINTF("%s %4lu %10llums",
                        threads[i].name,
                        threads[i].exitCode,
                        threads[i].duration);
#endif
            CloseHandle(h);
        }
    }
    DBG_PRINTS("done");
}

static void waitforthreads(DWORD ms)
{
    int i;
    HANDLE wh[SVCBATCH_MAX_THREADS];
    DWORD  nw = 0;

    DBG_PRINTS("started");
    for(i = 0; i < SVCBATCH_MAX_THREADS; i++) {
        if (threads[i].started) {
#if defined(_DEBUG)
            DBG_PRINTF("%s", threads[i].name);
#endif
            wh[nw++] = threads[i].thread;
        }
    }
    if (nw) {
        DBG_PRINTF("wait for %d threads", nw);
        if (nw > 1)
            WaitForMultipleObjects(nw, wh, TRUE, ms);
        else
            WaitForSingleObject(wh[0], ms);
    }
    DBG_PRINTS("done");
}

static void __cdecl consolecleanup(void)
{
    FreeConsole();
    DBG_PRINTS("done");
}

static void __cdecl objectscleanup(void)
{
    SAFE_CLOSE_HANDLE(workerended);
    SAFE_CLOSE_HANDLE(svcstopdone);
    SAFE_CLOSE_HANDLE(stopstarted);
    SAFE_CLOSE_HANDLE(dologrotate);
    SAFE_CLOSE_HANDLE(svcstopfile);
    SAFE_CLOSE_HANDLE(svcstopssig);
    if (sharedmem)
        UnmapViewOfFile(sharedmem);
    SAFE_CLOSE_HANDLE(sharedmmap);
    SVCBATCH_CS_CLOSE(service);
    DBG_PRINTS("done");
}

static DWORD createevents(void)
{
    workerended = CreateEventExW(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(workerended))
        return GetLastError();
    svcstopdone = CreateEventExW(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(svcstopdone))
        return GetLastError();
    stopstarted = CreateEventExW(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(stopstarted))
        return GetLastError();
    if (IS_SET(SVCBATCH_OPT_ROTATE)) {
        dologrotate = CreateEventExW(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(dologrotate))
            return GetLastError();
    }
    if (IS_SET(SVCBATCH_OPT_STOP_SIGNAL)) {
        WCHAR n[SVCBATCH_NAME_MAX];

        xwcslcpy(n, SVCBATCH_NAME_MAX, SVCBATCH_STOPPFX);
        xwcslcat(n, SVCBATCH_NAME_MAX, service->uuid);
        svcstopssig = CreateEventExW(NULL, n,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(svcstopssig))
            return GetLastError();
    }
    return 0;
}

static LPWSTR *mergearguments(LPWSTR msz, int *argc)
{
    int      i;
    int      x = 0;
    int      c = 1;
    LPWSTR   p;
    LPWSTR  *argv;

    if (msz) {
        for (p = msz; *p; p++, c++) {
            while (*p)
                p++;
        }
    }
    c   += svcmainargc;
    argv = xwaalloc(c);

    argv[x++] = (LPWSTR)service->name;
    /**
     * Add option arguments in the following order
     * ImagePath
     * ImagePathArguments
     * Service Start options
     */
    if (svcmainargc > 0) {
        for (i = 0; i < svcmainargc; i++)
            argv[x++] = (LPWSTR)svcmainargv[i];
    }
    if (msz) {
        for (p = msz; *p; p++) {
            argv[x++] = p;
            while (*p)
                p++;
        }
    }
    *argc = x;
#if defined(_DEBUG) && (_DEBUG > 1)
    for (i = 0; i < x; i++) {
        DBG_PRINTF("[%.2d] '%S'", i, argv[i]);
    }
#endif
    return argv;
}

static DWORD getsvcarguments(int *argc, LPWSTR **argv)
{
    DWORD   t;
    DWORD   c;
    HKEY    k = NULL;
    LPBYTE  b = NULL;
    LSTATUS s;

    s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, SYSTEM_SVC_SUBKEY,
                      0, KEY_QUERY_VALUE | KEY_READ, &k);
    if (s != ERROR_SUCCESS)
        goto finished;
    s = RegGetValueW(k, service->name, SVCBATCH_SVCARGS,
                     RRF_RT_REG_MULTI_SZ, &t, NULL, &c);
    if (s != ERROR_SUCCESS)
        goto finished;
    b = (LPBYTE)xmmalloc(c);
    s = RegGetValueW(k, service->name, SVCBATCH_SVCARGS,
                     RRF_RT_REG_MULTI_SZ, &t, b, &c);
    if (s != ERROR_SUCCESS) {
        xfree(b);
        RegCloseKey(k);
        return s;
    }
finished:
    if (k != NULL)
        RegCloseKey(k);
    *argv = mergearguments((LPWSTR)b, argc);
    return ERROR_SUCCESS;
}

static BOOL resolvescript(LPCWSTR bp)
{
    if (cmdproc->script)
        return TRUE;
    if (IS_EMPTY_WCS(bp))
        return TRUE;
    if (*bp == L':') {
        cmdproc->script = xwcsdup(bp + 1);
        return TRUE;
    }
    if (isdotslash(bp)) {
        cmdproc->script = xwcsdup(bp + 2);
        return TRUE;
    }
    cmdproc->script = xgetfinalpath(0, bp);
    if (IS_EMPTY_WCS(cmdproc->script))
        return FALSE;
    if (service->base == NULL) {
        LPWSTR p = xwcsrchr(cmdproc->script, L'\\');
        if (p) {
            *p = WNUL;
            service->base = xwcsdup(cmdproc->script);
            *p = L'\\';
        }
        else {
            SAFE_MEM_FREE(cmdproc->script);
            return FALSE;
        }
    }
    return TRUE;
}

static int parseoptions(int sargc, LPWSTR *sargv)
{
    DWORD    x;
    int      i;
    int      opt;
    int      cenvc = 0;
    int      eenvc = 0;
    int      uenvc = 0;
    int      wargc = 0;
    LPWSTR  *wargv;
    LPWSTR   eenvn[SVCBATCH_MAX_ENVS];
    LPWSTR   eenvv[SVCBATCH_MAX_ENVS];
    LPWSTR   uenvn[SVCBATCH_MAX_ENVS];
    WCHAR    uenvv[SVCBATCH_MAX_ENVS];
    LPCWSTR  cenvn[SVCBATCH_MAX_ENVS];

    LPCWSTR  cp;
    LPWSTR   wp;
    LPWSTR   pp;
    LPWSTR   scriptparam  = NULL;
    LPWSTR   svclogfname  = NULL;
    LPCWSTR  svcbaseparam = NULL;
    LPCWSTR  svchomeparam = NULL;
    LPCWSTR  svcworkparam = NULL;
    LPCWSTR  svcstopparam = NULL;
    LPCWSTR  commandparam = NULL;
    LPCWSTR  rotateparam  = NULL;
    LPCWSTR  outdirparam  = NULL;

    DBG_PRINTS("started");
    x = getsvcarguments(&wargc, &wargv);
    if (x != ERROR_SUCCESS)
        return xsyserror(x, SVCBATCH_SVCARGS, NULL);

    while ((opt = xwgetopt(wargc, wargv, scmdoptions)) != EOF) {
        switch (opt) {
            case '[':
                xwoptend = 1;
            break;
            case ']':
                xwoptarr = 0;
                xwoptend = 0;
            break;
            case 'C':
                if (cmdproc->optc < SVCBATCH_MAX_ARGS)
                    cmdproc->opts[cmdproc->optc++] = xwoptarg;
                else
                    return xsyserrno(16, L"c", xwoptarg);
            break;
            case 'S':
                if (svcstop == NULL)
                    svcstop = (LPSVCBATCH_PROCESS)xmcalloc(sizeof(SVCBATCH_PROCESS));
                if (svcstop->argc < SVCBATCH_MAX_ARGS)
                    svcstop->args[svcstop->argc++] = xwoptarg;
                else
                    return xsyserrno(16, L"s", xwoptarg);
            break;
            case 'f':
                cp = xwoptarg;
                while (*xwoptarg) {
                    switch (*xwoptarg) {
                        case L'B':
                            OPT_SET(SVCBATCH_OPT_CTRL_BREAK);
                        break;
                        case L'C':
                            OPT_SET(SVCBATCH_OPT_HAS_CTRL_BREAK);
                        break;
                        case L'E':
                            OPT_SET(SVCBATCH_OPT_STOP_SIGNAL);
                        break;
                        case L'F':
                            OPT_SET(SVCBATCH_OPT_STOP_FILE);
                        break;
                        case L'L':
                            OPT_SET(SVCBATCH_OPT_LOCALTIME);
                        break;
                        case L'Q':
                            OPT_SET(SVCBATCH_OPT_QUIET);
                        break;
                        case L'P':
                            preshutdown = SERVICE_ACCEPT_PRESHUTDOWN;
                        break;
                        case L'R':
                            OPT_SET(SVCBATCH_OPT_ROTATE_BY_SIG);
                            OPT_SET(SVCBATCH_OPT_ROTATE);
                        break;
                        case L'U':
                            OPT_CLR(SVCBATCH_OPT_ENV);
                        break;
                        case L'Y':
                            OPT_SET(SVCBATCH_OPT_WRPIPE);
                        break;
                        case L'0':
                            svcfailmode = SVCBATCH_FAIL_ERROR;
                        break;
                        case L'1':
                            svcfailmode = SVCBATCH_FAIL_NONE;
                        break;
                        case L'2':
                            svcfailmode = SVCBATCH_FAIL_EXIT;
                        break;
                        default:
                            xsyserrno(12, L"f", cp);
                        break;
                    }
                    xwoptarg++;
                }
            break;
            /**
             * Options with arguments
             */
            case 'n':
                if (svclogfname)
                    return xsyserrno(10, L"n", xwoptarg);
                if (xwcspbrk(xwoptarg, L"\\:;<>?*|\""))
                    return xsyserrno(20, L"n", xwoptarg);
                if (*xwoptarg == L'/') {
                    /* Set only stop log name */
                    stoplogname = xwoptarg + 1;
                }
                else {
                    svclogfname = xwcsdup(xwoptarg);
                    wp = xwcschr(svclogfname, L'/');
                    if (wp != NULL) {
                        *(wp++) = WNUL;
                        stoplogname = wp;
                    }
                }
            break;
            case 'c':
                if (commandparam)
                    return xsyserrno(10, L"c", xwoptarg);
                commandparam = skipdotslash(xwoptarg);
                if (commandparam == NULL)
                    return xsyserrno(11, L"c", xwoptarg);
                xwoptarr     = 'C';
            break;
            case 's':
                if (svcstopparam)
                    return xsyserrno(10, L"s", xwoptarg);
                svcstopparam = xwoptarg;
                xwoptarr     = 'S';
                if (svcstop == NULL)
                    svcstop  = (LPSVCBATCH_PROCESS)xmcalloc(sizeof(SVCBATCH_PROCESS));
            break;
            case 'r':
                if (rotateparam)
                    return xsyserrno(10, L"r", xwoptarg);
                rotateparam = xwoptarg;
            break;
            case 'b':
                if (svcbaseparam)
                    return xsyserrno(10, L"b", xwoptarg);
                svcbaseparam = skipdotslash(xwoptarg);
            break;
            case 'h':
                if (svchomeparam)
                    return xsyserrno(10, L"h", xwoptarg);
                svchomeparam = skipdotslash(xwoptarg);
            break;
            case 'o':
                if (outdirparam)
                    return xsyserrno(10, L"o", xwoptarg);
                outdirparam  = skipdotslash(xwoptarg);
            break;
            case 'w':
                if (svcworkparam)
                    return xsyserrno(10, L"w", xwoptarg);
                svcworkparam = skipdotslash(xwoptarg);
            break;
            case 'k':
                killdepth = xwcstoi(xwoptarg, NULL);
                if ((killdepth < 0) || (killdepth > SVCBATCH_MAX_KILLDEPTH))
                    return xsyserrno(13, L"k", xwoptarg);
            break;
            case 'm':
                if (*xwoptarg != L'.')
                    srvcmaxlogs = xwcstoi(xwoptarg, &wp);
                else
                    wp = (LPWSTR)xwoptarg;
                if (*wp == L'.')
                    stopmaxlogs = xwcstoi(wp + 1, NULL);
                if ((srvcmaxlogs < 0) || (srvcmaxlogs > SVCBATCH_MAX_LOGS))
                    return xsyserrno(13, L"m", xwoptarg);
                if ((stopmaxlogs < 0) || (stopmaxlogs > SVCBATCH_MAX_LOGS))
                    return xsyserrno(13, L"m", xwoptarg);
            break;
            case 't':
                stoptimeout = xwcstoi(xwoptarg, NULL);
                if ((stoptimeout < SVCBATCH_STOP_TMIN) || (stoptimeout > SVCBATCH_STOP_TMAX))
                    return xsyserrno(13, L"t", xwoptarg);
                stoptimeout = stoptimeout * 1000;
            break;
            /**
             * Options that can be defined
             * multiple times
             */
            case 'e':
                if (*xwoptarg == L'/') {
                    if (xenvprefix)
                        return xsyserrno(10, L"e", xwoptarg);
                    xenvprefix = xwoptarg;
                    break;
                }
                if (*xwoptarg == L'=') {
                    if (!xisvalidvarname(xwoptarg + 1))
                        return xsyserrno(14, L"e", xwoptarg);
                    uenvprefix = xwoptarg + 1;
                    break;
                }
                pp = xwcsdup(xwoptarg);
                wp = xwcschr(pp, L'=');
                if (wp == NULL) {
                    xfree(pp);
                    if (cenvc < SVCBATCH_MAX_ENVS)
                        cenvn[cenvc++] = xwoptarg;
                    else
                        return xsyserrno(17, L"e", xwoptarg);
                }
                else {
                    *(wp++) = WNUL;
                    if ((wp[0] == L'$') && (wp[1] == L'_') && isalpha(wp[2]) &&
                        (wp[3] == L'$') && (wp[4] == WNUL)) {
                        if (!xisvalidvarname(pp))
                            return xsyserrno(14, L"e", pp);
                        if (uenvc < SVCBATCH_MAX_ENVS) {
                            uenvn[uenvc] = pp;
                            uenvv[uenvc] = xtoupper(wp[2]);
                            uenvc++;
                        }
                        else {
                            return xsyserrno(17, L"e", xwoptarg);
                        }
                    }
                    else {
                        if (eenvc < SVCBATCH_MAX_ENVS) {
                            eenvn[eenvc] = pp;
                            eenvv[eenvc] = wp;
                            eenvc++;
                        }
                        else {
                            return xsyserrno(17, L"e", xwoptarg);
                        }
                    }
                }
            break;
            case ENOENT:
                return xsyserrno(11, xwoption, NULL);
            break;
            default:
                return xsyserrno(22, xwoption, NULL);
            break;
        }
    }
    if (IS_SET(SVCBATCH_OPT_CTRL_BREAK) &&
        IS_SET(SVCBATCH_OPT_HAS_CTRL_BREAK)) {
        return xsyserrno(21, L"features B and C", xwoptarg);
    }
    if (IS_SET(SVCBATCH_OPT_STOP_FILE) &&
        IS_SET(SVCBATCH_OPT_STOP_SIGNAL)) {
        return xsyserrno(21, L"features E and F", xwoptarg);
    }
    if (IS_SET(SVCBATCH_OPT_STOP_FILE)   && svcstopparam)
        return xsyserrno(21, L"feature F and option s", xwoptarg);
    if (IS_SET(SVCBATCH_OPT_STOP_SIGNAL) && svcstopparam)
        return xsyserrno(21, L"feature E and option s", xwoptarg);

    wargc -= xwoptind;
    wargv += xwoptind;

    if (wargc == 0) {
        /**
         * No script file defined.
         */
        if (commandparam == NULL)
            scriptparam = xwcsconcat(service->name, L".bat");
    }
    else {
        scriptparam = xwcsdup(wargv[0]);
        for (i = 1; i < wargc; i++) {
            /**
             * Add arguments for script file
             */
            if (cmdproc->argc < SVCBATCH_MAX_ARGS)
                cmdproc->args[cmdproc->argc++] = wargv[i];
            else
                return xsyserrno(17, L"script",  wargv[i]);
        }
    }
    for (i = 1; i < sargc; i++) {
        if (cmdproc->argc < SVCBATCH_MAX_ARGS)
            cmdproc->args[cmdproc->argc++] = sargv[i];
        else
            return xsyserrno(17, L"script",  sargv[i]);
#if defined(_DEBUG) && (_DEBUG > 1)
        DBG_PRINTF("[%.2d] '%S'", wargc + xwoptind + i - 1, sargv[i]);
#endif
    }
    if (IS_SET(SVCBATCH_OPT_QUIET)) {
        /**
         * Discard any log rotate related command options
         * when -q is defined
         */
        svcoptions &= 0x00000FFF;
        rotateparam = NULL;
        stoplogname = NULL;
        svclogfname = NULL;
        outdirparam = NULL;
        stopmaxlogs = 0;
        srvcmaxlogs = 0;
    }
    else {
        outputlog = (LPSVCBATCH_LOG)xmcalloc(sizeof(SVCBATCH_LOG));

        outputlog->logName = svclogfname ? svclogfname : SVCBATCH_LOGNAME;
        SVCBATCH_CS_INIT(outputlog);
    }
    /**
     * Find the location of SVCBATCH_SERVICE_HOME
     * all relative paths are resolved against it.
     *
     * 1. If -h is defined and is absolute path
     *    set it as SetCurrentDirectory and use it as
     *    home directory for resolving other relative paths
     *
     * 2. If batch file is defined as absolute path
     *    set it as SetCurrentDirectory and resolve -h parameter
     *    if defined as relative path. If -h was defined and
     *    and is resolved as valid path set it as home directory.
     *    If -h was defined and cannot be resolved fail.
     *
     * 3. Use running svcbatch.exe directory and set it as
     *    SetCurrentDirectory.
     *    If -h parameter was defined, resolve it and set as home
     *    directory or fail.
     *    In case -h was not defined resolve batch file and set its
     *    directory as home directory or fail if it cannot be resolved.
     *
     */
    if (isabsolutepath(svcbaseparam)) {
        service->base = xgetfinalpath(1, svcbaseparam);
        if (IS_EMPTY_WCS(service->base))
            return xsyserror(ERROR_FILE_NOT_FOUND, svcbaseparam, NULL);
    }
    if (isabsolutepath(scriptparam)) {
        if (!resolvescript(scriptparam))
            return xsyserror(ERROR_FILE_NOT_FOUND, scriptparam, NULL);
    }
    if (isabsolutepath(svchomeparam)) {
        service->home = xgetfinalpath(1, svchomeparam);
        if (IS_EMPTY_WCS(service->home))
            return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
    }
    else {
        if (svchomeparam == NULL) {
            if (service->base)
                service->home = service->base;
            else
                service->home = program->directory;
        }
        else {
            if (service->base)
                SetCurrentDirectoryW(service->base);
            else
                SetCurrentDirectoryW(program->directory);
            service->home = xgetfinalpath(1, svchomeparam);
            if (IS_EMPTY_WCS(service->home))
                return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
        }
    }
    SetCurrentDirectoryW(service->home);
    if (svcworkparam == NULL) {
        /* Use the same directories for home and work */
        service->work = service->home;
    }
    else {
        service->work = xgetfinalpath(2, svcworkparam);
        if (IS_EMPTY_WCS(service->work))
            return xsyserror(ERROR_FILE_NOT_FOUND, svcworkparam, NULL);
    }
    if (service->temp == NULL)
        service->temp = service->work;
    if (isrelativepath(svcbaseparam)) {
        xfree(service->base);
        service->base = xgetfinalpath(1, svcbaseparam);
        if (IS_EMPTY_WCS(service->base))
            return xsyserror(ERROR_FILE_NOT_FOUND, svcbaseparam, NULL);
    }
    if (!resolvescript(scriptparam))
        return xsyserror(ERROR_FILE_NOT_FOUND, scriptparam, NULL);
    if (service->base == NULL)
        service->base = service->work;
    if (outputlog) {
        if (outdirparam == NULL)
            outdirparam = SVCBATCH_LOGSDIR;
        x = createlogsdir(outdirparam);
        if (x)
            return x;
    }
    else {
        /**
         * Use temp directory as logs directory
         * for quiet mode.
         */
        service->logs = service->temp;
    }
    /**
     * Add additional environment variables
     * They are unique to this service instance
     */
    if (IS_SET(SVCBATCH_OPT_ENV)) {
        xsetsvcenv(uenvprefix, L"_BASE", service->base);
        xsetsvcenv(uenvprefix, L"_HOME", service->home);
        xsetsvcenv(uenvprefix, L"_LOGS", service->logs);
        xsetsvcenv(uenvprefix, L"_NAME", service->name);
        xsetsvcenv(uenvprefix, L"_TEMP", service->temp);
        xsetsvcenv(uenvprefix, L"_UUID", service->uuid);
        xsetsvcenv(uenvprefix, L"_WORK", service->work);
    }

    for (i = 0; i < uenvc; i++) {
        x = xsetusrenv(uenvn[i], uenvv[i]);
        if (x)
            return xsyserror(x, L"SetUserEnvironment", uenvn[i]);
        xfree(uenvn[i]);
    }
    for (i = 0; i < eenvc; i++) {
        x = xsetenvvar(eenvn[i], eenvv[i]);
        if (x)
            return xsyserror(x, eenvn[i], eenvv[i]);
        xfree(eenvn[i]);
    }
    if (commandparam) {
        /**
         * Search the current working folder,
         * and then search the folders that are
         * specified in the system path.
         *
         * This is the system default value.
         */
        wp = xexpandenvstr(commandparam);
        if (wp == NULL)
            return xsyserror(GetLastError(), commandparam, NULL);
        SetSearchPathMode(BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE);
        if (isrelativepath(wp))
            cmdproc->application = xsearchexe(wp);
        else
            cmdproc->application = xgetfinalpath(0, wp);
        if (cmdproc->application == NULL)
            return xsyserror(ERROR_FILE_NOT_FOUND, wp, NULL);
        xfree(wp);
        for (x = 0; x < cmdproc->argc; x++) {
            wp = xexpandenvstr(cmdproc->args[x]);
            if (wp == NULL)
                return xsyserror(GetLastError(), L"ExpandEnvironment", cmdproc->args[x]);
            cmdproc->args[x] = wp;
        }
    }
    else {
        wp = xgetenv(L"COMSPEC");
        if (wp == NULL)
            return xsyserror(ERROR_BAD_ENVIRONMENT, L"COMSPEC", NULL);

        cmdproc->application = xgetfinalpath(0, wp);
        if (cmdproc->application == NULL)
            return xsyserror(ERROR_FILE_NOT_FOUND, wp, NULL);
        xfree(wp);
        cmdproc->opts[cmdproc->optc++] = SVCBATCH_DEF_ARGS;
        OPT_SET(SVCBATCH_OPT_WRPIPE);
        xenvprefix = NULL;
    }
    if (rotateparam) {
        if (!resolverotate(rotateparam))
            return xsyserrno(12, L"r", rotateparam);
        OPT_SET(SVCBATCH_OPT_ROTATE);
    }
    if (svcstopparam) {
        if (xiswcschar(svcstopparam, L'@')) {
            svcstop->script = cmdproc->script;
            if (svcstop->argc == 0)
                svcstop->args[svcstop->argc++] = L"stop";
        }
        else if (*svcstopparam == L'$') {
            /**
             * Use different stop application
             */
            wp = xexpandenvstr(skipdotslash(svcstopparam));
            if (wp == NULL)
                return xsyserror(GetLastError(), svcstopparam, NULL);
            SetSearchPathMode(BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE);
            if (isrelativepath(wp))
                svcstop->application = xsearchexe(wp);
            else
                svcstop->application = xgetfinalpath(0, wp);
            if (svcstop->application == NULL)
                return xsyserror(ERROR_FILE_NOT_FOUND, wp, NULL);
            xfree(wp);
            for (x = 0; x < svcstop->argc; x++) {
                wp = xexpandenvstr(svcstop->args[x]);
                if (wp == NULL)
                    return xsyserror(GetLastError(), L"ExpandEnvironment", svcstop->args[x]);
                svcstop->args[x] = wp;
            }
        }
        else {
            if (*svcstopparam == ':')
                svcstop->script = xwcsdup(svcstopparam + 1);
            else if (isdotslash(svcstopparam))
                svcstop->script = xwcsdup(svcstopparam + 2);
            else
                svcstop->script = xgetfinalpath(0, svcstopparam);
            if (svcstop->script == NULL)
                return xsyserror(ERROR_FILE_NOT_FOUND, svcstopparam, NULL);
        }
        if ((stopmaxlogs > 0) && (svclogfname == NULL) && (stoplogname == NULL))
            stoplogname = SVCBATCH_LOGSTOP;
        if (stoplogname == NULL)
            stopmaxlogs = 0;
    }
    else {
        SAFE_MEM_FREE(svcstop);
    }

    for (i = 0; i < cenvc; i++)
        SetEnvironmentVariableW(cenvn[i], NULL);

    xfree(scriptparam);
    DBG_PRINTS("done");
    return 0;
}

static void WINAPI servicemain(DWORD argc, LPWSTR *argv)
{
    DWORD  rv = 0;

    DBG_PRINTS("started");
    service->status.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    service->status.dwCurrentState = SERVICE_START_PENDING;

    SVCBATCH_CS_INIT(service);
    atexit(objectscleanup);

    if (argc > 0)
        service->name = argv[0];
    if (IS_EMPTY_WCS(service->name)) {
        xsyserror(ERROR_INVALID_PARAMETER, L"InvalidServiceName", NULL);
        exit(1);
    }
    service->handle = RegisterServiceCtrlHandlerExW(service->name, servicehandler, NULL);
    if (IS_INVALID_HANDLE(service->handle)) {
        xsyserror(GetLastError(), L"RegisterServiceCtrlHandlerEx", service->name);
        exit(1);
    }
    DBG_PRINTF("%S", service->name);
    xsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    service->uuid = xuuidstring(NULL);
    if (IS_EMPTY_WCS(service->uuid)) {
        rv = xsyserror(GetLastError(), L"SVCBATCH_SERVICE_UUID", NULL);
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    rv = parseoptions(argc, argv);
    if (rv) {
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    rv = createevents();
    if (rv) {
        xsyserror(rv, L"CreateEvent", NULL);
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    if (outputlog) {
        xsvcstatus(SERVICE_START_PENDING, 0);

        rv = openlogfile(outputlog, TRUE);
        if (rv != 0) {
            xsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
    }
    xsvcstatus(SERVICE_START_PENDING, 0);

    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rv = xsyserror(GetLastError(), L"WorkerThread", NULL);
        goto finished;
    }
    WaitForSingleObject(threads[SVCBATCH_WORKER_THREAD].thread, INFINITE);
    SVCBATCH_CS_ENTER(service);
    if (InterlockedExchange(&service->state, SERVICE_STOP_PENDING) != SERVICE_STOP_PENDING) {
        /**
         * Service ended without stop signal
         */
        DBG_PRINTS("ended without SERVICE_CONTROL_STOP");
    }
    SVCBATCH_CS_LEAVE(service);
    DBG_PRINTS("waiting for stop to finish");
    WaitForSingleObject(svcstopdone, stoptimeout);
    waitforthreads(SVCBATCH_STOP_STEP);

finished:
    DBG_PRINTS("closing");
    closelogfile(outputlog);
    threadscleanup();
    xsvcstatus(SERVICE_STOPPED, rv);
    DBG_PRINTS("done");
}

static DWORD svcstopmain(void)
{
    DWORD rc;

    workerended = CreateEventEx(NULL, NULL,
                                CREATE_EVENT_MANUAL_RESET,
                                EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(workerended))
        return GetLastError();
    SVCBATCH_CS_INIT(service);
    atexit(objectscleanup);

    DBG_PRINTF("%S (%lu) 0x%08X", service->name, cmdproc->ppid, svcoptions);
    if (outputlog) {
        rc = openlogfile(outputlog, FALSE);
        if (rc)
            return rc;
    }
    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rc = xsyserror(GetLastError(), L"WorkerThread", NULL);
        setsvcstatusexit(rc);
        goto finished;
    }
    rc = WaitForSingleObject(threads[SVCBATCH_WORKER_THREAD].thread, stoptimeout);
    if (rc != WAIT_OBJECT_0) {
        DBG_PRINTS("stop timeout");
        stopshutdown(SVCBATCH_STOP_SYNC);
        setsvcstatusexit(rc);
        DBG_PRINTS("waiting for worker thread to finish");
    }
    waitforthreads(SVCBATCH_STOP_WAIT);

finished:
    DBG_PRINTS("closing");
    closelogfile(outputlog);
    threadscleanup();
    DBG_PRINTS("done");
    return service->status.dwServiceSpecificExitCode;
}

static int setsvcarguments(SC_HANDLE svc, int argc, LPCWSTR *argv)
{
    int     e;
    int     n;
    HKEY    k = NULL;
    HKEY    a = NULL;
    LPBYTE  b = NULL;
    LSTATUS s;

    if ((svc == NULL) || (argc == 0))
        return 0;
    e = __LINE__;
    s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, SYSTEM_SVC_SUBKEY,
                      0, KEY_QUERY_VALUE | KEY_READ | KEY_WRITE, &k);
    if (s != ERROR_SUCCESS) {
        SetLastError(s);
        return e + 1;;
    }
    e = __LINE__;
    b = (LPBYTE)xargvtomsz(argc, argv, &n);
    if (b == NULL) {
        s = GetLastError();
        goto finished;
    }
    e = __LINE__;
    s = RegOpenKeyExW(k, service->name,
                      0, KEY_QUERY_VALUE | KEY_READ | KEY_WRITE, &a);
    if (s != ERROR_SUCCESS)
        goto finished;
    e = __LINE__;
    s = RegSetValueExW(a, SVCBATCH_SVCARGS, 0, REG_MULTI_SZ, b, n);
finished:
    xfree(b);
    if (a)
        RegCloseKey(a);
    if (k)
        RegCloseKey(k);
    if (s != ERROR_SUCCESS) {
        SetLastError(s);
        return e + 1;
    }
    else {
        return 0;
    }
}

static int xscmcommand(LPCWSTR ncmd)
{
    int i = 0;
    while (scmcommands[i] != NULL) {
        if (xwcsequals(ncmd, scmcommands[i]))
            return i;
        i++;
    }
    return -1;
}

static int xscmhelp(LPCWSTR cmd)
{
    int r = 0;

    if (IS_EMPTY_STR(cmd)) {
        fputs(xgenerichelp, stdout);
    }
    else {
        int i = xscmcommand(cmd);
        if (i < 0) {
            fprintf(stdout,
                    "\nError:\n  Unknown command: %S\n",
                    cmd);
            r = ERROR_INVALID_NAME;
        }
        else {
            fputs(xcommandhelp[i], stdout);
        }
    }
    return r;
}

static int xscmexecute(int cmd, int argc, LPCWSTR *argv)
{
    PSERVICE_CONTROL_STATUS_REASON_PARAMSW ssr;
    LPSERVICE_STATUS_PROCESS ssp;
    WCHAR     cb[SVCBATCH_PATH_MAX];
    DWORD     bneed;
    int       i;
    int       x;
    int       opt;
    int       rv = 0;
    int       ec = 0;
    int       ep = 0;
    int       en = 0;
    int       cmdverbose  = 1;
    int       wtime       = 0;
    ULONGLONG wtmstart    = 0;
    ULONGLONG wtimeout    = 0;
    LPCWSTR   ed          = NULL;
    LPCWSTR   ex          = NULL;
    LPWSTR    pp          = NULL;
    LPWSTR    sdepends    = NULL;
    LPWSTR    binarypath  = NULL;
    LPCWSTR   description = NULL;
    LPCWSTR   displayname = NULL;
    LPCWSTR   privileges  = NULL;
    LPCWSTR   username    = NULL;
    LPCWSTR   password    = NULL;
    DWORD     starttype   = SERVICE_NO_CHANGE;
    DWORD     servicetype = SERVICE_NO_CHANGE;
    DWORD     srmajor     = SERVICE_STOP_REASON_MAJOR_NONE;
    DWORD     srminor     = SERVICE_STOP_REASON_MINOR_NONE;
    DWORD     srflag      = SERVICE_STOP_REASON_FLAG_PLANNED;
    SC_HANDLE mgr         = NULL;
    SC_HANDLE svc         = NULL;

    if (cmd == SVCBATCH_SCM_VERSION) {
        fputs(cnamestamp, stdout);
        return 0;
    }
    if (cmd == SVCBATCH_SCM_HELP)
        return xscmhelp(argv[0]);
    if (IS_EMPTY_WCS(argv[0])) {
        fprintf(stdout, "\nError:\n  Missing service name");
        xscmhelp(NULL);
        return ERROR_INVALID_PARAMETER;
    }
    ssr = (PSERVICE_CONTROL_STATUS_REASON_PARAMSW)xmcalloc(sizeof(SERVICE_CONTROL_STATUS_REASON_PARAMSW));
    ssp = &ssr->ServiceStatus;
    service->name = argv[0];
    wtmstart      = GetTickCount64();
    DBG_PRINTF("%S %S", scmcommands[cmd], service->name);
    if (xwcspbrk(service->name, L"/\\:;<>?*|\"")) {
        rv = ERROR_INVALID_NAME;
        ec = __LINE__;
        ex = SVCBATCH_MSG(23);
        goto finished;
    }
    if (xwcslen(service->name) > SVCBATCH_NAME_MAX) {
        rv = ERROR_INVALID_NAME;
        ec = __LINE__;
        ex = SVCBATCH_MSG(24);
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_CREATE) {
        starttype   = SERVICE_DEMAND_START;
        servicetype = SERVICE_WIN32_OWN_PROCESS;
    }
    if ((cmd == SVCBATCH_SCM_START) || (cmd == SVCBATCH_SCM_STOP))
        wtime = SVCBATCH_SCM_WAIT_DEF;

    while ((opt = xlongopt(argc, argv, scmcoptions, scmallowed[cmd])) != EOF) {
        switch (opt) {
            case 'b':
                xfree(binarypath);
                pp = xgetfinalpath(0, skipdotslash(xwoptarg));
                if (pp == NULL) {
                    rv = GetLastError();
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
                binarypath = xappendarg(1, NULL, pp);
                xfree(pp);
            break;
            case 'D':
                x = xwcslen(xwoptarg);
                xfree(sdepends);
                sdepends = xwmalloc(x + 2);
                for (i = 0; i < x; i++) {
                    if (xwoptarg[i] == L'/')
                        sdepends[i] = WNUL;
                    else
                        sdepends[i] = xwoptarg[i];
                }
                sdepends[i++] = WNUL;
                sdepends[i]   = WNUL;
            break;
            case 'd':
                description = xwoptarg;
            break;
            case 'n':
                displayname = xwoptarg;
            break;
            case 'p':
                password    = xwoptarg;
            break;
            case 'P':
                privileges  = xwoptarg;
            break;
            case 'q':
                cmdverbose  = 0;
            break;
            case 's':
                starttype   = xnamemap(xwoptarg, starttypemap, SERVICE_NO_CHANGE);
                if (starttype == SERVICE_NO_CHANGE) {
                    rv = ERROR_INVALID_PARAMETER;
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
            break;
            case 'u':
                if ((xwoptarg[0] > 47) && (xwoptarg[0] < 51) && (xwoptarg[1] == WNUL))
                    username = scmsvcaccounts[xwoptarg[0] - 48];
                else
                    username = xwoptarg;
            break;
            case 'w':
                if (xwoptarg) {
                    wtime = xwcstoi(xwoptarg, NULL);
                    if ((wtime < 0) || (wtime > SVCBATCH_STOP_TMAX)) {
                        rv = ERROR_INVALID_PARAMETER;
                        ec = __LINE__;
                        ed = xwoptarg;
                        goto finished;
                    }
                }
                else {
                    /* Use maximum  wait time */
                    wtime = SVCBATCH_STOP_TMAX;
                }
            break;
            case ENOENT:
                rv = ERROR_BAD_LENGTH;
                ec = __LINE__;
                ed = xwoption;
                goto finished;
            break;
            case EINVAL:
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ex = SVCBATCH_MSG(22);
                ed = xwoption;
                goto finished;
            break;
            default:
                rv = ERROR_NOT_SUPPORTED;
                ec = __LINE__;
                ed = xwoption;
                goto finished;
            break;
        }
    }
    argc -= xwoptind;
    argv += xwoptind;
    if (wtime)
        wtimeout = wtime * ONE_SECOND;
    mgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (mgr == NULL) {
        rv = GetLastError();
        ec = __LINE__;
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_CREATE) {
        if (binarypath == NULL)
            binarypath = xappendarg(1, NULL, program->application);
        svc = CreateServiceW(mgr,
                             service->name,
                             displayname,
                             SERVICE_ALL_ACCESS,
                             servicetype,
                             starttype,
                             SERVICE_ERROR_NORMAL,
                             binarypath,
                             NULL,
                             NULL,
                             sdepends,
                             username,
                             password);
        ep = setsvcarguments(svc, argc, argv);
        if (ep) {
            rv = GetLastError();
            ec = __LINE__;
            ed = SVCBATCH_SVCARGS;
            goto finished;
        }
    }
    else {
        svc = OpenServiceW(mgr,
                           service->name,
                           SERVICE_ALL_ACCESS);
    }
    if (svc == NULL) {
        rv = GetLastError();
        ec = __LINE__;
        ed = service->name;
        goto finished;
    }

    if (cmd == SVCBATCH_SCM_DELETE) {
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState != SERVICE_STOPPED) {
            rv = ERROR_SERVICE_ALREADY_RUNNING;
            ec = __LINE__;
            ed = SVCBATCH_MSG(25);
            goto finished;
        }
        if (!DeleteService(svc)) {
            rv = GetLastError();
            ec = __LINE__;
        }
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_START) {
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState != SERVICE_STOPPED) {
            rv = ERROR_SERVICE_ALREADY_RUNNING;
            ec = __LINE__;
            goto finished;
        }
        if (!StartServiceW(svc, argc, argv)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (wtime) {
            if (!QueryServiceStatusEx(svc,
                                      SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                      SZ_STATUS_PROCESS_INFO, &bneed)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
            while (ssp->dwCurrentState == SERVICE_START_PENDING) {
                Sleep(ONE_SECOND);
                if (!QueryServiceStatusEx(svc,
                                          SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                          SZ_STATUS_PROCESS_INFO, &bneed)) {
                    rv = GetLastError();
                    ec = __LINE__;
                    goto finished;
                }
                if (GetTickCount64() - wtmstart > wtimeout) {
                    /** Timeout */
                    rv = ERROR_SERVICE_REQUEST_TIMEOUT;
                    ec = __LINE__;
                    goto finished;
                }
            }
            if (ssp->dwCurrentState != SERVICE_RUNNING) {
                rv = ERROR_SERVICE_START_HANG;
                ec = __LINE__;
            }
        }
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_STOP) {
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState == SERVICE_STOPPED) {
            rv = ERROR_SERVICE_NOT_ACTIVE;
            ec = __LINE__;
            goto finished;
        }
        while (ssp->dwCurrentState == SERVICE_STOP_PENDING) {
            if (wtime == 0)
                goto finished;
            Sleep(ONE_SECOND);
            if (!QueryServiceStatusEx(svc,
                                      SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                      SZ_STATUS_PROCESS_INFO, &bneed)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
            if (ssp->dwCurrentState == SERVICE_STOPPED)
                goto finished;
            if (GetTickCount64() - wtmstart > wtimeout) {
                /** Timeout */
                rv = ERROR_SERVICE_REQUEST_TIMEOUT;
                ec = __LINE__;
                goto finished;
            }
        }
        if (argc == 0) {
            if (!ControlService(svc, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)ssp)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
        }
        else {
            DWORD   sv;
            LPWSTR  sp;
            LPCWSTR rp = argv[0];

            sv = xwcstoi(rp, &sp);
            if ((sv < 1) || (sv > 4)) {
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ed = argv[0];
                goto finished;
            }
            srflag = (sv << 28);
            sv = 0;
            if (*sp) {
                rp = sp + 1;
                sv = xwcstoi(rp, &sp);
            }
            if ((sv < 1) || (sv > 255)) {
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ed = argv[0];
                goto finished;
            }
            srmajor = (sv << 16);
            sv = 0;
            if (*sp) {
                rp = sp + 1;
                sv = xwcstoi(rp, &sp);
            }
            if ((sv < 1) || (sv > 65535)) {
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ed = argv[0];
                goto finished;
            }
            srminor = sv;
            if (argc > 1) {
                /* Comment is limited to 128 chars */
                xwcslcpy(cb, 128, argv[1]);
                ssr->pszComment = cb;
            }
            ssr->dwReason = srflag | srmajor | srminor;
            if (!ControlServiceExW(svc, SERVICE_CONTROL_STOP,
                                   SERVICE_CONTROL_STATUS_REASON_INFO, (LPBYTE)ssr)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
        }
        while (ssp->dwCurrentState != SERVICE_STOPPED) {
            if (wtime == 0)
                break;
            Sleep(ONE_SECOND);
            if (!QueryServiceStatusEx(svc,
                                      SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                      SZ_STATUS_PROCESS_INFO, &bneed)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
            if (ssp->dwCurrentState == SERVICE_STOPPED)
                break;
            if (GetTickCount64() - wtmstart > wtimeout) {
                /** Timeout */
                rv = ERROR_SERVICE_REQUEST_TIMEOUT;
                ec = __LINE__;
                goto finished;
            }
        }
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_CONTROL) {
        int cc = 0;
        if (argc == 0) {
            rv = ERROR_INVALID_PARAMETER;
            ec = __LINE__;
            ex = SVCBATCH_MSG(26);
            goto finished;
        }
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState != SERVICE_RUNNING) {
            rv = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
            ec = __LINE__;
            ed = SVCBATCH_MSG(2);
            goto finished;
        }
        cc = xwcstoi(argv[0], NULL);
        ed = argv[0];

        if ((cc < 128) || (cc > 255)) {
            rv = ERROR_INVALID_PARAMETER;
            ec = __LINE__;
            goto finished;
        }
        if (!ControlServiceExW(svc, cc,
                               SERVICE_CONTROL_STATUS_REASON_INFO, (LPBYTE)ssr)) {
            rv = GetLastError();
            ec = __LINE__;
        }
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_CONFIG) {
        if (!ChangeServiceConfigW(svc,
                                  servicetype,
                                  starttype,
                                  SERVICE_NO_CHANGE,
                                  binarypath,
                                  NULL,
                                  NULL,
                                  sdepends,
                                  username,
                                  password,
                                  displayname)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        ep = setsvcarguments(svc, argc, argv);
        if (ep) {
            rv = GetLastError();
            ec = __LINE__;
            ed = SVCBATCH_SVCARGS;
            goto finished;
        }
    }
    if (description) {
        SERVICE_DESCRIPTIONW sc;
        sc.lpDescription = (LPWSTR)description;
        if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sc)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
    }
    if (privileges) {
        LPWSTR reqprivs;
        SERVICE_REQUIRED_PRIVILEGES_INFOW sc;

        x = xwcslen(privileges);
        reqprivs = xwmalloc(x + 2);
        for (i = 0; i < x; i++) {
            if (privileges[i] == L'/')
                reqprivs[i] = WNUL;
            else
                reqprivs[i] = privileges[i];
        }
        reqprivs[i++] = WNUL;
        reqprivs[i]   = WNUL;

        sc.pmszRequiredPrivileges = reqprivs;
        if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &sc)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
    }

finished:
    if (svc != NULL)
        CloseServiceHandle(svc);
    if (mgr != NULL)
        CloseServiceHandle(mgr);
    if (cmdverbose) {

        if (rv) {
            wchar_t eb[SVCBATCH_LINE_MAX];
            if (ex == NULL) {
            xwinapierror(eb, SVCBATCH_LINE_MAX, rv);
            ex = eb;
            }
            fprintf(stdout, "Service Name : %S\n", service->name);
            fprintf(stdout, "     Command : %S\n", scmcommands[cmd]);
            fprintf(stdout, "             : FAILED\n");
            if ((wtime > 0) && (rv == ERROR_SERVICE_REQUEST_TIMEOUT))
            fprintf(stdout, "               %llu ms\n", GetTickCount64() - wtmstart);
            if (ec) {
            if (ep)
            fprintf(stdout, "        LINE : %d (%d)\n", ec, ep);
            else
            fprintf(stdout, "        LINE : %d\n", ec);
            }
            fprintf(stdout, "       ERROR : %d (0x%x)\n", rv,  rv);
            fprintf(stdout, "               %S\n", ex);
            if (ed != NULL) {
            fprintf(stdout, "               %S", ed);
            if (en)
            fprintf(stdout, " (%d)", en);
            fputc('\n', stdout);
            }
            fputc('\n', stdout);
        }
        else {
            fprintf(stdout, "Service Name : %S\n", service->name);
            fprintf(stdout, "     Command : %S\n", scmcommands[cmd]);
            fprintf(stdout, "             : SUCCESS\n");
            if (wtime)
            fprintf(stdout, "               %llu ms\n", GetTickCount64() - wtmstart);
            if (cmd == SVCBATCH_SCM_CONTROL)
            fprintf(stdout, "               %S\n", argv[0]);
            if (cmd == SVCBATCH_SCM_CREATE)
            fprintf(stdout, "     STARTUP : %S (%lu)\n", xcodemap(starttypemap, starttype), starttype);
            if (cmd == SVCBATCH_SCM_START && wtime)
            fprintf(stdout, "         PID : %lu\n",  ssp->dwProcessId);
            if (cmd == SVCBATCH_SCM_STOP  && wtime)
            fprintf(stdout, "    EXITCODE : %lu (0x%lx)\n", ssp->dwServiceSpecificExitCode, ssp->dwServiceSpecificExitCode);
            fputc('\n', stdout);
        }
    }
    DBG_PRINTS("done");
    return rv;
}

#if defined(_DEBUG)
static void __cdecl dbgcleanup(void)
{
    HANDLE h;

    DBG_PRINTS("done");
    h = InterlockedExchangePointer(&dbgfile, NULL);
    if (IS_VALID_HANDLE(h)) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    DeleteCriticalSection(&dbglock);
}

static DWORD dbgfopen(void)
{
    HANDLE h;
    LPWSTR n;
    DWORD  rc;

    if (IS_EMPTY_WCS(service->temp))
        return ERROR_PATH_NOT_FOUND;
    n = xwmakepath(service->temp, program->name, DBG_FILE_NAME);
    h = CreateFileW(n, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    xfree(n);
    if (h != INVALID_HANDLE_VALUE) {
        InterlockedExchangePointer(&dbgfile, h);
        if (rc == 0)
            dbgprints(__FUNCTION__, __LINE__, cnamestamp);
        if (rc == ERROR_ALREADY_EXISTS) {
            rc = 0;
#if (_DEBUG > 1)
            if (dbgsvcmode == 1) {
                LARGE_INTEGER ee = {{ 0, 0 }};
                if (SetFilePointerEx(dbgfile, ee, NULL, FILE_BEGIN)) {
                    if (SetEndOfFile(dbgfile)) {
                        dbgprints(__FUNCTION__, __LINE__, cnamestamp);
                        return 0;
                    }
                }
                rc = GetLastError();
            }
#endif
        }
    }
    return rc;
}

#endif

static int xwmaininit(int argc, LPCWSTR *argv)
{
    WCHAR  bb[SVCBATCH_PATH_MAX];
    LPWSTR dp = NULL;
    DWORD  nn;

#if defined (_DEBUG)
    InitializeCriticalSection(&dbglock);
    atexit(dbgcleanup);
#endif
    xmemzero(threads, SVCBATCH_MAX_THREADS, sizeof(SVCBATCH_THREAD));
    service = (LPSVCBATCH_SERVICE)xmcalloc( sizeof(SVCBATCH_SERVICE));
    program = (LPSVCBATCH_PROCESS)xmcalloc( sizeof(SVCBATCH_PROCESS));
    cmdproc = (LPSVCBATCH_PROCESS)xmcalloc( sizeof(SVCBATCH_PROCESS));

    program->state             = SVCBATCH_PROCESS_RUNNING;
    program->pInfo.hProcess    = GetCurrentProcess();
    program->pInfo.dwProcessId = GetCurrentProcessId();
    program->pInfo.dwThreadId  = GetCurrentThreadId();
    program->sInfo.cb          = DSIZEOF(STARTUPINFOW);
    GetStartupInfoW(&program->sInfo);

    nn = GetModuleFileNameW(NULL, bb, SVCBATCH_PATH_MAX - 4);
    if (nn == 0)
        return GetLastError();
    if (nn >= (SVCBATCH_PATH_MAX - 4))
        return ERROR_INSUFFICIENT_BUFFER;
    nn = xfixmaxpath(bb, nn);
    program->application = xwcsdup(bb);
    while (--nn > 4) {
        if ((dp == NULL) && (bb[nn] == L'.')) {
             dp = bb + nn;
            *dp = WNUL;
            continue;
        }
        if (bb[nn] == L'\\') {
            bb[nn++]           = WNUL;
            program->directory = xwcsdup(bb);
            program->name      = xwcsdup(bb + nn);
            break;
        }
    }
    ASSERT_WSTR(program->application, ERROR_BAD_PATHNAME);
    ASSERT_WSTR(program->directory,   ERROR_BAD_PATHNAME);
    ASSERT_WSTR(program->name,        ERROR_BAD_PATHNAME);

    xwcslower(program->name);
    return 0;
}

static LPWSTR gettempdir(void)
{
    WCHAR  bb[BBUFSIZ];
    DWORD  nn;

    nn = GetTempPathW(BBUFSIZ, bb);
    if (nn == 0)
        return NULL;
    if (nn > MAX_PATH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return NULL;
    }
    bb[--nn] = WNUL;
    return xgetfinalpath(2, bb);
}

/**
 * Main program entry
 *
 * 1. If invoked from SCM
 *    argc and argv are constructed
 *    by SCM, using CommandLineToArgvW Win32 API
 *    where the lpCmdLine is value of the registry key
 *    HKLM\SYSTEM\CurrentControlSet\Services\service\ImagePath
 *
 * 2. Invoked as Console application
 *    argc and argv are standard program
 *    arguments
 *
 * 3. Invoked as Stop process by service
 *    when it SCM sends a STOP control signal
 *    to the service, and a service was configured
 *    with stop process feature
 *
 */
int wmain(int argc, LPCWSTR *argv)
{
    DWORD   x;
    int     r = 0;
    LPCWSTR p = zerostring;
    HANDLE  h;
    SERVICE_TABLE_ENTRYW se[2];

#if defined(_DEBUG)
# if defined(_MSC_VER) && (_MSC_VER < 1900)
    /* Not supported */
# else
    _set_invalid_parameter_handler(xiphandler);
# endif
   _CrtSetReportMode(_CRT_ASSERT, 0);
#endif
    /**
     * Make sure child processes are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    r = xwmaininit(argc, argv);
    if (r != 0)
        return r;
    if (argc > 1)
        p = argv[1];
    if (xisalpha(*p)) {
        /**
         * Check if this is a Service Manager command
         */
        int cmd = xscmcommand(p);
        if (cmd >= 0) {
            argc  -= 2;
            argv  += 2;
#if defined(_DEBUG)
            dbgsvcmode = 3;
            DBG_PRINTS("started");
#endif
            r = xscmexecute(cmd, argc, argv);
            goto finished;
        }
    }
    /**
     * Check if running as child stop process.
     */
    if (xwstartswith(p, SVCBATCH_MMAPPFX)) {
        LPWSTR dp;
        p += 2;
        cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT;
#if defined(_DEBUG)
        dbgsvcmode = 2;
#endif
        sharedmmap  = OpenFileMappingW(FILE_MAP_READ, FALSE, p);
        if (sharedmmap == NULL) {
            r = xsyserror(GetLastError(), L"OpenFileMapping", p);
            goto finished;
        }
        sharedmem = (LPSVCBATCH_IPC)MapViewOfFile(
                                        sharedmmap,
                                        FILE_MAP_READ,
                                        0, 0, DSIZEOF(SVCBATCH_IPC));
        if (sharedmem == NULL) {
            r = xsyserror(GetLastError(), L"MapViewOfFile", p);
            CloseHandle(sharedmmap);
            goto finished;
        }
        dp = sharedmem->data;
        cmdproc->ppid = sharedmem->ppid;
        svcoptions    = sharedmem->options;
        stoptimeout   = sharedmem->timeout;
        killdepth     = sharedmem->killdepth;
        srvcmaxlogs   = sharedmem->maxlogs;

        cmdproc->application = dp + sharedmem->application;
        cmdproc->script      = dp + sharedmem->script;
        service->name = dp + sharedmem->name;
        service->temp = dp + sharedmem->temp;
        service->work = dp + sharedmem->work;
        service->logs = dp + sharedmem->logs;
        if (sharedmem->logn && IS_NOT(SVCBATCH_OPT_QUIET)) {
            outputlog = (LPSVCBATCH_LOG)xmcalloc(sizeof(SVCBATCH_LOG));
            outputlog->logName = dp + sharedmem->logn;
            SVCBATCH_CS_INIT(outputlog);
        }
        else {
            OPT_SET(SVCBATCH_OPT_QUIET);
        }

        cmdproc->argc = sharedmem->argc;
        cmdproc->optc = sharedmem->optc;
        for (x = 0; x < cmdproc->argc; x++)
            cmdproc->args[x] = dp + sharedmem->args[x];
        for (x = 0; x < cmdproc->optc; x++)
            cmdproc->opts[x] = dp + sharedmem->opts[x];
#if defined(_DEBUG)
        dbgfopen();
        DBG_PRINTS(cnamestamp);
#endif
        r = svcstopmain();
        goto finished;
    }
    servicemode   = 1;
    service->temp = gettempdir();
#if defined(_DEBUG)
    dbgsvcmode  = 1;
    dbgfopen();
#endif
    /**
     * Presume we are started from SCM
     *
     * Create invisible console and
     * start to run as service
     */
    h = GetStdHandle(STD_INPUT_HANDLE);
    if (IS_INVALID_HANDLE(h)) {
        if (AllocConsole()) {
            /**
             * AllocConsole should create new set of
             * standard i/o handles
             */
            atexit(consolecleanup);
            h = GetStdHandle(STD_INPUT_HANDLE);
            ASSERT_HANDLE(h, ERROR_DEV_NOT_EXIST);
        }
        else {
            r = GetLastError();
            goto finished;
        }
    }
    program->sInfo.hStdInput  = h;
    program->sInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    program->sInfo.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consolehandler, TRUE);

    svcmainargc = argc - 1;
    svcmainargv = argv + 1;
    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = NULL;
    se[1].lpServiceProc = NULL;
    if (!StartServiceCtrlDispatcherW(se))
        r = GetLastError();

finished:
#if defined(_DEBUG)
    DBG_PRINTS("done");
#endif
    return r;
}
