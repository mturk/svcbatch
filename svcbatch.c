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
#if defined(_DEBUG)
#include <crtdbg.h>
#endif
#include "svcbatch.h"

#if defined(_DEBUG)
static void dbgprintf(LPCSTR, LPCSTR, ...);
static void dbgprints(LPCSTR, LPCSTR);
static char dbgsvcmode = 'x';

# define DBG_PRINTF(Fmt, ...)   dbgprintf(__FUNCTION__, Fmt, ##__VA_ARGS__)
# define DBG_PRINTS(Msg)        dbgprints(__FUNCTION__, Msg)
#else
# define DBG_PRINTF(Fmt, ...)   (void)0
# define DBG_PRINTS(Msg)        (void)0
#endif

#define xsyserror(_n, _e, _d)   svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_ERROR_TYPE,      _n, _e, _d)
#define xsyswarn(_n, _e, _d)    svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_WARNING_TYPE,    _n, _e, _d)
#define xsysinfo(_e, _d)        svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_INFORMATION_TYPE, 0, _e, _d)

#define SZ_STATUS_PROCESS_INFO  sizeof(SERVICE_STATUS_PROCESS)
#define SYSTEM_SVC_SUBKEY       L"SYSTEM\\CurrentControlSet\\Services"

typedef enum {
    SVCBATCH_WORKER_THREAD = 0,
    SVCBATCH_WRPIPE_THREAD,
    SVCBATCH_STOP_THREAD,
#if SVCBATCH_LEAN_AND_MEAN
    SVCBATCH_ROTATE_THREAD,
#endif
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
    DWORD               exitCode;
    DWORD               argc;
    DWORD               optc;
    LPWSTR              commandLine;
    LPWSTR              application;
    LPWSTR              script;
    LPWSTR              directory;
    LPWSTR              name;
    LPWSTR              args[SVCBATCH_MAX_ARGS];
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
    LPWSTR                  uuid;
    LPWSTR                  work;
#if SVCBATCH_LEAN_AND_MEAN
    WCHAR                   logs[SVCBATCH_PATH_MAX];
#else
    LPWSTR                  logs;
#endif

} SVCBATCH_SERVICE, *LPSVCBATCH_SERVICE;

#if SVCBATCH_LEAN_AND_MEAN
typedef struct _SVCBATCH_LOG {
    volatile LONG64     size;
    volatile HANDLE     fd;
    volatile LONG       state;
    volatile LONG       count;
    int                 maxLogs;
    CRITICAL_SECTION    cs;
    LPCWSTR             fileExt;
    LPCWSTR             logName;
    WCHAR               logFile[SVCBATCH_PATH_MAX];

} SVCBATCH_LOG, *LPSVCBATCH_LOG;

typedef struct _SVCBATCH_IPC {
    DWORD   processId;
    DWORD   options;
    DWORD   timeout;
    DWORD   argc;
    DWORD   optc;
    DWORD   killdepth;
    WCHAR   uuid[SVCBATCH_UUID_MAX];
    WCHAR   name[SVCBATCH_NAME_MAX];
    WCHAR   logName[SVCBATCH_NAME_MAX];
    WCHAR   home[SVCBATCH_PATH_MAX];
    WCHAR   work[SVCBATCH_PATH_MAX];
    WCHAR   logs[SVCBATCH_PATH_MAX];
    WCHAR   application[SVCBATCH_PATH_MAX];
    WCHAR   script[SVCBATCH_PATH_MAX];
    WCHAR   args[SVCBATCH_MAX_ARGS][SVCBATCH_NAME_MAX];
    WCHAR   opts[SVCBATCH_MAX_ARGS][SVCBATCH_NAME_MAX];

} SVCBATCH_IPC, *LPSVCBATCH_IPC;
#endif

typedef struct _SVCBATCH_NAME_MAP {
    LPCWSTR name;
    DWORD   code;
} SVCBATCH_NAME_MAP, *LPSVCBATCH_NAME_MAP;

static int                   svcmainargc = 0;
static LPCWSTR              *svcmainargv = NULL;
static LPSVCBATCH_PROCESS    program     = NULL;
static LPSVCBATCH_SERVICE    service     = NULL;

static LPSVCBATCH_PROCESS    svcstop     = NULL;
static LPSVCBATCH_PROCESS    cmdproc     = NULL;
#if SVCBATCH_LEAN_AND_MEAN
static LPSVCBATCH_LOG        outputlog   = NULL;
static LPSVCBATCH_LOG        statuslog   = NULL;
static LPSVCBATCH_IPC        sharedmem   = NULL;
#endif

static volatile LONG         killdepth   = 4;

#if SVCBATCH_LEAN_AND_MEAN
static LONGLONG              counterbase    = CPP_INT64_C(0);
static LONGLONG              counterfreq    = CPP_INT64_C(0);
static LONGLONG              rotateinterval = CPP_INT64_C(0);
static LONGLONG              rotatesize     = CPP_INT64_C(0);
static LARGE_INTEGER         rotatetime     = {{ 0, 0 }};

static BOOL      rotatebysize   = FALSE;
static BOOL      rotatebytime   = FALSE;
static BOOL      rotatebysignal = FALSE;
#endif
static BOOL      servicemode    = TRUE;

static DWORD     svcoptions     = 0;
static DWORD     preshutdown    = 0;
static int       stoptimeout    = SVCBATCH_STOP_TIMEOUT;
static int       svcfailmode    = SVCBATCH_FAIL_EXIT;
static HANDLE    stopstarted    = NULL;
static HANDLE    svcstopdone    = NULL;
static HANDLE    workerended    = NULL;
#if SVCBATCH_LEAN_AND_MEAN
static HANDLE    dologrotate    = NULL;
static HANDLE    sharedmmap     = NULL;
static LPCWSTR   outdirparam    = NULL;
#endif
static SVCBATCH_THREAD threads[SVCBATCH_MAX_THREADS];

static LPWSTR       svclogfname   = NULL;
static WCHAR        zerostring[]  = {  0,  0,  0,  0 };
static WCHAR        CRLFW[]       = { 13, 10,  0,  0 };
static CHAR         CRLFA[]       = { 13, 10,  0,  0 };
static BYTE         YYES[]        = { 89, 13, 10,  0 };

static LPCSTR  cnamestamp  = SVCBATCH_RES_NAME " " SVCBATCH_VERSION_TXT;
static LPCWSTR wnamestamp  = CPP_WIDEN(SVCBATCH_RES_NAME) L" " SVCBATCH_VERSION_WCS;
static LPCWSTR cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);

static int     xwoptind    = 1;
static LPCWSTR xwoptarg    = NULL;
static LPCWSTR xwoption    = NULL;

#if SVCBATCH_LEAN_AND_MEAN
static LPCWSTR cmdoptions  = L"bc:e:f:gh:k:lm:n:o:pqr:s:tvw:";
#else
static LPCWSTR cmdoptions  = L"c:e:f:h:k:pw:";
#endif

#if SVCBATCH_HAVE_SCM
/**
 * Service Manager types
 *
 */
typedef enum {
    SVCBATCH_SCM_CREATE = 0,
    SVCBATCH_SCM_CONFIG,
    SVCBATCH_SCM_CONTROL,
    SVCBATCH_SCM_DELETE,
    SVCBATCH_SCM_START,
    SVCBATCH_SCM_STOP
} SVCBATCH_SCM_CMD;

static const wchar_t *scmcommands[] = {
    L"Create",                  /* SVCBATCH_SCM_CREATE      */
    L"Config",                  /* SVCBATCH_SCM_CONFIG      */
    L"Control",                 /* SVCBATCH_SCM_CONTROL     */
    L"Delete",                  /* SVCBATCH_SCM_DELETE      */
    L"Start",                   /* SVCBATCH_SCM_START       */
    L"Stop",                    /* SVCBATCH_SCM_STOP        */
    NULL
};

static const SVCBATCH_NAME_MAP starttypemap[] = {
    { L"automatic", SERVICE_AUTO_START      },
    { L"auto",      SERVICE_AUTO_START      },
    { L"demand",    SERVICE_DEMAND_START    },
    { L"disabled",  SERVICE_DISABLED        },
    { L"manual",    SERVICE_DEMAND_START    },
    { NULL,         0                       }
};

static const wchar_t *scmallowed[] = {
    L"vbdDinpPsu",         /* SVCBATCH_SCM_CREATE      */
    L"vbdDinpPsu",         /* SVCBATCH_SCM_CONFIG      */
    L"v",                  /* SVCBATCH_SCM_CONTROL     */
    L"vw",                 /* SVCBATCH_SCM_DELETE      */
    L"vw",                 /* SVCBATCH_SCM_START       */
    L"vw",                 /* SVCBATCH_SCM_STOP        */
    NULL
};

/**
 * Long options ...
 *
 * <option><options><option name>
 *
 * option:          Any alphanumeric character
 * options:         '.' Option without argument
 *                  '+' Argument is next option
 *                  ':' Argument can be part of the option separated by ':' or '='
 *                      If the option does not end with ':' or '=', the argument is next option
 *                  '?' Argument is optional and it must be part of the
 *                      current option, separated by ':' or '='
 *
 */

static const wchar_t *scmcoptions[] = {
    L"b+binpath",
    L"b+bin",
    L"d+description",
    L"d+desc",
    L"D:depend",
    L"n+displayname",
    L"n+display",
    L"n+name",
    L"p+password",
    L"P:privileges",
    L"P:privs",
    L"i.interactive",
    L"i.interact",
    L"u+obj",
    L"u+username",
    L"u+user",
    L"s:start",
    L"v?verbose",
    L"w?wait",
    NULL
};

#endif

/**
 * (element & 1) == valid file name character
 * (element & 2) == character should be escaped in command line
 */
static const char xfnchartype[128] =
{
    /** Reject all ctrl codes...                                          */
        0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /**   ! " # $ % & ' ( ) * + , - . /  0 1 2 3 4 5 6 7 8 9 : ; < = > ?  */
        3,1,2,1,1,1,1,1,1,1,0,1,1,1,1,0, 1,1,1,1,1,1,1,1,1,1,0,0,0,1,0,0,
    /** @ A B C D E F G H I J K L M N O  P Q R S T U V W X Y Z [ \ ] ^ _  */
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,
    /** ` a b c d e f g h i j k l m n o  p q r s t u v w x y z { | } ~    */
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1
};

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

static __inline void xfixpathsep(LPWSTR str)
{
    for (; *str != 0; str++) {
        if (*str == L'/')
            *str = L'\\';
    }
}

static __inline int xtolower(int ch)
{
    if ((ch > 64) && (ch < 91))
        return ch + 32;
    else
        return ch;
}

static __inline int xisblank(int ch)
{
    if ((ch == 32) || (ch == 9))
        return 1;
    else
        return 0;
}

static __inline int xisoptswitch(int c)
{
    return ((c == 45) || (c == 47));
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
    int    s[SBUFSIZ];
    LPWSTR ep;
    LPWSTR bp;

    ASSERT_ZERO(argc, NULL);
    ASSERT_LESS(argc, SBUFSIZ, NULL);

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

static void xwchreplace(LPWSTR s)
{
    LPWSTR d;

    for (d = s; *s; s++, d++) {
        if (*s == SVCBATCH_REPLACE_CHAR) {
            if (*(s + 1) == SVCBATCH_REPLACE_CHAR)
                *d = *(s++);
            else
                *d = L'%';
        }
        else {
            *d = *s;
        }
    }
    *d = WNUL;
}

static void xwcsreplace(LPWSTR s)
{
     while (*s) {
        if (*s == SVCBATCH_REPLACE_CHAR)
            *s = L'%';
        s++;
    }
}

/**
 * Simple atoi with range between 0 and INT_MAX.
 * Leading white space characters are ignored.
 * Returns negative value on error.
 */
static int xwcstoi(LPCWSTR sp, LPWSTR *ep)
{
    INT64 rv = CPP_INT64_C(0);
    int   dc = 0;

    ASSERT_WSTR(sp, -1);
    while(xisblank(*sp))
        sp++;

    while(iswdigit(*sp)) {
        int dv = *sp - L'0';

        if (dv || rv) {
            rv *= 10;
            rv += dv;
        }
        if (rv > INT_MAX) {
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
        *ep = (LPWSTR )sp;
    return (int)rv;
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
    if ((n < 0) || (n > c)) {
        dst[c] = WNUL;
        return siz;
    }
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
    if ((n < 0) || (n > c)) {
        dst[c] = '\0';
        return siz;
    }
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

static LPWSTR xexpandenvstr(LPCWSTR str)
{
    LPWSTR  buf = NULL;
    DWORD   bsz = BBUFSIZ;
    DWORD   len = 0;

    if (IS_EMPTY_WCS(str))
        return NULL;
    if (xwcschr(str, L'%') == NULL)
        buf = xwcsdup(str);
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
    return buf;
}

static DWORD xsetenv(LPCWSTR s)
{
    DWORD  r = 0;
    LPWSTR n;
    LPWSTR v;
    LPWSTR e = NULL;

    ASSERT_NULL(s, ERROR_BAD_ENVIRONMENT);
    n = xwcsdup(s);
    v = xwcschr(n + 1, L'=');
    if (v == NULL) {
        r = ERROR_INVALID_PARAMETER;
        goto finished;
    }
    *v++ = WNUL;
    if (*v == WNUL) {
        r = ERROR_INVALID_DATA;
        goto finished;
    }
    e = xexpandenvstr(v);
    if (e == NULL) {
        r = GetLastError();
        goto finished;
    }
    if (!SetEnvironmentVariableW(n, e))
        r = GetLastError();
finished:
    xfree(n);
    xfree(e);
    return r;
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

    if (nq) {
        nq = 0;
        if (xwcspbrk(s2, L" \t\"")) {
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
    e  = (LPWSTR )xrealloc(s1, nn * sizeof(WCHAR));
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

    while ((sa = xtolower(*str++)) == xtolower(*src++)) {
        if (sa == 0)
            return 1;
    }
    return 0;
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

static int xwgetopt(int nargc, LPCWSTR *nargv, LPCWSTR opts)
{
    static LPCWSTR place = zerostring;
    LPCWSTR oli = NULL;
    int option;

    xwoptarg = NULL;
    if (*place == WNUL) {
        if (xwoptind >= nargc) {
            /* No more arguments */
            place = zerostring;
            return EOF;
        }
        place  = nargv[xwoptind];
        option = *(place++);
        if (!xisoptswitch(option)) {
            place = zerostring;
            /**
             * Argument is not an option
             * Use it as in-place argument
             */
            xwoptarg = nargv[xwoptind++];
            return ':';
        }
        if ((*place == WNUL) || (*place == option)) {
            /* We have '--' or '//' */
            place = zerostring;
            if (option == '-')
                xwoptind++;
            return EOF;
        }
        xwoption = place;
    }
    option = *(place++);
    if (option != ':') {
        /* Options are case insensitive */
        oli = xwcschr(opts, xtolower(option));
    }
    if (oli == NULL) {
        xwoptind++;
        place = zerostring;
        return EINVAL;
    }

    /* Does this option need an argument? */
    if (oli[1] == L':') {
        /**
         * Option-argument is either the rest of this argument
         * or the entire next argument.
         */
        if (*place) {
            /* Skip blanks */
            while (xisblank(*place))
                ++place;
        }
        if (*place) {
            if (*place == L':')  {
                ++place;
                while (xisblank(*place))
                    ++place;
                if (*place == WNUL) {
                    /* Missing explicit in-place argument */
                    place = zerostring;
                    return ENOENT;
                }
            }
            xwoptarg = place;
        }
        else if (nargc > ++xwoptind) {
            xwoptarg = nargv[xwoptind];
            while (xisblank(*xwoptarg))
                ++xwoptarg;
        }
        xwoptind++;
        place = zerostring;
        if (IS_EMPTY_WCS(xwoptarg)) {
            /* Option-argument is absent or empty */
            return ENOENT;
        }
    }
    else {
        /* Don't need argument */
        if (*place == WNUL) {
            xwoptind++;
            place = zerostring;
        }
    }
    return oli[0];
}

static int xlongopt(int nargc, LPCWSTR *nargv, LPCWSTR opts, LPCWSTR *longopts)
{
    LPCWSTR *longopt;
    LPCWSTR  place;

    xwoptarg = NULL;
    if (xwoptind >= nargc) {
        /* No more arguments */
        return EOF;
    }
    place  = nargv[xwoptind];
    if (*(place++) != '/')
        return EOF;
    if ((*place == WNUL) || (*place == '/'))
        return EOF;
    xwoption = place;
    longopt  = longopts;

    while (*longopt) {
        int option;
        int optmod;
        int optsep = 0;
        LPCWSTR optsrc;
        LPCWSTR optopt = NULL;

        optsrc = *longopt;
        optmod = optsrc[1];
        if ((optmod == '.') || (optmod == '+')) {
            if (xwcsequals(xwoption, optsrc + 2))
                optopt = zerostring;
        }
        else {
            int endpos = xwstartswith(xwoption, optsrc + 2);
            if (endpos) {
                optopt = xwoption + endpos;
                if ((*optopt == ':') || (*optopt == '='))
                    optsep = *optopt++;
            }
        }
        if (optopt == NULL) {
            longopt++;
            continue;
        }
        /* Found long option */
        option = *optsrc;
        if (xwcschr(opts, option) == NULL)
            return EINVAL;
        if (optmod == '.') {
            /* No arguments needed */
            xwoptind++;
            return option;
        }
        /* Skip blanks */
        while (xisblank(*optopt))
            optopt++;
        if (*optopt) {
            if ((optmod == ':') && !optsep) {
                /* Data without separator */
                return ENOENT;
            }
            else {
                /* Argument is part of the option */
                xwoptarg = optopt;
                xwoptind++;
                return option;
            }
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
    /* Long option not found */
    return EOF;
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

#if SVCBATCH_LEAN_AND_MEAN
static LPWSTR xmktimedext(void)
{
    static WCHAR d[TBUFSIZ];
    SYSTEMTIME tm;
    int i = 0;

    if (IS_SET(SVCBATCH_OPT_LOCALTIME))
        GetLocalTime(&tm);
    else
        GetSystemTime(&tm);
    d[i++] = L'.';
    d[i++] = tm.wYear   / 1000 + L'0';
    d[i++] = tm.wYear   % 1000 / 100 + L'0';
    d[i++] = tm.wYear   % 100  / 10  + L'0';
    d[i++] = tm.wYear   % 10 + L'0';
    d[i++] = tm.wMonth  / 10 + L'0';
    d[i++] = tm.wMonth  % 10 + L'0';
    d[i++] = tm.wDay    / 10 + L'0';
    d[i++] = tm.wDay    % 10 + L'0';
    d[i++] = tm.wHour   / 10 + L'0';
    d[i++] = tm.wHour   % 10 + L'0';
    d[i++] = tm.wMinute / 10 + L'0';
    d[i++] = tm.wMinute % 10 + L'0';
    d[i++] = tm.wSecond / 10 + L'0';
    d[i++] = tm.wSecond % 10 + L'0';
    d[i++] = CNUL;

    return d;
}

static int xtimehdr(char *d, int sz)
{
    LARGE_INTEGER ct = {{ 0, 0 }};
    LARGE_INTEGER et = {{ 0, 0 }};
    DWORD ss, us, mm, hh;
    int   c, i = 0;

    QueryPerformanceCounter(&ct);
    et.QuadPart = ct.QuadPart - counterbase;

    /**
     * Convert to microseconds
     */
    et.QuadPart *= CPP_INT64_C(1000000);
    et.QuadPart /= counterfreq;
    ct.QuadPart  = et.QuadPart / CPP_INT64_C(1000);

    us = (DWORD)((et.QuadPart % CPP_INT64_C(1000000)));
    ss = (DWORD)((ct.QuadPart / MS_IN_SECOND) % CPP_INT64_C(60));
    mm = (DWORD)((ct.QuadPart / MS_IN_MINUTE) % CPP_INT64_C(60));
    hh = (DWORD)((ct.QuadPart / MS_IN_HOUR)   % CPP_INT64_C(24));

    d[i++] = hh / 10 + '0';
    d[i++] = hh % 10 + '0';
    d[i++] = ':';
    d[i++] = mm / 10 + '0';
    d[i++] = mm % 10 + '0';
    d[i++] = ':';
    d[i++] = ss / 10 + '0';
    d[i++] = ss % 10 + '0';
    d[i++] = '.';
    for (c = 0; c < 6; c++)
        d[i++] = '0';
    c = i;
    do {
        d[--c] = us % 10 + '0';
        us /= 10;
    } while (us);
    d[i] = CNUL;

    return i;
}
#endif

#if defined(_DEBUG)
/**
 * Runtime debugging functions
 */

static void dbgprintf(LPCSTR funcname, LPCSTR format, ...)
{
    int     n;
    char    b[SVCBATCH_LINE_MAX];
    va_list ap;

    n = xsnprintf(b, SVCBATCH_LINE_MAX, "[%.4lu] %c %-16s ",
                  GetCurrentThreadId(),
                  dbgsvcmode, funcname);

    va_start(ap, format);
    xvsnprintf(b + n, SVCBATCH_LINE_MAX - n, format, ap);
    va_end(ap);
    OutputDebugStringA(b);
}

static void dbgprints(LPCSTR funcname, LPCSTR string)
{
    if (string == NULL)
        OutputDebugStringA("\n");
    else
        dbgprintf(funcname, "%s", string);
}


static void xiphandler(LPCWSTR e,
                       LPCWSTR w, LPCWSTR f,
                       unsigned int n, uintptr_t r)
{
    dbgprints(__FUNCTION__,
              "invalid parameter handler called");
}

#endif

static BOOL xwinapierror(LPWSTR buf, int siz, DWORD err)
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
        do {
            buf[n--] = WNUL;
        } while ((n > 0) && ((buf[n] == 46) || (buf[n] < 33)));

        while (n-- > 0) {
            if (buf[n] < 32)
                buf[n] = 32;
        }
        return TRUE;
    }
    else {
        xsnwprintf(buf, siz,
                   L"Unrecognized system error code: %u", err);
        return FALSE;
    }
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

static DWORD svcsyserror(LPCSTR fn, int line, WORD typ, DWORD ern, LPCWSTR err, LPCWSTR eds)
{
    WCHAR   hdr[SVCBATCH_LINE_MAX];
    WCHAR   erb[SVCBATCH_LINE_MAX];
    LPCWSTR errarg[10];
    int     c, i = 0;

    errarg[i++] = wnamestamp;
    if (service->name)
        errarg[i++] = service->name;
    xwcslcpy(hdr, SVCBATCH_LINE_MAX, CRLFW);
    if (typ != EVENTLOG_INFORMATION_TYPE) {
        if (typ == EVENTLOG_ERROR_TYPE)
            errarg[i++] = L"\r\nreported the following error:";
        xsnwprintf(hdr + 2, SVCBATCH_LINE_MAX - 2,
                   L"svcbatch.c(%.4d, %S) ", line, fn);
    }
    xwcslcat(hdr, SVCBATCH_LINE_MAX, err);
    if (eds) {
        if (err)
            xwcslcat(hdr, SVCBATCH_LINE_MAX, L": ");
        xwcslcat(hdr, SVCBATCH_LINE_MAX, eds);
    }
    errarg[i++] = hdr;
    if (ern == 0) {
        ern = ERROR_INVALID_PARAMETER;
#if defined(_DEBUG)
        dbgprintf(fn, "%S", hdr + 2);
#endif
    }
    else {
        c = xsnwprintf(erb,   SVCBATCH_LINE_MAX, L"\r\nerror(%lu) ", ern);
        xwinapierror(erb + c, SVCBATCH_LINE_MAX - c, ern);
        errarg[i++] = erb;
#if defined(_DEBUG)
        dbgprintf(fn, "%S, %S", hdr + 2, erb + 2);
#endif
    }

    errarg[i++] = CRLFW;
    while (i < 10) {
        errarg[i++] = NULL;
    }
    if (setupeventlog()) {
        HANDLE es = RegisterEventSourceW(NULL, CPP_WIDEN(SVCBATCH_NAME));
        if (IS_VALID_HANDLE(es)) {
            /**
             * Generic message: '%1 %2 %3 %4 %5 %6 %7 %8 %9'
             * The event code in netmsg.dll is 3299
             */
            ReportEventW(es, typ, 0, 3299, NULL, 9, 0, errarg, NULL);
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
    DBG_PRINTF("[%d] ended %lu", p->id, p->exitCode);
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

static BOOL isabsolutepath(LPCWSTR p)
{
    if ((p != NULL) && (p[0] < 128)) {
        if ((p[0] == L'\\') || (isalpha(p[0]) && (p[1] == L':')))
            return TRUE;
    }
    return FALSE;
}

static BOOL isrelativepath(LPCWSTR p)
{
    return !isabsolutepath(p);
}

static DWORD fixshortpath(LPWSTR buf, DWORD len)
{
    if ((len > 5) && (len < SVCBATCH_NAME_MAX)) {
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
    return len;
}

static LPWSTR xgetfullpath(LPCWSTR path, LPWSTR dst, DWORD siz)
{
    DWORD len;

    ASSERT_WSTR(path, NULL);
    len = GetFullPathNameW(path, siz, dst, NULL);
    if ((len == 0) || (len >= siz))
        return NULL;
    fixshortpath(dst, len);
    return dst;
}

static LPWSTR xgetfinalpath(LPCWSTR path, int isdir,
                            LPWSTR dst, DWORD siz)
{
    HANDLE fh;
    WCHAR  sbb[SVCBATCH_PATH_MAX];
    LPWSTR buf = dst;
    DWORD  len;
    DWORD  atr = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

    if (dst == NULL) {
        siz = SVCBATCH_PATH_MAX;
        buf = sbb;
    }
    len = GetFullPathNameW(path, siz, buf, NULL);
    if ((len == 0) || (len >= siz))
        return NULL;

    fh = CreateFileW(buf, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, atr, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, buf, siz, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= siz))
        return NULL;

    fixshortpath(buf, len);
    if (dst == NULL)
        dst = xwcsdup(buf);
    return dst;
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

    fixshortpath(dst, len);
    return dst;
}

static LPWSTR xsearchexe(LPCWSTR name)
{
    WCHAR  buf[SVCBATCH_PATH_MAX];
    DWORD  len;
    HANDLE fh;

    len = SearchPathW(NULL, name, L".exe", SVCBATCH_PATH_MAX, buf, NULL);
    if ((len == 0) || (len >= SVCBATCH_PATH_MAX))
        return NULL;

    fh = CreateFileW(buf, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, buf, SVCBATCH_PATH_MAX, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= SVCBATCH_PATH_MAX))
        return NULL;

    fixshortpath(buf, len);
    return xwcsdup(buf);
}

static BOOL resolvescriptname(LPCWSTR bp)
{
    LPWSTR p;

    DBG_PRINTF("name: %S", bp);
    if (cmdproc->script)
        return TRUE;
    cmdproc->script = xgetfinalpath(bp, 0, NULL, 0);
    if (IS_EMPTY_WCS(cmdproc->script))
        return FALSE;
    p = xwcsrchr(cmdproc->script, L'\\');
    if (p) {
        *p = WNUL;
        service->base = xwcsdup(cmdproc->script);
        *p = L'\\';
        return TRUE;
    }
    else {
        SAFE_MEM_FREE(cmdproc->script);
        service->base = NULL;
        return FALSE;
    }
}

static void setsvcstatusexit(DWORD e)
{
    SVCBATCH_CS_ENTER(service);
    service->status.dwServiceSpecificExitCode = e;
    SVCBATCH_CS_LEAVE(service);
}

static void reportsvcstatus(DWORD status, DWORD param)
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
                xsyserror(param, L"Service stopped without SERVICE_CONTROL_STOP signal", NULL);
                SVCBATCH_CS_LEAVE(service);
                exit(ERROR_INVALID_LEVEL);
            }
            else {
                if (svcfailmode == SVCBATCH_FAIL_NONE) {
                    xsysinfo(L"Service stopped without SERVICE_CONTROL_STOP signal", NULL);
                    param = 0;
                    service->status.dwWin32ExitCode = NO_ERROR;
                }
                else {
                    /* svcfailmode == SVCBATCH_FAIL_ERROR */
                    xsyswarn(param, L"Service stopped without SERVICE_CONTROL_STOP signal", NULL);
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

#if SVCBATCH_LEAN_AND_MEAN
static BOOL logwlines(HANDLE h, int nl, LPCSTR sb, LPCSTR xb)
{
    char  wb[SBUFSIZ];
    DWORD wr;
    DWORD nw;

    ASSERT_HANDLE(h, FALSE);
    nw = xtimehdr(wb, SBUFSIZ);
    if (nl) {
        WriteFile(h, wb,     nw, &wr, NULL);
        WriteFile(h, CRLFA,   2, &wr, NULL);
    }
    if (sb || xb) {
        wb[nw++] = ' ';
        WriteFile(h, wb,     nw, &wr, NULL);

        nw = xstrlen(sb);
        if (nw)
            WriteFile(h, sb, nw, &wr, NULL);
        nw = xstrlen(xb);
        if (nw)
            WriteFile(h, xb, nw, &wr, NULL);
        WriteFile(h, CRLFA,   2, &wr, NULL);
    }
    return TRUE;
}

static BOOL logwrline(HANDLE h, int nl, LPCSTR sb)
{
    return logwlines(h, nl, sb, NULL);
}

static void logprintf(HANDLE h, int nl, LPCSTR format, ...)
{
    int     c;
    char    buf[SVCBATCH_LINE_MAX];
    va_list ap;

    if (IS_INVALID_HANDLE(h))
        return;
    va_start(ap, format);
    c = xvsnprintf(buf, SVCBATCH_LINE_MAX, format, ap);
    va_end(ap);
    if (c > 0)
        logwrline(h, nl, buf);
}

static void logwwrite(HANDLE h, int nl, LPCSTR hdr, LPCWSTR wcs)
{
    char buf[UBUFSIZ];
    int  len;

    if (IS_INVALID_HANDLE(h))
        return;
    len = xwcslen(wcs);
    if (len) {
        int c = UBUFSIZ - 1;
        int r;

        r = WideCharToMultiByte(CP_UTF8, 0, wcs, len, buf, c, NULL, NULL);
        if ((r == 0) || (r >= c)) {
            r = GetLastError();
            logwlines(h, nl, hdr, r == ERROR_INSUFFICIENT_BUFFER ? "<OVERFLOW>" : "<INVALID>");
        }
        else {
            buf[r] = '\0';
            logwlines(h, nl, hdr, buf);
        }
    }
    else {
        logwlines(h, nl, hdr, NULL);
    }
}

static void logwrtime(HANDLE h, int nl, LPCSTR hdr)
{
    SYSTEMTIME tt;

    if (IS_INVALID_HANDLE(h))
        return;
    if (IS_SET(SVCBATCH_OPT_LOCALTIME))
        GetLocalTime(&tt);
    else
        GetSystemTime(&tt);
    logprintf(h, nl, "%-16s : %.4hu-%.2hu-%.2hu %.2hu:%.2hu:%.2hu.%.3hu",
              hdr, tt.wYear, tt.wMonth, tt.wDay, tt.wHour,
              tt.wMinute, tt.wSecond, tt.wMilliseconds);
}

static void logwinver(HANDLE h)
{
    OSVERSIONINFOEXW os;
    char             nb[SVCBATCH_NAME_MAX] = { 0, 0};
    char             vb[SVCBATCH_NAME_MAX] = { 0, 0};
    LPCSTR           pv = NULL;
    DWORD            sz;
    DWORD            br = 0;
    HKEY             hk;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;

    /**
     * C4996: 'GetVersionExW': was declared deprecated
     */
    ZeroMemory(&os, sizeof(OSVERSIONINFOEXW));
    os.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
    GetVersionExW((LPOSVERSIONINFOW)&os);

    sz = SVCBATCH_NAME_MAX - 1;
    if (RegGetValueA(hk, NULL, "ProductName",
                     RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                     NULL, nb, &sz) != ERROR_SUCCESS)
        goto finished;
    sz = SVCBATCH_NAME_MAX - 1;
    if (RegGetValueA(hk, NULL, "ReleaseId",
                     RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                     NULL, vb, &sz) == ERROR_SUCCESS)
        pv = vb;
    sz = SVCBATCH_NAME_MAX - 1;
    if (RegGetValueA(hk, NULL, "DisplayVersion",
                     RRF_RT_REG_SZ,
                     NULL, vb, &sz) == ERROR_SUCCESS)
        pv = vb;
    if (pv == NULL)
        goto finished;
    sz = 4;
    RegGetValueA(hk, NULL, "UBR", RRF_RT_REG_DWORD,
                 NULL, (PVOID)&br, &sz);

    logprintf(h, 0, "OS Name          : %s", nb);
    logprintf(h, 0, "OS Version       : %s %lu.%lu.%lu.%lu", vb,
              os.dwMajorVersion, os.dwMinorVersion,
              os.dwBuildNumber, br);

finished:
    CloseHandle(hk);
}


static void logconfig(HANDLE h)
{
    DWORD i;

    logwrline(h, 0, cnamestamp);
    logwinver(h);
    logwwrite(h, 1, "Service name     : ", service->name);
    logwwrite(h, 0, "Service uuid     : ", service->uuid);
    logwwrite(h, 0, "Script program   : ", cmdproc->application);
    for (i = 0; i < cmdproc->optc; i++)
    logwwrite(h, 0, "                   ", cmdproc->opts[i]);
    logwwrite(h, 0, "Script file      : ", cmdproc->script);
    for (i = 0; i < cmdproc->argc; i++)
    logwwrite(h, 0, "                   ", cmdproc->args[i]);
    if (svcstop) {
    logwwrite(h, 0, "Shutdown         : ", svcstop->script);
    for (i = 0; i < svcstop->argc; i++)
    logwwrite(h, 0, "                   ", svcstop->args[i]);
    }
    logwwrite(h, 0, "Program name     : ", program->name);
    logwwrite(h, 0, "Program directory: ", program->directory);
    logwwrite(h, 0, "Base directory   : ", service->base);
    logwwrite(h, 0, "Home directory   : ", service->home);
    logwwrite(h, 0, "Logs directory   : ", service->logs);
    logwwrite(h, 0, "Work directory   : ", service->work);

    FlushFileBuffers(h);
}

static void logwrstat(LPSVCBATCH_LOG log, int nl, int wt, LPCSTR hdr)
{
    HANDLE h;

    if (log == NULL)
        return;
    SVCBATCH_CS_ENTER(log);
    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h == NULL)
        goto finished;
    if (InterlockedCompareExchange(&log->count, 1, 0) == 0)
        nl = 1;
    else if (nl)
        nl--;
    if (wt)
        logwrtime(h, nl, hdr);
    else
        logwrline(h, nl, hdr);
finished:
    InterlockedExchangePointer(&log->fd, h);
    SVCBATCH_CS_LEAVE(log);
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

static DWORD createlogsdir(LPSVCBATCH_LOG log)
{
    LPWSTR p;
    WCHAR dp[SVCBATCH_PATH_MAX];

    if (isrelativepath(outdirparam)) {
        int i;
        i = xwcsncat(dp, SVCBATCH_PATH_MAX, 0, service->work);
        i = xwcsncat(dp, SVCBATCH_PATH_MAX, i, L"\\");
        i = xwcsncat(dp, SVCBATCH_PATH_MAX, i, outdirparam);
        xfixpathsep(dp);
    }
    else {
        p = xgetfullpath(outdirparam, dp, SVCBATCH_PATH_MAX);
        if (p == NULL) {
            xsyserror(0, L"xgetfullpath", outdirparam);
            return ERROR_BAD_PATHNAME;
        }
    }
    p = xgetdirpath(dp, service->logs, SVCBATCH_PATH_MAX);
    if (p == NULL) {
        DWORD rc = GetLastError();

        if (rc > ERROR_PATH_NOT_FOUND)
            return xsyserror(rc, L"xgetdirpath", dp);

        rc = xcreatedir(dp);
        if (rc != 0)
            return xsyserror(rc, L"xcreatedir", dp);
        p = xgetdirpath(dp, service->logs, SVCBATCH_PATH_MAX);
        if (p == NULL)
            return xsyserror(GetLastError(), L"xgetdirpath", dp);
    }
    if (_wcsicmp(service->logs, service->home) == 0) {
        xsyserror(0, L"Logs directory cannot be the same as home directory", service->logs);
        return ERROR_INVALID_PARAMETER;
    }
    return 0;
}

static DWORD rotateprevlogs(LPSVCBATCH_LOG log, BOOL ssp, HANDLE ssh)
{
    DWORD rc;
    int   i;
    int   x;
    WCHAR lognn[SVCBATCH_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA ad;

    x = xwcslcpy(lognn, SVCBATCH_PATH_MAX, log->logFile);
    if (log->maxLogs > 1)
        x = xwcsncat(lognn, SVCBATCH_PATH_MAX, x, L".0");
    else
        x = xwcsncat(lognn, SVCBATCH_PATH_MAX, x, xmktimedext());
    if (x >= SVCBATCH_PATH_MAX)
        return xsyserror(ERROR_BAD_PATHNAME, lognn, NULL);

    if (GetFileAttributesExW(log->logFile, GetFileExInfoStandard, &ad)) {
        if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
            DBG_PRINTS("empty log");
            if (ssh) {
                logwrline(ssh, 0, "Empty");
                logwwrite(ssh, 0, "- ", log->logFile);
            }
            return 0;
        }
        if (!MoveFileExW(log->logFile, lognn, MOVEFILE_REPLACE_EXISTING))
            return xsyserror(GetLastError(), log->logFile, lognn);
        if (ssp)
            reportsvcstatus(SERVICE_START_PENDING, 0);
        if (ssh) {
            logwrline(ssh, 0, "Moving");
            logwwrite(ssh, 0, "  ", log->logFile);
            logwwrite(ssh, 0, "> ", lognn);
        }
    }
    else {
        rc = GetLastError();
        if (rc != ERROR_FILE_NOT_FOUND)
            return xsyserror(rc, log->logFile, NULL);
        else
            return 0;
    }
    if (log->maxLogs > 1) {
        int n = log->maxLogs;
        WCHAR logpn[SVCBATCH_PATH_MAX];

        wmemcpy(logpn, lognn, x + 1);
        x--;
        for (i = 2; i < log->maxLogs; i++) {
            lognn[x] = L'0' + i;

            if (!GetFileAttributesExW(lognn, GetFileExInfoStandard, &ad))
                break;
            if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0))
                break;
        }
        n = i;
        if (ssh)
            logprintf(ssh, 0, "Rotating %d of %d", n, log->maxLogs);
        /**
         * Rotate previous log files
         */
        for (i = n; i > 0; i--) {
            logpn[x] = L'0' + i - 1;
            lognn[x] = L'0' + i;
            if (GetFileAttributesExW(logpn, GetFileExInfoStandard, &ad)) {
                if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
                    if (ssh)
                        logwwrite(ssh, 0, "- ", logpn);
                }
                else {
                    if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING))
                        return xsyserror(GetLastError(), logpn, lognn);
                    if (ssp)
                        reportsvcstatus(SERVICE_START_PENDING, 0);
                    if (ssh) {
                        logwwrite(ssh, 0, "  ", logpn);
                        logwwrite(ssh, 0, "> ", lognn);
                    }
                }
            }
            else {
                rc = GetLastError();
                if (rc != ERROR_FILE_NOT_FOUND)
                    return xsyserror(rc, logpn, NULL);
            }
        }
    }

    return 0;
}

#define _IsLeapYear(y) ((!(y % 4)) ? (((y % 400) && !(y % 100)) ? 0 : 1) : 0)
static int getdayofyear(int y, int m, int d)
{
    static const int dayoffset[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int r;

    r = dayoffset[m - 1] + d;
    if (_IsLeapYear(y) && (r > 59))
        r++;
    return r;
}

static int xwcsftime(LPWSTR dst, int siz, LPCWSTR fmt)
{
    LPCWSTR s = fmt;
    LPWSTR  d = dst;
    int     n = siz;
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
        if (*s == L'%') {
            int i = 0;
            int w;
            s++;
            switch (*s) {
                case L'%':
                    d[i++] = L'%';
                break;
                case L'C':
                    d[i++] = tm.wYear / 1000 + L'0';
                    d[i++] = tm.wYear % 1000 / 100 + L'0';
                break;
                case L'y':
                    d[i++] = tm.wYear % 100 / 10 + L'0';
                    d[i++] = tm.wYear % 10 + L'0';
                break;
                case L'Y':
                    ASSERT_SIZE(n, 4, siz);
                    d[i++] = tm.wYear / 1000 + L'0';
                    d[i++] = tm.wYear % 1000 / 100 + L'0';
                    d[i++] = tm.wYear % 100 / 10 + L'0';
                    d[i++] = tm.wYear % 10 + L'0';
                break;
                case L'd':
                    d[i++] = tm.wDay  / 10 + L'0';
                    d[i++] = tm.wDay % 10 + L'0';
                break;
                case L'm':
                    d[i++] = tm.wMonth / 10 + L'0';
                    d[i++] = tm.wMonth % 10 + L'0';
                break;
                case L'H':
                    d[i++] = tm.wHour / 10 + L'0';
                    d[i++] = tm.wHour % 10 + L'0';
                break;
                case L'M':
                    d[i++] = tm.wMinute / 10 + L'0';
                    d[i++] = tm.wMinute % 10 + L'0';
                break;
                case L'S':
                    d[i++] = tm.wSecond / 10 + L'0';
                    d[i++] = tm.wSecond % 10 + L'0';
                break;
                case L'j':
                    w = getdayofyear(tm.wYear, tm.wMonth, tm.wDay);
                    d[i++] = w / 100 + L'0';
                    d[i++] = w % 100 / 10 + L'0';
                    d[i++] = w % 10 + L'0';
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
                break;
                case L'w':
                    d[i++] = L'0' + tm.wDayOfWeek;
                break;
                /** Custom formatting codes */
                case L's':
                    ASSERT_SIZE(n,  3, siz);
                    d[i++] = tm.wMilliseconds / 100 + L'0';
                    d[i++] = tm.wMilliseconds % 100 / 10 + L'0';
                    d[i++] = tm.wMilliseconds % 10 + L'0';
                break;
                case L'L':
                    ASSERT_SIZE(n, 14, siz);
                    d[i++] = tm.wYear / 1000 + L'0';
                    d[i++] = tm.wYear % 1000 / 100 + L'0';
                    d[i++] = tm.wYear % 100 / 10 + L'0';
                    d[i++] = tm.wYear % 10 + L'0';
                    d[i++] = tm.wMonth / 10 + L'0';
                    d[i++] = tm.wMonth % 10 + L'0';
                    d[i++] = tm.wDay  / 10 + L'0';
                    d[i++] = tm.wDay % 10 + L'0';
                    d[i++] = tm.wHour / 10 + L'0';
                    d[i++] = tm.wHour % 10 + L'0';
                    d[i++] = tm.wMinute / 10 + L'0';
                    d[i++] = tm.wMinute % 10 + L'0';
                    d[i++] = tm.wSecond / 10 + L'0';
                    d[i++] = tm.wSecond % 10 + L'0';
                break;
                case L'N':
                    i = xwcslcpy(d, n, service->name);
                break;
                case L'P':
                    i = xwcslcpy(d, n, program->name);
                break;
                default:
                    SetLastError(ERROR_BAD_FORMAT);
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
    return (int)(d - dst);
}

static DWORD makelogname(LPWSTR dst, int siz, LPCWSTR src)
{
    DWORD rc;
    rc = xwcsftime(dst, siz, src);
    DBG_PRINTF("%S -> %S", src, dst);
    return rc;
}

static DWORD openlogfile(LPSVCBATCH_LOG log, BOOL ssp, HANDLE ssh)
{
    HANDLE  fh = NULL;
    DWORD   rc;
    DWORD   cd = CREATE_ALWAYS;
    WCHAR   nb[SVCBATCH_NAME_MAX];
    BOOL    rp = TRUE;
    LPCWSTR np = log->logName;
    int     i;

    if (ssp) {
        if (ssh)
            logwrtime(ssh, 1, "Log create");
    }
    else {
        rp = IS_NOT(SVCBATCH_OPT_TRUNCATE);
    }
    if (xwcschr(np, L'%')) {
        rc = makelogname(nb, SVCBATCH_NAME_MAX, np);
        if (rc == 0)
            return xsyserror(GetLastError(), np, NULL);
        rp = FALSE;
        np = nb;
        if (log == statuslog)
            cd = OPEN_ALWAYS;
    }
    i = xwcsncat(log->logFile, SVCBATCH_PATH_MAX, 0, service->logs);
    i = xwcsncat(log->logFile, SVCBATCH_PATH_MAX, i, L"\\");
    i = xwcsncat(log->logFile, SVCBATCH_PATH_MAX, i, np);
    i = xwcsncat(log->logFile, SVCBATCH_PATH_MAX, i, log->fileExt);

    if (rp) {
        rc = rotateprevlogs(log, ssp, ssh);
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
    if (ssh) {
        if (rc == ERROR_ALREADY_EXISTS)
            logwwrite(ssh, 0, "x ", log->logFile);
        else
            logwwrite(ssh, 0, "+ ", log->logFile);
        if (ssp)
            logwrtime(ssh, 0, "Log opened");
    }
    if ((rc == ERROR_ALREADY_EXISTS) && (cd == OPEN_ALWAYS)) {
        LARGE_INTEGER off = {{ 0, 0 }};

        DBG_PRINTF("appending to the %S", log->logFile);
        if (SetFilePointerEx(fh, off, NULL, FILE_END)) {
            DWORD wr;
            WriteFile(fh, CRLFA, 2, &wr, NULL);
            WriteFile(fh, CRLFA, 2, &wr, NULL);
        }
    }
#if defined(_DEBUG)
    else {
        if (rc == ERROR_ALREADY_EXISTS)
            dbgprintf(ssp ? "createlogfile" : "openlogfile",
                      "truncated %S", log->logFile);
        else
            dbgprintf(ssp ? "createlogfile" : "openlogfile",
                      "created %S",   log->logFile);
    }
#endif
    InterlockedExchange64(&log->size, 0);
    InterlockedExchangePointer(&log->fd, fh);

    return 0;
}

static DWORD rotatelogs(LPSVCBATCH_LOG log)
{
    DWORD  rc = 0;
    LONG   nr = 0;
    HANDLE h  = NULL;
    HANDLE s  = NULL;

    SVCBATCH_CS_ENTER(log);
    InterlockedExchange(&log->state, 1);

    if (statuslog) {
        SVCBATCH_CS_ENTER(statuslog);
        s = InterlockedExchangePointer(&statuslog->fd, NULL);
        logwrline(s, 0, "Rotating");
    }
    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h == NULL) {
        rc = ERROR_FILE_NOT_FOUND;
        goto finished;
    }
    nr = InterlockedIncrement(&log->count);
    FlushFileBuffers(h);
    if (s) {
        LARGE_INTEGER sz;

        logwwrite(s, 0, "  ", log->logFile);
        if (GetFileSizeEx(h, &sz))
        logprintf(s, 0, "  size           : %lld", sz.QuadPart);
        if (rotatebysize)
        logprintf(s, 0, "  rotate size    : %lld", rotatesize);
        logprintf(s, 0, "Log generation   : %lu", nr);
    }
    if (IS_SET(SVCBATCH_OPT_TRUNCATE)) {
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (SetFilePointerEx(h, ee, NULL, FILE_BEGIN)) {
            if (SetEndOfFile(h)) {
                logwrtime(s, 0, "Log truncated");
                InterlockedExchangePointer(&log->fd, h);
                goto finished;
            }
        }
        rc = GetLastError();
        xsyserror(rc, log->logFile, NULL);
        CloseHandle(h);
    }
    else {
        CloseHandle(h);
        rc = openlogfile(log, FALSE, s);
    }
    if (rc)
        setsvcstatusexit(rc);
    logwrtime(s, 0, "Log rotated");

finished:
    if (statuslog) {
        InterlockedExchangePointer(&statuslog->fd, s);
        InterlockedExchange(&statuslog->count, 0);
        SVCBATCH_CS_LEAVE(statuslog);
    }
    InterlockedExchange64(&log->size, 0);
    SVCBATCH_CS_LEAVE(log);
    return rc;
}

static DWORD closelogfile(LPSVCBATCH_LOG log)
{
    HANDLE h;
    HANDLE s = NULL;

    if (log == NULL)
        return ERROR_FILE_NOT_FOUND;

    SVCBATCH_CS_ENTER(log);
    DBG_PRINTF("%d %S", log->state, log->logFile);
    InterlockedExchange(&log->state, 1);
    if (log != statuslog) {
        if (statuslog) {
            SVCBATCH_CS_ENTER(statuslog);
            s = InterlockedExchangePointer(&statuslog->fd, NULL);
        }
    }

    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h) {
        if (log == statuslog)
            logwrtime(h, 0, "Status closed");
        if (s) {
            logwrline(s, 0, "Closing");
            logwwrite(s, 0, "  ", log->logFile);
        }
        FlushFileBuffers(h);
        CloseHandle(h);
        if (s)
            logwrtime(s, 0, "Log closed");
    }
    if (s) {
        InterlockedExchangePointer(&statuslog->fd, s);
        SVCBATCH_CS_LEAVE(statuslog);
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

    rotatebytime = TRUE;
}

static BOOL resolverotate(LPCWSTR rp)
{
    LPWSTR ep;

    ASSERT_WSTR(rp, FALSE);

    if (*rp == L'S') {
        if (rotatebysignal) {
            DBG_PRINTS("rotate by signal already defined");
            return FALSE;
        }
        DBG_PRINTS("rotate by signal");
        rotatebysignal = TRUE;
        return TRUE;
    }
    if (xwcspbrk(rp, L"BKMG")) {
        int      val;
        LONGLONG siz;
        LONGLONG mux = CPP_INT64_C(0);

        if (rotatebysize) {
            DBG_PRINTS("rotate by size already defined");
            return FALSE;
        }
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
            DBG_PRINTF("rotate size %S is less then %dK",
                       rp, SVCBATCH_MIN_ROTATE_SIZ);
            return FALSE;
        }
        else {
            DBG_PRINTF("rotate if larger then %S", rp);
            rotatesize   = siz;
            rotatebysize = TRUE;
        }
    }
    else {
        if (rotatebytime) {
            DBG_PRINTS("rotate by time already defined");
            return FALSE;
        }
        rotateinterval      = CPP_INT64_C(0);
        rotatetime.QuadPart = CPP_INT64_C(0);

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
            if (*ep)
                return FALSE;

            DBG_PRINTF("rotate each day at %.2d:%.2d:%.2d",
                       hh, mm, ss);
            resolvetimeout(hh, mm, ss, 1);
        }
        else {
            int mm;

            mm = xwcstoi(rp, &ep);
            if (*ep || mm < 0) {
                DBG_PRINTF("invalid rotate timeout %S", rp);
                return FALSE;
            }
            else if (mm == 0) {
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
                rotatebytime = TRUE;
            }
        }
    }
    return TRUE;
}

static DWORD runshutdown(void)
{
    WCHAR rb[SVCBATCH_UUID_MAX];
    DWORD i, rc = 0;

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
    ZeroMemory(sharedmem, sizeof(SVCBATCH_IPC));
    sharedmem->processId = program->pInfo.dwProcessId;
    sharedmem->options   = svcoptions & 0x000000FF;
    sharedmem->timeout   = stoptimeout;
    sharedmem->killdepth = killdepth;
    if (outputlog)
        xwcslcpy(sharedmem->logName, SVCBATCH_NAME_MAX, outputlog->logName);
    xwcslcpy(sharedmem->name, SVCBATCH_NAME_MAX, service->name);
    xwcslcpy(sharedmem->home, SVCBATCH_PATH_MAX, service->home);
    xwcslcpy(sharedmem->work, SVCBATCH_PATH_MAX, service->work);
    xwcslcpy(sharedmem->logs, SVCBATCH_PATH_MAX, service->logs);
    xwcslcpy(sharedmem->uuid, SVCBATCH_UUID_MAX, service->uuid);
    xwcslcpy(sharedmem->script,      SVCBATCH_PATH_MAX, svcstop->script);
    xwcslcpy(sharedmem->application, SVCBATCH_PATH_MAX, cmdproc->application);
    sharedmem->argc = svcstop->argc;
    for (i = 0; i < svcstop->argc; i++)
        wmemcpy(sharedmem->args[i], svcstop->args[i], SVCBATCH_NAME_MAX);
    sharedmem->optc = cmdproc->optc;
    for (i = 0; i < cmdproc->optc; i++)
        wmemcpy(sharedmem->opts[i], cmdproc->opts[i], SVCBATCH_NAME_MAX);

    rc = createiopipes(&svcstop->sInfo, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }
    svcstop->application = program->application;
    svcstop->commandLine = xappendarg(1, NULL, svcstop->application);
    svcstop->commandLine = xappendarg(0, svcstop->commandLine, rb);
    DBG_PRINTF("cmdline %S", svcstop->commandLine);
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
#endif

static DWORD WINAPI stopthread(void *msg)
{
    DWORD rc = 0;
    DWORD ws = WAIT_OBJECT_1;
    int   ri = stoptimeout;

    ResetEvent(svcstopdone);
    SetEvent(stopstarted);

    if (msg == NULL)
        reportsvcstatus(SERVICE_STOP_PENDING, stoptimeout + SVCBATCH_STOP_HINT);
    DBG_PRINTS("started");
#if SVCBATCH_LEAN_AND_MEAN
    if (outputlog) {
        SVCBATCH_CS_ENTER(outputlog);
        InterlockedExchange(&outputlog->state, 1);
        SVCBATCH_CS_LEAVE(outputlog);
    }
    if (statuslog && msg)
        logwrstat(statuslog, 2, 0, msg);
    if (svcstop) {
        ULONGLONG rs;

        DBG_PRINTS("creating shutdown process");
        rs = GetTickCount64();
        rc = runshutdown();
        ri = (int)(GetTickCount64() - rs);
        DBG_PRINTF("shutdown finished in %d ms", ri);
        reportsvcstatus(SERVICE_STOP_PENDING, 0);
        ri = stoptimeout - ri;
        if (ri < SVCBATCH_STOP_SYNC)
            ri = SVCBATCH_STOP_SYNC;
        DBG_PRINTF("waiting %d ms for worker", ri);
        ws = WaitForSingleObject(workerended, ri);
    }
#endif
    if (ws != WAIT_OBJECT_0) {
        reportsvcstatus(SERVICE_STOP_PENDING, 0);
        SetConsoleCtrlHandler(NULL, TRUE);
        if (IS_SET(SVCBATCH_OPT_BREAK)) {
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

    reportsvcstatus(SERVICE_STOP_PENDING, 0);
    if (ws != WAIT_OBJECT_0) {
        DBG_PRINTS("worker process is still running ... terminating");
        killprocess(cmdproc, ws);
    }
    else {
        DBG_PRINTS("worker process ended");
        cleanprocess(cmdproc);
    }
    reportsvcstatus(SERVICE_STOP_PENDING, 0);
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

#if SVCBATCH_LEAN_AND_MEAN
static void stopshutdown(DWORD rt)
{
    DWORD ws;

    DBG_PRINTS("started");
    SetConsoleCtrlHandler(NULL, TRUE);
    if (IS_SET(SVCBATCH_OPT_BREAK)) {
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
#endif

#if SVCBATCH_LEAN_AND_MEAN
static DWORD logwrdata(LPSVCBATCH_LOG log, BYTE *buf, DWORD len)
{
    DWORD  rc = 0;
    DWORD  wr = 0;
    HANDLE h;

#if defined(_DEBUG) && (_DEBUG > 2)
    DBG_PRINTF("writing %4lu bytes", len);
#endif
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
        return xsyserror(rc, L"Log write", NULL);
#if defined(_DEBUG) && (_DEBUG > 2)
    DBG_PRINTF("wrote   %4lu bytes", wr);
#endif
    if (IS_SET(SVCBATCH_OPT_ROTATE) && rotatebysize) {
        if (log->size >= rotatesize) {
            if (canrotatelogs(log)) {
                DBG_PRINTS("rotating by size");
                logwrstat(statuslog, 0, 1, "Rotate by size");
                SetEvent(dologrotate);
            }
        }
    }
    return 0;
}
#endif

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
        else
            DBG_PRINTF("error %lu", rc);
    }
    DBG_PRINTS("done");
#endif
    return rc;
}

#if SVCBATCH_LEAN_AND_MEAN
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
    if (rotatebytime) {
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
                logwrstat(statuslog, 0, 1, "Rotate by time");
                if (canrotatelogs(outputlog)) {
                    DBG_PRINTS("rotate by time");
                    rc = rotatelogs(outputlog);
                    rw = SVCBATCH_ROTATE_READY;
                }
                else {
                    DBG_PRINTS("rotate is busy ... canceling timer");
                    if (outputlog->size)
                        logwrstat(statuslog, 0, 0, "Canceling timer  : Rotate busy");
                    else
                        logwrstat(statuslog, 0, 0, "Canceling timer  : Log empty");
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
                logwrstat(statuslog, 1, 1, "Rotate ready");
                if (rotatebysize) {
                    if (outputlog->size >= rotatesize) {
                        InterlockedExchange(&outputlog->state, 1);
                        DBG_PRINTS("rotating by size");
                        logwrstat(statuslog, 0, 0, "Rotating by size");
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
#endif

static DWORD WINAPI workerthread(void *unused)
{
    DWORD    i;
    HANDLE   wr = NULL;
    LPHANDLE rp = NULL;
    LPHANDLE wp = NULL;
    DWORD    rc = 0;
    DWORD    cf = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
#if SVCBATCH_LEAN_AND_MEAN
    HANDLE   rd = NULL;
    LPSVCBATCH_PIPE op = NULL;
#endif

    DBG_PRINTS("started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_STARTING);
#if SVCBATCH_LEAN_AND_MEAN
    if (outputlog)
        rp = &rd;
#endif
    if (IS_SET(SVCBATCH_OPT_YYES))
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
#if SVCBATCH_LEAN_AND_MEAN
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
#endif
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    DBG_PRINTF("cmdline %S", cmdproc->commandLine);
    if (IS_SET(SVCBATCH_OPT_BREAK))
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

    if (IS_SET(SVCBATCH_OPT_YYES)) {
        if (!xcreatethread(SVCBATCH_WRPIPE_THREAD,
                           1, wrpipethread, wr)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"Write thread", NULL);
            TerminateProcess(cmdproc->pInfo.hProcess, rc);
            cmdproc->exitCode = rc;
            goto finished;
        }
    }
#if SVCBATCH_LEAN_AND_MEAN
    if (IS_SET(SVCBATCH_OPT_ROTATE)) {
        if (!xcreatethread(SVCBATCH_ROTATE_THREAD,
                           1, rotatethread, NULL)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"Rotate thread", NULL);
            TerminateProcess(cmdproc->pInfo.hProcess, rc);
            cmdproc->exitCode = rc;
            goto finished;
        }
    }
#endif
    ResumeThread(cmdproc->pInfo.hThread);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_RUNNING);
    if (IS_SET(SVCBATCH_OPT_YYES))
        ResumeThread(threads[SVCBATCH_WRPIPE_THREAD].thread);

    SAFE_CLOSE_HANDLE(cmdproc->pInfo.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    DBG_PRINTF("running process %lu", cmdproc->pInfo.dwProcessId);
#if SVCBATCH_LEAN_AND_MEAN
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
    else
#endif
    {
        WaitForSingleObject(cmdproc->pInfo.hProcess, INFINITE);
    }
    DBG_PRINTS("stopping");
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_STOPPING);
    if (IS_SET(SVCBATCH_OPT_YYES)) {
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
#if SVCBATCH_LEAN_AND_MEAN
    if (op != NULL) {
        SAFE_CLOSE_HANDLE(op->pipe);
        SAFE_CLOSE_HANDLE(op->o.hEvent);
        free(op);
    }
#endif
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
    static LPCSTR msg = "UNKNOWN";

    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
            msg = "Service signaled : SERVICE_CONTROL_PRESHUTDOWN";
        break;
        case SERVICE_CONTROL_SHUTDOWN:
            msg = "Service signaled : SERVICE_CONTROL_SHUTDOWN";
        break;
        case SERVICE_CONTROL_STOP:
            msg = "Service signaled : SERVICE_CONTROL_STOP";
        break;
#if SVCBATCH_LEAN_AND_MEAN
        case SVCBATCH_CTRL_ROTATE:
            msg = "Service signaled : SVCBATCH_CTRL_ROTATE";
        break;
#endif
    }
    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
            /* fall through */
        case SERVICE_CONTROL_SHUTDOWN:
            /* fall through */
            InterlockedExchange(&killdepth, 0);
        case SERVICE_CONTROL_STOP:
            DBG_PRINTF("service %s", msg);
            SVCBATCH_CS_ENTER(service);
            if (service->state == SERVICE_RUNNING) {
                reportsvcstatus(SERVICE_STOP_PENDING, stoptimeout + SVCBATCH_STOP_HINT);
                xcreatethread(SVCBATCH_STOP_THREAD, 0, stopthread, (LPVOID)msg);
            }
            SVCBATCH_CS_LEAVE(service);
        break;
#if SVCBATCH_LEAN_AND_MEAN
        case SVCBATCH_CTRL_BREAK:
            if (IS_SET(SVCBATCH_OPT_CTRL_BREAK)) {
                DBG_PRINTS("service SVCBATCH_CTRL_BREAK signaled");
                logwrstat(statuslog, 2, 0, "Service signaled : SVCBATCH_CTRL_BREAK");
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
            if (rotatebysignal) {
                BOOL rb = canrotatelogs(outputlog);
                /**
                 * Signal to rotatethread that
                 * user send custom service control
                 */
                logwrstat(statuslog, 1, 0, msg);
                if (rb) {
                    DBG_PRINTS("signaling SVCBATCH_CTRL_ROTATE");
                    SetEvent(dologrotate);
                }
                else {
                    DBG_PRINTS("rotatelogs is busy");
                    if (outputlog->size)
                        logwrstat(statuslog, 0, 1, "Log is busy");
                    else
                        logwrstat(statuslog, 0, 1, "Log is empty");
                    return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
                }
            }
            else {
                DBG_PRINTS("log rotation is disabled");
                return ERROR_INVALID_SERVICE_CONTROL;
            }
        break;
#endif
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
                DBG_PRINTF("[%d]", i);
                threads[i].duration = GetTickCount64() - threads[i].duration;
#endif
                threads[i].exitCode = ERROR_DISCARDED;
                TerminateThread(h, threads[i].exitCode);
            }
#if defined(_DEBUG)
            DBG_PRINTF("[%d] 0x%08x %10llums", i,
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
            DBG_PRINTF("[%d]", i);
#endif
            wh[nw++] = threads[i].thread;
        }
    }
    if (nw) {
        DBG_PRINTF("wait for %d", nw);
        if (nw > 1)
            WaitForMultipleObjects(nw, wh, TRUE, ms);
        else
            WaitForSingleObject(wh[0], ms);
    }
    DBG_PRINTS("done");
}

static void __cdecl cconsolecleanup(void)
{
    FreeConsole();
    DBG_PRINTS("done");
}

static void __cdecl objectscleanup(void)
{
    SAFE_CLOSE_HANDLE(workerended);
    SAFE_CLOSE_HANDLE(svcstopdone);
    SAFE_CLOSE_HANDLE(stopstarted);
#if SVCBATCH_LEAN_AND_MEAN
    SAFE_CLOSE_HANDLE(dologrotate);
    if (sharedmem)
        UnmapViewOfFile(sharedmem);
    SAFE_CLOSE_HANDLE(sharedmmap);
#endif
    SVCBATCH_CS_CLOSE(service);
    DBG_PRINTS("done");
}

static DWORD createevents(void)
{
    svcstopdone = CreateEventEx(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(svcstopdone))
        return GetLastError();
    stopstarted = CreateEventEx(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(stopstarted))
        return GetLastError();
#if SVCBATCH_LEAN_AND_MEAN
    if (IS_SET(SVCBATCH_OPT_ROTATE)) {
        dologrotate = CreateEventEx(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(dologrotate))
            return GetLastError();
    }
#endif
    return 0;
}

static LPWSTR *mergearguments(int orgc, LPCWSTR *orgv, LPWSTR msz, int *argc)
{
    int      x = 0;
    int      i = 0;
    int      c = 0;
    LPWSTR   p;
    LPWSTR  *argv;

    if (msz) {
        for (p = msz; *p; p++, c++) {
            while (*p)
                p++;
        }
    }
    c   += svcmainargc;
    c   += orgc;
    argv = xwaalloc(c);

    argv[i++] = (LPWSTR)orgv[0];
    /**
     * Add option arguments in the following order
     * ImagePath
     * ImagePathArguments
     * Service Start options
     */
    if (svcmainargc > 0) {
        for (x = 0; x < svcmainargc; x++)
            argv[i++] = (LPWSTR)svcmainargv[x];
    }
    if (msz) {
        for (p = msz; *p; p++) {
            argv[i++] = p;
            while (*p)
                p++;
        }
    }
    if (orgc > 1) {
        for (x = 1; x < orgc; x++)
            argv[i++] = (LPWSTR)orgv[x];
    }
    *argc = c;
#if defined(_DEBUG) && (_DEBUG > 2)
    for (x = 0; x < c; x++) {
        DBG_PRINTF("[%d] '%S'", x, argv[x]);
    }
#endif
    return argv;
}

static DWORD getsvcarguments(int orgc, LPCWSTR *orgv, int *argc, LPWSTR **argv)
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
    *argv = mergearguments(orgc, orgv, (LPWSTR)b, argc);
    return ERROR_SUCCESS;
}

static int parseoptions(int argc, LPCWSTR *argv)
{
    DWORD    x;
    int      i;
    int      opt;
#if SVCBATCH_LEAN_AND_MEAN
    int      rcnt = 0;
    int      scnt = 0;
    int      qcnt = 0;
#endif
    int      ccnt = 0;
    int      rargc;
    LPCWSTR *rargv;

    LPCWSTR  scriptparam  = NULL;
    LPCWSTR  svchomeparam = NULL;
    LPCWSTR  svcworkparam = NULL;
    LPCWSTR  commandparam = NULL;
#if SVCBATCH_LEAN_AND_MEAN
    LPCWSTR  svcstopparam = NULL;
    LPCWSTR  maxlogsparam = NULL;
    LPCWSTR  sparam[SVCBATCH_MAX_ARGS];
    LPCWSTR  rparam[4];
#endif
    LPCWSTR  cparam[SVCBATCH_MAX_ARGS];
    WCHAR    wb[BBUFSIZ];

    DBG_PRINTF("started %d", argc);
    x = getsvcarguments(argc, argv, &rargc, &rargv);
    if (x != ERROR_SUCCESS)
        return xsyserror(x, SVCBATCH_SVCARGS, NULL);
    argc = rargc;
    argv = rargv;

    while ((opt = xwgetopt(argc, argv, cmdoptions)) != EOF) {
        switch (opt) {
            case ':':
                if (scriptparam == NULL) {
                    scriptparam = xwoptarg;
                }
                else {
                    /**
                     * Add arguments for batch file
                     */
                    if (xwcslen(xwoptarg) >= SVCBATCH_NAME_MAX)
                        return xsyserror(0, L"The argument is too large", xwoptarg);

                    if (cmdproc->argc < SVCBATCH_MAX_ARGS)
                        cmdproc->args[cmdproc->argc++] = xwcsdup(xwoptarg);
                    else
                        return xsyserror(0, L"Too many arguments", xwoptarg);
                }
            break;
#if SVCBATCH_LEAN_AND_MEAN
            case 'b':
                svcoptions  |= SVCBATCH_OPT_CTRL_BREAK;
            break;
            case 'g':
                svcoptions  |= SVCBATCH_OPT_BREAK;
            break;
            case 'l':
                svcoptions  |= SVCBATCH_OPT_LOCALTIME;
            break;
            case 't':
                svcoptions  |= SVCBATCH_OPT_TRUNCATE;
            break;
            case 'v':
                svcoptions  |= SVCBATCH_OPT_VERBOSE;
            break;
#endif
            case 'p':
                preshutdown |= SERVICE_ACCEPT_PRESHUTDOWN;
            break;
            /**
             * Options with arguments
             */
#if SVCBATCH_LEAN_AND_MEAN
            case 'm':
                maxlogsparam = xwoptarg;
            break;
            case 'n':
                if (svclogfname)
                    return xsyserror(0, L"Found multiple -n command options", xwoptarg);
                svclogfname = xwcsdup(skipdotslash(xwoptarg));
                if (svclogfname == NULL)
                    xsyswarn(0, L"The -n command option value is invalid", xwoptarg);
                if (xwcspbrk(svclogfname, L"/\\:;<>?*|\""))
                    return xsyserror(0, L"Found invalid filename characters", svclogfname);
                xwcsreplace(svclogfname);
            break;
            case 'o':
                outdirparam  = skipdotslash(xwoptarg);
                if (outdirparam == NULL)
                    xsyswarn(0, L"The -o command option value is invalid", xwoptarg);
            break;
#endif
            case 'f':
                svcfailmode = xwcstoi(xwoptarg, NULL);
                if ((svcfailmode < 0) || (svcfailmode > 2))
                    return xsyserror(0, L"The -f command option value is outside valid range", xwoptarg);
            break;
            case 'h':
                svchomeparam = skipdotslash(xwoptarg);
                if (svchomeparam == NULL)
                    xsyswarn(0, L"The -h command option value is invalid", xwoptarg);
            break;
            case 'w':
                svcworkparam = skipdotslash(xwoptarg);
                if (svcworkparam == NULL)
                    xsyswarn(0, L"The -w command option value is invalid", xwoptarg);
            break;
            case 'k':
                stoptimeout  = xwcstoi(xwoptarg, NULL);
                if ((stoptimeout < SVCBATCH_STOP_TMIN) || (stoptimeout > SVCBATCH_STOP_TMAX))
                    return xsyserror(0, L"The -k command option value is outside valid range", xwoptarg);
                stoptimeout  = stoptimeout * 1000;
            break;
            /**
             * Options that can be defined
             * multiple times
             */
            case 'c':
                if (commandparam) {
                    if (ccnt < SVCBATCH_MAX_ARGS)
                        cparam[ccnt++] = xwoptarg;
                    else
                        return xsyserror(0, L"Too many -c arguments", xwoptarg);
                }
                else {
                    commandparam = skipdotslash(xwoptarg);
                    if (commandparam == NULL)
                        xsyswarn(0, L"The -c command option value is invalid", xwoptarg);
                }
            break;
            case 'e':
                x = xsetenv(xwoptarg);
                if (x)
                    return xsyserror(x, xwoptarg, NULL);
            break;
#if SVCBATCH_LEAN_AND_MEAN
            case 'q':
                svcoptions |= SVCBATCH_OPT_QUIET;
                qcnt++;
            break;
            case 's':
                if (svcstopparam) {
                    if (scnt < SVCBATCH_MAX_ARGS)
                        sparam[scnt++] = xwoptarg;
                    else
                        return xsyserror(0, L"Too many -s arguments", xwoptarg);
                }
                else {
                    svcstopparam = skipdotslash(xwoptarg);
                    if (svcstopparam == NULL)
                        xsyswarn(0, L"The -s command option value is invalid", xwoptarg);

                }
            break;
            case 'r':
                if (rcnt < 3)
                    rparam[rcnt++] = xwoptarg;
                else
                    return xsyserror(0, L"Too many -r options", xwoptarg);
            break;
#endif
            case ENOENT:
                return xsyserror(0, L"Missing argument for command line option", xwoption);
            break;
            default:
                return xsyserror(0, L"Invalid command line option", xwoption);
            break;
        }
    }

    argc -= xwoptind;
    argv += xwoptind;

    if (scriptparam == NULL) {
        if (argc == 0) {
            /**
             * No batch file defined.
             */
            if (xwcspbrk(service->name, L":;<>?*|\""))
                return xsyserror(0, L"Service name has invalid filename characters", NULL);
            i = xwcsncat(wb, BBUFSIZ, 0, service->name);
            i = xwcsncat(wb, BBUFSIZ, i, L".bat");
            scriptparam = wb;
        }
        else {
            scriptparam = argv[0];
            argc -= 1;
            argv += 1;
        }
    }
    for (i = 0; i < argc; i++) {
        /**
         * Add arguments for batch file
         */
        if (xwcslen(argv[i]) >= SVCBATCH_NAME_MAX)
            return xsyserror(0, L"The argument is too large", argv[i]);

        if (cmdproc->argc < SVCBATCH_MAX_ARGS)
            cmdproc->args[cmdproc->argc++] = xwcsdup(argv[i]);
        else
            return xsyserror(0, L"Too many arguments", argv[i]);
    }
    service->uuid = xuuidstring(NULL);
    if (IS_EMPTY_WCS(service->uuid))
        return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_UUID", NULL);
#if SVCBATCH_LEAN_AND_MEAN
    if (svcstopparam && qcnt > 1) {
        /**
         * Use -qq to only disable shutdown logging
         */
        qcnt = 0;
    }
    if (qcnt == 0) {
        outputlog = (LPSVCBATCH_LOG)xmcalloc(sizeof(SVCBATCH_LOG));

        outputlog->logName = svclogfname ? svclogfname : SVCBATCH_LOGNAME;
        outputlog->maxLogs = SVCBATCH_MAX_LOGS;
        outputlog->fileExt = SVCBATCH_LOGFEXT;
        if (maxlogsparam) {
            outputlog->maxLogs = xwcstoi(maxlogsparam, NULL);
            if ((outputlog->maxLogs < 1) || (outputlog->maxLogs > SVCBATCH_MAX_LOGS))
                return xsyserror(0, L"Invalid -m command option value", maxlogsparam);
        }
        SVCBATCH_CS_INIT(outputlog);
    }
    else {
        WCHAR bb[TBUFSIZ];
        /**
         * Ensure that log related command options
         * are not defined when -q is defined
         */
        i = 0;
        if (maxlogsparam)
            i = xwcsncat(bb, TBUFSIZ, i, L"-m ");
        if (rcnt)
            i = xwcsncat(bb, TBUFSIZ, i, L"-r ");
        if (IS_SET(SVCBATCH_OPT_TRUNCATE))
            i = xwcsncat(bb, TBUFSIZ, i, L"-t ");
        if (i) {
#if defined(_DEBUG) && (_DEBUG > 1)
            xsyswarn(0, L"Option -q is mutually exclusive with option(s)", bb);
            rcnt = 0;
#else
            return xsyserror(0, L"Option -q is mutually exclusive with option(s)", bb);
#endif
        }
    }
#endif
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

    if (svchomeparam == NULL)
        svchomeparam = svcworkparam;
    if (svcworkparam == NULL)
        svcworkparam = svchomeparam;

    if (isabsolutepath(svchomeparam)) {
        service->home = xgetfinalpath(svchomeparam, 1, NULL, 0);
        if (IS_EMPTY_WCS(service->home))
            return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
    }
    if (service->home == NULL) {
        if (isabsolutepath(scriptparam)) {
            if (!resolvescriptname(scriptparam))
                return xsyserror(ERROR_FILE_NOT_FOUND, scriptparam, NULL);

            if (svchomeparam == NULL) {
                service->home = service->base;
            }
            else {
                SetCurrentDirectoryW(service->base);
                service->home = xgetfinalpath(svchomeparam, 1, NULL, 0);
                if (IS_EMPTY_WCS(service->home))
                    return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
            }
        }
    }
    if (service->home == NULL) {
        if (svchomeparam == NULL) {
            service->home = program->directory;
        }
        else {
            SetCurrentDirectoryW(program->directory);
            service->home = xgetfinalpath(svchomeparam, 1, NULL, 0);
            if (IS_EMPTY_WCS(service->home))
                return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
        }
    }
    SetCurrentDirectoryW(service->home);
    if (svchomeparam == svcworkparam) {
        /* Use the same directories for home and work */
        service->work = service->home;
    }
    else {
        service->work = xgetfinalpath(svcworkparam, 1, NULL, 0);
        if (IS_EMPTY_WCS(service->work))
            return xsyserror(ERROR_FILE_NOT_FOUND, svcworkparam, NULL);
    }
    if (!resolvescriptname(scriptparam))
        return xsyserror(ERROR_FILE_NOT_FOUND, scriptparam, NULL);
    if (commandparam) {
        if (isrelativepath(commandparam))
            cmdproc->application = xsearchexe(commandparam);
        else
            cmdproc->application = xgetfinalpath(commandparam, 0, NULL, 0);
        if (cmdproc->application == NULL)
            return xsyserror(ERROR_FILE_NOT_FOUND, commandparam, NULL);
        for (i = 0; i < ccnt; i++) {
            if (xwcslen(cparam[i]) >= SVCBATCH_NAME_MAX)
                return xsyserror(0, L"The argument is too large", cparam[i]);
            cmdproc->opts[cmdproc->optc++] = cparam[i];
        }
    }
    else {
        LPWSTR wp = xgetenv(L"COMSPEC");
        if (wp == NULL)
            return xsyserror(ERROR_BAD_ENVIRONMENT, L"COMSPEC", NULL);

        cmdproc->application = xgetfinalpath(wp, 0, NULL, 0);
        if (cmdproc->application == NULL)
            return xsyserror(ERROR_FILE_NOT_FOUND, wp, NULL);
        xfree(wp);
        cmdproc->opts[cmdproc->optc++] = SVCBATCH_DEF_ARGS L" /C";
        svcoptions |= SVCBATCH_OPT_YYES;
    }
#if SVCBATCH_LEAN_AND_MEAN
    if (rcnt) {
        for (i = 0; i < rcnt; i++) {
            if (!resolverotate(rparam[i]))
                return xsyserror(0, L"Invalid rotate parameter", rparam[i]);
        }
        if (rotatebysize || rotatebytime)
            outputlog->maxLogs = 0;

        svcoptions |= SVCBATCH_OPT_ROTATE;
    }
    if (svcstopparam) {
        svcstop = (LPSVCBATCH_PROCESS)xmcalloc(sizeof(SVCBATCH_PROCESS));
        svcstop->script = xgetfinalpath(svcstopparam, 0, NULL, 0);
        if (svcstop->script == NULL)
            return xsyserror(ERROR_FILE_NOT_FOUND, svcstopparam, NULL);
        for (i = 0; i < scnt; i++) {
            if (xwcslen(sparam[i]) >= SVCBATCH_NAME_MAX)
                return xsyserror(0, L"The argument is too large", sparam[i]);
            svcstop->args[i] = xwcsdup(sparam[i]);
            xwchreplace(svcstop->args[i]);
        }
        svcstop->argc = scnt;
    }
#endif
    DBG_PRINTS("done");
    return 0;
}

static void WINAPI servicemain(DWORD argc, LPWSTR *argv)
{
    DWORD  rv = 0;
    DWORD  i;
#if SVCBATCH_LEAN_AND_MEAN
    HANDLE sh = NULL;
    LPSVCBATCH_LOG log = NULL;
#endif

    DBG_PRINTS("started");
    service->status.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    service->status.dwCurrentState = SERVICE_START_PENDING;

    if (argc > 0)
        service->name = argv[0];
    if (IS_EMPTY_WCS(service->name)) {
        xsyserror(ERROR_INVALID_PARAMETER, L"Service name", NULL);
        exit(1);
    }
    service->handle = RegisterServiceCtrlHandlerExW(service->name, servicehandler, NULL);
    if (IS_INVALID_HANDLE(service->handle)) {
        xsyserror(GetLastError(), L"RegisterServiceCtrlHandlerEx", service->name);
        exit(1);
    }
    DBG_PRINTF("%S", service->name);
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    rv = parseoptions(argc, argv);
    if (rv) {
        reportsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    rv = createevents();
    if (rv) {
        xsyserror(rv, L"CreateEvent", NULL);
        reportsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
#if SVCBATCH_LEAN_AND_MEAN
    log = outputlog;
    if (IS_SET(SVCBATCH_OPT_VERBOSE)) {
        statuslog = (LPSVCBATCH_LOG)xmcalloc(sizeof(SVCBATCH_LOG));

        statuslog->logName = svclogfname ? svclogfname : SVCBATCH_LOGNAME;
        statuslog->maxLogs = SVCBATCH_DEF_LOGS;
        statuslog->fileExt = SBSTATUS_LOGFEXT;
        SVCBATCH_CS_INIT(statuslog);

        log = statuslog;
    }
    if (log) {
        if (outdirparam == NULL)
            outdirparam = SVCBATCH_LOGSDIR;
        rv = createlogsdir(log);
        if (rv) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
    }
    else {
        if (outdirparam == NULL) {
#if defined(_DEBUG)
            xsyswarn(ERROR_INVALID_PARAMETER, L"log directory", NULL);
            xsysinfo(L"Use -o option with parameter set to the exiting directory",
                     L"failing over to SVCBATCH_SERVICE_WORK");
#endif
            xwcslcpy(service->logs, SVCBATCH_PATH_MAX, service->work);
        }
        else {
            LPWSTR op = xgetfinalpath(outdirparam, 1, service->logs, SVCBATCH_PATH_MAX);
            if (op == NULL) {
                rv = xsyserror(GetLastError(), L"xgetfinalpath", outdirparam);
                reportsvcstatus(SERVICE_STOPPED, rv);
                return;
            }
        }
    }
#else
    service->logs = service->work;
#endif
    for (i = 0; i < cmdproc->argc; i++)
        xwchreplace(cmdproc->args[i]);
    /**
     * Add additional environment variables
     * They are unique to this service instance
     */
    SetEnvironmentVariableW(L"SVCBATCH_APP_BIN",      program->application);
    SetEnvironmentVariableW(L"SVCBATCH_APP_DIR",      program->directory);
    SetEnvironmentVariableW(L"SVCBATCH_APP_VER",      SVCBATCH_VERSION_VER);

    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_BASE", service->base);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_HOME", service->home);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS", service->logs);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_NAME", service->name);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_UUID", service->uuid);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_WORK", service->work);
#if SVCBATCH_LEAN_AND_MEAN
    if (statuslog) {
        rv = openlogfile(statuslog, TRUE, NULL);
        if (rv != 0) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        sh = statuslog->fd;
        logconfig(sh);
    }
    if (outputlog) {
        reportsvcstatus(SERVICE_START_PENDING, 0);

        rv = openlogfile(outputlog, TRUE, sh);
        if (rv != 0) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
    }
#endif
    reportsvcstatus(SERVICE_START_PENDING, 0);

    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rv = xsyserror(GetLastError(), L"Worker thread", NULL);
        goto finished;
    }
    WaitForSingleObject(threads[SVCBATCH_WORKER_THREAD].thread, INFINITE);
    SVCBATCH_CS_ENTER(service);
    if (InterlockedExchange(&service->state, SERVICE_STOP_PENDING) != SERVICE_STOP_PENDING) {
        /**
         * Service ended without stop signal
         */
        DBG_PRINTS("ended without SERVICE_CONTROL_STOP");
#if SVCBATCH_LEAN_AND_MEAN
        logwrstat(statuslog, 2, 0, "Service stopped without SERVICE_CONTROL_STOP");
#endif
    }
    SVCBATCH_CS_LEAVE(service);
    DBG_PRINTS("waiting for stop to finish");
    WaitForSingleObject(svcstopdone, stoptimeout);
    waitforthreads(SVCBATCH_STOP_STEP);

finished:
    DBG_PRINTS("closing");
#if SVCBATCH_LEAN_AND_MEAN
    closelogfile(outputlog);
    closelogfile(statuslog);
#endif
    threadscleanup();
    reportsvcstatus(SERVICE_STOPPED, rv);
    DBG_PRINTS("done");
}

#if SVCBATCH_LEAN_AND_MEAN
static DWORD svcstopmain(void)
{
    DWORD rc;

    DBG_PRINTF("%S", service->name);
    if (outputlog) {
        rc = openlogfile(outputlog, FALSE, NULL);
        if (rc)
            return rc;
    }

    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rc = xsyserror(GetLastError(), L"Worker thread", NULL);
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
#endif

#if SVCBATCH_HAVE_SCM
static int setsvcarguments(SC_HANDLE svc, int argc, LPCWSTR *argv)
{
    int     e;
    int     n;
    DWORD   t;
    DWORD   c;
    HKEY    k = NULL;
    HKEY    a = NULL;
    LPBYTE  b = NULL;
    LSTATUS s;
    LPWSTR  w;

    if ((svc == NULL) || (argc == 0))
        return 0;
    e = __LINE__;
    s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, SYSTEM_SVC_SUBKEY,
                      0, KEY_QUERY_VALUE | KEY_READ | KEY_WRITE, &k);
    if (s != ERROR_SUCCESS)
        goto finished;
    e = __LINE__;
    s = RegGetValueW(k, service->name, SVCBATCH_SVCARGS,
                     RRF_RT_REG_MULTI_SZ, &t, NULL, &c);
    if (s == ERROR_SUCCESS) {
        b = (LPBYTE)xmmalloc(c);
        e = __LINE__;
        s = RegGetValueW(k, service->name, SVCBATCH_SVCARGS,
                         RRF_RT_REG_MULTI_SZ, &t, b, &c);
        if (s != ERROR_SUCCESS)
            goto finished;
        c = c - 2;
    }
    w = xargvtomsz(argc, argv, &n);
    if (b) {
        b = xrealloc(b, c + n);
        memcpy(b + c, w, n);
        n = n + c;
        xfree(w);
    }
    else {
        b = (LPBYTE)w;
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
    RegCloseKey(a);
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
    int s = xtolower(*ncmd++);

    if ((s == 'c') || (s == 'd') || (s == 's')) {
        int i = 0;
        int x = 0;
        WCHAR ccmd[16];

        while (*ncmd)
            ccmd[x++] = xtolower(*ncmd++);
        ccmd[x] = WNUL;
        while (scmcommands[i] != NULL) {
            if (wcscmp(ccmd, scmcommands[i] + 1) == 0)
                return i;
            i++;
        }
    }
    return -1;
}

static int xscmexecute(int cmd, int argc, LPCWSTR *argv)
{
    PSERVICE_CONTROL_STATUS_REASON_PARAMSW ssr;
    LPSERVICE_STATUS_PROCESS ssp;
    WCHAR     cb[SBUFSIZ];
    DWORD     bneed;
    int       i;
    int       x;
    int       opt;
    int       rv = 0;
    int       ec = 0;
    int       ep = 0;
    int       cmdverbose  = 0;
    ULONGLONG wtmstart    = 0;
    ULONGLONG wtimeout    = 0;
    LPCWSTR   ed          = NULL;
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
    DWORD     wtime       = 0;
    SC_HANDLE mgr         = NULL;
    SC_HANDLE svc         = NULL;

    ssr = (PSERVICE_CONTROL_STATUS_REASON_PARAMSW)xmcalloc(sizeof(SERVICE_CONTROL_STATUS_REASON_PARAMSW));
    ssp = &ssr->ServiceStatus;
    service->name = argv[0];
    DBG_PRINTF("%S %S", scmcommands[cmd], service->name);
    if (cmd == SVCBATCH_SCM_CREATE) {
        starttype   = SERVICE_DEMAND_START;
        servicetype = SERVICE_WIN32_OWN_PROCESS;
    }
    while ((opt = xlongopt(argc, argv, scmallowed[cmd], scmcoptions)) != EOF) {
        switch (opt) {
            case 'i':
                servicetype = SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
            break;
            case 'v':
                cmdverbose = xwcstoi(xwoptarg, NULL);
                if (cmdverbose < 1)
                    cmdverbose = 1;
            break;
            case 'b':
                if (binarypath != NULL) {
                    rv = ERROR_ALREADY_ASSIGNED;
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
                pp = xgetfinalpath(skipdotslash(xwoptarg), 0, NULL, 0);
                if (pp == NULL) {
                    rv = ERROR_FILE_NOT_FOUND;
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
                else {
                    binarypath = xappendarg(1, NULL, pp);
                    xfree(pp);
                }
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
            case 'p':
                password    = xwoptarg;
            break;
            case 'P':
                privileges  = xwoptarg;
            break;
            case 's':
                starttype = xnamemap(xwoptarg, starttypemap, SERVICE_NO_CHANGE);
                if (starttype == SERVICE_NO_CHANGE) {
                    rv = ERROR_INVALID_PARAMETER;
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
            break;
            case 'n':
                displayname = xwoptarg;
            break;
            case 'u':
                username    = xwoptarg;
            break;
            case 'w':
                if (xwoptarg) {
                    wtime = xwcstoi(xwoptarg, NULL);
                    if (wtime > SVCBATCH_STOP_TMAX)
                        wtime = SVCBATCH_STOP_TMAX;
                }
                if (wtime < SVCBATCH_STOP_TMIN)
                    wtime = SVCBATCH_SCM_WAIT_DEF;
            break;
            case ENOENT:
                rv = ERROR_BAD_LENGTH;
                ec = __LINE__;
                ed = xwoption;
                goto finished;
            break;
            default:
                rv = ERROR_BAD_DRIVER_LEVEL;
                ec = __LINE__;
                ed = xwoption;
                goto finished;
            break;
        }
    }
    argc -= xwoptind;
    argv += xwoptind;
    if (xwcspbrk(service->name, L"/\\")) {
        rv = ERROR_INVALID_NAME;
        ec = __LINE__;
        ed = L"The (/) and (\\) are not valid service name characters";
        goto finished;
    }
    if (xwcslen(service->name) > 256) {
        rv = ERROR_INVALID_NAME;
        ec = __LINE__;
        ed = L"The maximum service name length is 256 characters";
        goto finished;
    }
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
    if (wtime) {
        wtimeout = wtime * ONE_SECOND;
        wtmstart = GetTickCount64();
    }
    if (cmd == SVCBATCH_SCM_DELETE) {
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState == SERVICE_STOPPED)
            wtime = 0;
        if (wtime) {
            if (ssp->dwCurrentState != SERVICE_STOP_PENDING) {
                if (!ControlService(svc, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)ssp)) {
                    rv = GetLastError();
                    ec = __LINE__;
                    goto finished;
                }
            }
            while (ssp->dwCurrentState != SERVICE_STOPPED) {
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
                    /* Timeout */
                    rv = ERROR_SERVICE_REQUEST_TIMEOUT;
                    ec = __LINE__;
                    goto finished;
                }
            }
        }
        if (ssp->dwCurrentState != SERVICE_STOPPED) {
            rv = ERROR_SERVICE_ALREADY_RUNNING;
            ec = __LINE__;
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
            LPCWSTR rp = argv[0];
            if (xwcschr(rp, L':')) {
                LPWSTR sp;
                DWORD  sv;

                sv = xwcstoi(rp, &sp);
                if ((sv < 1) || (sv > 4)) {
                    rv = ERROR_INVALID_PARAMETER;
                    ec = __LINE__;
                    ed = argv[0];
                    goto finished;
                }
                srflag = (sv << 28);
                if (*sp == L':') {
                    rp = sp + 1;
                    sv = xwcstoi(rp, &sp);
                    if ((sv < 1) || (sv > 255)) {
                        rv = ERROR_INVALID_PARAMETER;
                        ec = __LINE__;
                        ed = argv[0];
                        goto finished;
                    }
                    srmajor = (sv << 16);
                }
                if (*sp == L':') {
                    rp = sp + 1;
                    sv = xwcstoi(rp, &sp);
                    if ((sv < 1) || (sv > 65535)) {
                        rv = ERROR_INVALID_PARAMETER;
                        ec = __LINE__;
                        ed = argv[0];
                        goto finished;
                    }
                    srminor = sv;
                }
            }
            else {
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ed = argv[0];
                goto finished;
            }
            if (argc > 1) {
                /* Comment is limited to 128 chars */
                xwcslcpy(cb, SBUFSIZ, argv[1]);
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
        DWORD sctrl;
        if (argc == 0) {
            rv = ERROR_INVALID_PARAMETER;
            ec = __LINE__;
            ed = L"The Control code is missing. Use control [service name] <value>";
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
            ed = L"The service is not in the RUNNING state";
            goto finished;
        }
        sctrl = xwcstoi(argv[0], NULL);
        if ((sctrl < 128) || (sctrl > 255)) {
            rv = ERROR_INVALID_PARAMETER;
            ec = __LINE__;
            ed = argv[0];
            goto finished;
        }
        if (!ControlServiceExW(svc, sctrl,
                               SERVICE_CONTROL_STATUS_REASON_INFO, (LPBYTE)ssr)) {
            rv = GetLastError();
            ec = __LINE__;
            ed = argv[0];
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
            xwinapierror(eb, SVCBATCH_LINE_MAX, rv);
            fprintf(stderr, "Service Name : %S\n", service->name);
            fprintf(stderr, "     Command : %S\n", scmcommands[cmd]);
            if (ep)
            fprintf(stdout, "               %d (%d)\n", ec, ep);
            else
            fprintf(stdout, "               %d\n", ec);
            if (wtime && cmdverbose > 1)
            fprintf(stderr, "               %llu ms\n", GetTickCount64() - wtmstart);
            fprintf(stdout, "             : FAILED\n");
            fprintf(stderr, "             : %d (0x%x)\n", rv,  rv);
            fprintf(stderr, "             : %S\n", eb);
            if (ed != NULL)
            fprintf(stderr, "               %S\n", ed);
            if ((cmdverbose > 1) && (argc > 0)) {
            fputs("\n   Arguments :\n", stderr);
            for (i = 0; i < argc; i++)
            fprintf(stderr, "               %S\n", argv[i]);
            }
            fputc('\n', stderr);
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
            fprintf(stdout, "     STARTUP : %d\n", starttype);
            if (cmd == SVCBATCH_SCM_START)
            fprintf(stdout, "         PID : %lu\n",  ssp->dwProcessId);
            if (cmd == SVCBATCH_SCM_STOP)
            fprintf(stdout, "    EXITCODE : %lu (0x%x)\n", ssp->dwServiceSpecificExitCode,
                                                           ssp->dwServiceSpecificExitCode);
            if ((cmdverbose > 1) && (argc > 0)) {
            fputs("\n   Arguments :\n", stdout);
            for (i = 0; i < argc; i++)
            fprintf(stdout, "               %S\n", argv[i]);
            }
            fputc('\n', stdout);
        }
    }
    DBG_PRINTF("done %d", rv);
    return rv;
}
#endif

static int xwmaininit(int argc, LPCWSTR *argv)
{
    WCHAR  bb[SVCBATCH_PATH_MAX];
    LPWSTR dp = NULL;
    DWORD  nn;
 #if SVCBATCH_LEAN_AND_MEAN
   LARGE_INTEGER i;

    /**
     * On systems that run Windows XP or later,
     * this functions will always succeed.
     */
    QueryPerformanceFrequency(&i);
    counterfreq = i.QuadPart;
    QueryPerformanceCounter(&i);
    counterbase = i.QuadPart;
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

    nn = GetModuleFileNameW(NULL, bb, SVCBATCH_PATH_MAX);
    if (nn == 0)
        return GetLastError();
    if (nn >= SVCBATCH_PATH_MAX)
        return ERROR_INSUFFICIENT_BUFFER;
    nn = fixshortpath(bb, nn);
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
    svcmainargc = argc - 1;
    svcmainargv = argv + 1;

    ASSERT_WSTR(program->application, ERROR_BAD_PATHNAME);
    ASSERT_WSTR(program->directory,   ERROR_BAD_PATHNAME);
    ASSERT_WSTR(program->name,        ERROR_BAD_PATHNAME);

    return 0;
}

int wmain(int argc, LPCWSTR *argv)
{
#if SVCBATCH_LEAN_AND_MEAN
    DWORD   x;
#endif
    int     i;
    int     r = 0;
    LPCWSTR p = NULL;

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
    if (argc == 2) {
        p = argv[1];
        if ((p[0] == L'/') && (p[1] == L'?') && (p[2] == WNUL)) {
            fputs(cnamestamp, stdout);
            return 0;
        }
    }
    r = xwmaininit(argc, argv);
    if (r != 0) {
        DBG_PRINTF("main failed with error %d", r);
        return r;
    }
#if SVCBATCH_LEAN_AND_MEAN
    /**
     * Check if running as service or as a child process.
     */
    if (xwstartswith(p, SVCBATCH_MMAPPFX)) {
        p += 2;
        servicemode = FALSE;
        cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT;
        wnamestamp  = CPP_WIDEN(SHUTDOWN_APPNAME) L" " SVCBATCH_VERSION_WCS;
        cwsappname  = CPP_WIDEN(SHUTDOWN_APPNAME);
#if defined(_DEBUG)
        dbgsvcmode = '1';
#endif
        sharedmmap  = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, p);
        if (sharedmmap == NULL)
            return xsyserror(GetLastError(), L"OpenFileMapping", p);

        sharedmem = (LPSVCBATCH_IPC)MapViewOfFile(
                                        sharedmmap,
                                        FILE_MAP_ALL_ACCESS,
                                        0, 0, DSIZEOF(SVCBATCH_IPC));
        if (sharedmem == NULL)
            return xsyserror(GetLastError(), L"MapViewOfFile", p);

        stoptimeout = sharedmem->timeout;
        svcoptions  = sharedmem->options;
        killdepth   = sharedmem->killdepth;
#if defined(_DEBUG)
        dbgprints(__FUNCTION__, cnamestamp);
        dbgprintf(__FUNCTION__, "ppid %lu", sharedmem->processId);
        dbgprintf(__FUNCTION__, "opts 0x%08x", sharedmem->options);
        dbgprintf(__FUNCTION__, "time %lu", stoptimeout);
#endif
        cmdproc->application = sharedmem->application;
        cmdproc->argc   = sharedmem->argc;
        cmdproc->optc   = sharedmem->optc;
        cmdproc->script = sharedmem->script;
        for (x = 0; x < cmdproc->argc; x++)
            cmdproc->args[x] = sharedmem->args[x];
        for (x = 0; x < cmdproc->optc; x++)
            cmdproc->opts[x] = sharedmem->opts[x];
        service->home = sharedmem->home;
        service->work = sharedmem->work;
        service->name = sharedmem->name;
        service->uuid = sharedmem->uuid;
        if (IS_NOT(SVCBATCH_OPT_QUIET)) {
            outputlog = (LPSVCBATCH_LOG)xmcalloc(sizeof(SVCBATCH_LOG));

            outputlog->logName = sharedmem->logName;
            outputlog->maxLogs = SVCBATCH_DEF_LOGS;
            outputlog->fileExt = SHUTDOWN_LOGFEXT;
            SVCBATCH_CS_INIT(outputlog);
        }
        xwcslcpy(service->logs, SVCBATCH_PATH_MAX, sharedmem->logs);
    }
#endif
#if SVCBATCH_HAVE_SCM
    if (servicemode && (argc > 2)) {
        /**
         * Check if this is a Service Manager command
         */
        i = xwcslen(argv[1]);
        if ((i > 3) && (i < 8)) {
            int cmd = xscmcommand(argv[1]);
            if (cmd >= 0) {
                argc  -= 2;
                argv  += 2;
                return xscmexecute(cmd, argc, argv);
            }
        }
    }
#endif
#if defined(_DEBUG)
    if (servicemode)
        dbgsvcmode = '0';
#endif

    /**
     * Create logic state events
     */
    workerended = CreateEventEx(NULL, NULL,
                                CREATE_EVENT_MANUAL_RESET,
                                EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(workerended))
        return xsyserror(GetLastError(), L"CreateEventEx", NULL);

    SVCBATCH_CS_INIT(service);
    atexit(objectscleanup);
    if (IS_INVALID_HANDLE(program->sInfo.hStdInput)) {
        HANDLE h = NULL;
        if (AllocConsole()) {
            /**
             * AllocConsole should create new set of
             * standard i/o handles
             */
            atexit(cconsolecleanup);
            h = GetStdHandle(STD_INPUT_HANDLE);
            ASSERT_HANDLE(h, ERROR_DEV_NOT_EXIST);
        }
        else {
            return xsyserror(GetLastError(), L"AllocConsole", NULL);
        }
        program->sInfo.hStdInput  = h;
        program->sInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        program->sInfo.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }

    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consolehandler, TRUE);

    if (servicemode) {
        SERVICE_TABLE_ENTRYW se[2];
        se[0].lpServiceName = zerostring;
        se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
        se[1].lpServiceName = NULL;
        se[1].lpServiceProc = NULL;
        if (!StartServiceCtrlDispatcherW(se))
            r = GetLastError();
    }
#if SVCBATCH_LEAN_AND_MEAN
    else {
        r = svcstopmain();
    }
#endif
    DBG_PRINTS("done");
    return r;
}
