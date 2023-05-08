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
static void dbgprintf(const char *, const char *, ...);
static void dbgprints(const char *, const char *);
#endif

typedef enum {
    SVCBATCH_WORKER_THREAD = 0,
    SVCBATCH_WRITE_THREAD,
    SVCBATCH_STOP_THREAD,
    SVCBATCH_MONITOR_THREAD,
    SVCBATCH_ROTATE_THREAD,
    SVCBATCH_MAX_THREADS
} SVCBATCH_THREAD_ID;

typedef struct _SVCBATCH_THREAD {
    volatile LONG          nStarted;
    LPTHREAD_START_ROUTINE lpStartAddress;
    HANDLE                 hThread;
    LPVOID                 lpParameter;
    DWORD                  dwId;
    DWORD                  dwThreadId;
    DWORD                  dwExitCode;
    ULONGLONG              dwDuration;
} SVCBATCH_THREAD, *LPSVCBATCH_THREAD;

typedef struct _SVCBATCH_PIPE {
    OVERLAPPED  oOverlap;
    HANDLE      hPipe;
    BYTE        bBuffer[HBUFSIZ];
    DWORD       nRead;
    DWORD       dwState;
} SVCBATCH_PIPE, *LPSVCBATCH_PIPE;

typedef struct _SVCBATCH_PROCESS {
    volatile LONG       dwCurrentState;
    DWORD               dwType;
    DWORD               dwCreationFlags;
    HANDLE              hRdPipe;
    HANDLE              hWrPipe;
    PROCESS_INFORMATION pInfo;
    STARTUPINFOW        sInfo;
    DWORD               dwExitCode;
    DWORD               nArgc;
    LPWSTR              lpCommandLine;
    LPWSTR              lpExe;
    LPWSTR              lpDir;
    LPWSTR              lpArgv[SVCBATCH_MAX_ARGS];
} SVCBATCH_PROCESS, *LPSVCBATCH_PROCESS;

typedef struct _SVCBATCH_LOG {
    volatile LONG       dwCurrentState;
    volatile LONG       nRotateCount;
    volatile HANDLE     hFile;
    volatile LONG64     nWritten;
    DWORD               dwType;
    CRITICAL_SECTION    csLock;
    LPWSTR              lpFileName;
    WCHAR               szDir[SVCBATCH_PATH_MAX];

} SVCBATCH_LOG, *LPSVCBATCH_LOG;

typedef struct _SVCBATCH_SERVICE {
    volatile LONG           dwCurrentState;
    SERVICE_STATUS_HANDLE   hStatus;
    SERVICE_STATUS          sStatus;
    CRITICAL_SECTION        csLock;

    LPWSTR                  lpBase;
    LPWSTR                  lpHome;
    LPWSTR                  lpLogs;
    LPWSTR                  lpName;
    LPWSTR                  lpUuid;
    LPWSTR                  lpWork;

} SVCBATCH_SERVICE, *LPSVCBATCH_SERVICE;

static LPSVCBATCH_PROCESS    svcstopproc = NULL;
static LPSVCBATCH_PROCESS    svcmainproc = NULL;
static LPSVCBATCH_PROCESS    svcxcmdproc = NULL;
static LPSVCBATCH_SERVICE    mainservice = NULL;
static LPSVCBATCH_LOG        svcbatchlog = NULL;

static LONGLONG              rotateint   = CPP_INT64_C(0);
static LARGE_INTEGER         rotatetmo   = {{ 0, 0 }};
static LARGE_INTEGER         rotatesiz   = {{ 0, 0 }};
static LARGE_INTEGER         pcfrequency = {{ 0, 0 }};
static LARGE_INTEGER         pcstarttime = {{ 0, 0 }};

static BOOL      hasctrlbreak     = FALSE;
static BOOL      haslogrotate     = FALSE;
static BOOL      rotatebysize     = FALSE;
static BOOL      rotatebytime     = FALSE;
static BOOL      uselocaltime     = FALSE;
static BOOL      haslogstatus     = FALSE;
static BOOL      truncatelogs     = FALSE;
static BOOL      havemainlogs     = TRUE;
static BOOL      havestoplogs     = TRUE;

static DWORD     preshutdown      = 0;
static int       svccodepage      = 0;
static int       svcmaxlogs       = SVCBATCH_MAX_LOGS;
static int       stoptimeout      = SVCBATCH_STOP_TIME;
static int       killdepth        = 2;

static HANDLE    svcstopstart     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    workfinished     = NULL;
static HANDLE    ctrlbreaksig     = NULL;
static HANDLE    logrotatesig     = NULL;

static SVCBATCH_THREAD svcthread[SVCBATCH_MAX_THREADS];

static wchar_t      zerostring[2] = { WNUL, WNUL };
static const wchar_t *CRLFW       = L"\r\n";
static const char    *CRLFA       =  "\r\n";
static const char    *YYES        =  "Y\r\n";

static const char    *cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;
static const wchar_t *wnamestamp  = CPP_WIDEN(SVCBATCH_NAME) L" " SVCBATCH_VERSION_WCS;
static const wchar_t *cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);
static const wchar_t *outdirparam = NULL;
static const wchar_t *codepageopt = NULL;
static const wchar_t *svclogfname = NULL;
static const wchar_t *svclogfnext = NULL;

static int            xwoptind    = 1;
static wchar_t        xwoption    = WNUL;
static const wchar_t *xwoptarg    = NULL;

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

#if defined(_DEBUG) && (_DEBUG > 1)

static volatile LONG    xncmmalloc  = 0;
static volatile LONG    xncmcalloc  = 0;
static volatile LONG    xncrealloc  = 0;
static volatile LONG    xnnrealloc  = 0;
static volatile LONG    xncfree     = 0;
static volatile LONG    xnmemalloc  = 0;
static volatile LONG    xnmemfree   = 0;
static volatile LONG    xdbgtrace   = 0;

typedef struct _dbgmemslot {
    volatile PVOID   ptr;
    DWORD            len;
    const char      *fc;
    const char      *fn;
    const char      *dsc;
    const wchar_t   *tag;
} dbgmemslot;

static dbgmemslot       dbgmemory[TBUFSIZ];
static CRITICAL_SECTION dbgmemlock;

static int dbgmemtrace(const char *f, const char *c, void *p, void *m, size_t s)
{
    int   i;
    DWORD n = (DWORD)s;

    EnterCriticalSection(&dbgmemlock);
    for (i = 0; i < TBUFSIZ; i++) {
        if (InterlockedCompareExchangePointer(&(dbgmemory[i].ptr), m, p) == p) {
            dbgmemory[i].len += n;
#if (_DEBUG > 2)
            if (InterlockedCompareExchange(&xdbgtrace, 0, 0)) {
                if (p == NULL)
                    dbgprintf(f, "%-10s %p %2d %6lu", c, m, i + 1, dbgmemory[i].len);
                else
                    dbgprintf(f, "%-10s %p %2d %6lu <- %p %6lu", c, m, i + 1,
                              dbgmemory[i].len, p, n);
            }
#endif
            if (p == NULL) {
                dbgmemory[i].fc  = c;
                dbgmemory[i].fn  = f;
                dbgmemory[i].tag = NULL;
                dbgmemory[i].dsc = NULL;
            }
            InterlockedAdd(&xnmemalloc, n);
            LeaveCriticalSection(&dbgmemlock);
            return i + 1;
        }
    }
    LeaveCriticalSection(&dbgmemlock);
    dbgprintf(f, "%-10s %p", c, p);
    return 0;
}

static int dbguntrace(const char *f, const char *c, void *m)
{
    int   i;

    EnterCriticalSection(&dbgmemlock);
    for (i = 0; i < TBUFSIZ; i++) {
        if (InterlockedCompareExchangePointer(&(dbgmemory[i].ptr), NULL, m) == m) {
            InterlockedAdd(&xnmemfree, dbgmemory[i].len);
            if (InterlockedCompareExchange(&xdbgtrace, 0, 0)) {
                const wchar_t *tag = dbgmemory[i].tag ? dbgmemory[i].tag : zerostring;
                if (dbgmemory[i].dsc)
                    dbgprintf(f, "%-10s %p %2d %6lu %s %S",
                              c, m, i + 1, dbgmemory[i].len,
                              dbgmemory[i].dsc, tag);
                else
                    dbgprintf(f, "%-10s %p %2d %6lu %S", c, m, i + 1,
                              dbgmemory[i].len, tag);
                dbgmemory[i].len = 0;
                dbgmemory[i].tag = NULL;
                dbgmemory[i].dsc = NULL;
            }
            LeaveCriticalSection(&dbgmemlock);
            return i + 1;
        }
    }
    LeaveCriticalSection(&dbgmemlock);
    dbgprintf(f, "%-10s %p  0", c, m);
    return 0;
}

static int dbgmemtag(const char *fn, void *mem, const wchar_t *tag, const char *dsc)
{
    int i;

    if (mem == NULL) {
        if (tag == NULL)
            return 0;
        mem = (void *)tag;
        tag = wcsrchr(tag, L'\\');
        if (tag)
            tag++;
        else
            tag = (const wchar_t *)mem;
    }
    EnterCriticalSection(&dbgmemlock);
    for (i = 0; i < TBUFSIZ; i++) {
        if (InterlockedCompareExchangePointer(&(dbgmemory[i].ptr), mem, mem) == mem) {
            if (tag)
                dbgmemory[i].tag = tag;
            if (dsc) {
                dbgmemory[i].dsc = dsc;
            }
            LeaveCriticalSection(&dbgmemlock);
            return i + 1;
        }
    }
    LeaveCriticalSection(&dbgmemlock);
    dbgprintf(fn, "dbgmemtag  %p", mem);
    return 0;
}

static void dbgmemdump(const char *fn)
{
    int   i;
    DWORD sc = 0;

    EnterCriticalSection(&dbgmemlock);
    InterlockedIncrement(&xdbgtrace);
    dbgprintf(fn, "allocated  %lu", xnmemalloc);
    dbgprintf(fn, "freed      %lu", xnmemfree);

    for (i = 0; i < TBUFSIZ; i++) {
        if (InterlockedCompareExchangePointer(&(dbgmemory[i].ptr), NULL, NULL)) {
            const wchar_t *tag = dbgmemory[i].tag ? dbgmemory[i].tag : zerostring;

            if (dbgmemory[i].dsc)
                dbgprintf(dbgmemory[i].fn, "%-10s %p %2d %6lu %s %S",
                          dbgmemory[i].fc,  dbgmemory[i].ptr, i + 1,
                          dbgmemory[i].len, dbgmemory[i].dsc, tag);
            else
                dbgprintf(dbgmemory[i].fn, "%-10s %p %2d %6lu %S",
                          dbgmemory[i].fc,  dbgmemory[i].ptr, i + 1,
                          dbgmemory[i].len, tag);
            sc++;
        }
    }
    dbgprintf(fn, "malloc     %lu", xncmmalloc);
    dbgprintf(fn, "realloc    %lu", xncrealloc);
    dbgprintf(fn, "calloc     %lu", xncmcalloc);
    dbgprintf(fn, "free       %lu", xncfree);
    dbgprintf(fn, "used       %lu", xncmmalloc + xncmcalloc + xnnrealloc);
    dbgprintf(fn, "left       %lu", sc);
    LeaveCriticalSection(&dbgmemlock);

}

#endif

static int xfatalerr(const char *func, int err)
{

    OutputDebugStringA(">>> " SVCBATCH_NAME " " SVCBATCH_VERSION_STR);
    OutputDebugStringA(func);
    OutputDebugStringA("<<<\n\n");
    _exit(err);
    TerminateProcess(GetCurrentProcess(), err);

    return err;
}

#if defined(_DEBUG) && (_DEBUG > 1)

static void *xmmalloc_dbg(const char *fn, const char *fd, size_t size)
{
    void *p;

    p = malloc(size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    dbgmemtrace(fn, fd ? fd : "malloc", NULL, p, size);
    InterlockedIncrement(&xncmmalloc);
    return p;
}

static void *xmcalloc_dbg(const char *fn, const char *fd, size_t number, size_t size)
{
    void *p;

    p = calloc(number, size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    dbgmemtrace(fn, fd ? fd : "calloc", NULL, p, number * size);
    InterlockedIncrement(&xncmcalloc);
    return p;
}

static void *xrealloc_dbg(const char *fn, void *mem, size_t size)
{
    void *p;

    p = realloc(mem, size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    dbgmemtrace(fn, "realloc", mem, p, size);
    if (mem == NULL)
        InterlockedIncrement(&xnnrealloc);
    InterlockedIncrement(&xncrealloc);
    return p;
}

static wchar_t *xwmalloc_dbg(const char *fn, const char *fd, size_t size)
{
    wchar_t *p = (wchar_t *)xmmalloc_dbg(fn, fd ? fd : "xwmalloc", size * sizeof(wchar_t));

    p[size - 1] = WNUL;
    return p;
}

static wchar_t *xwcalloc_dbg(const char *fn, size_t size)
{
    return (wchar_t *)xmcalloc_dbg(fn, "xwcalloc", size, sizeof(wchar_t));
}

static void xfree_dbg(const char *fn, void *mem)
{
    if (mem) {
        if (dbguntrace(fn, "xfree", mem)) {
            InterlockedIncrement(&xncfree);
            free(mem);
        }
    }
#if (_DEBUG > 2)
    else {
        dbgprints(fn, "xfree");
    }
#endif
}

static wchar_t *xwcsdup_dbg(const char *fn, const wchar_t *s)
{
    wchar_t *p;
    size_t   n;

    if (IS_EMPTY_WCS(s))
        return NULL;
    n = wcslen(s);
    p = xwmalloc_dbg(fn, "xwcsdup", n + 1);
    return wmemcpy(p, s, n);
}

#else

static void *xmmalloc(size_t size)
{
    void *p;

    p = malloc(size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    return p;
}

static void *xmcalloc(size_t number, size_t size)
{
    void *p;

    p = calloc(number, size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    return p;
}

static void *xrealloc(void *mem, size_t size)
{
    void *p;

    p = realloc(mem, size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    return p;
}

static wchar_t *xwmalloc(size_t size)
{
    wchar_t *p = (wchar_t *)xmmalloc(size * sizeof(wchar_t));

    p[size - 1] = WNUL;
    return p;
}

static __inline wchar_t *xwcalloc(size_t size)
{
    return (wchar_t *)xmcalloc(size, sizeof(wchar_t));
}

static __inline void xfree(void *mem)
{
    if (mem)
        free(mem);
}

static __inline wchar_t *xwcsdup(const wchar_t *s)
{
    if (IS_EMPTY_WCS(s))
        return NULL;
    return _wcsdup(s);
}

#endif

static __inline void xmemzero(void *mem, size_t number, size_t size)
{
    memset(mem, 0, number * size);
}

static __inline int xwcslen(const wchar_t *s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    else
        return (int)wcslen(s);
}

static __inline int xstrlen(const char *s)
{
    if (IS_EMPTY_STR(s))
        return 0;
    else
        return (int)strlen(s);
}

static char *xwcstombs(int cp, char *dst, int siz, const wchar_t *src)
{
    int r = 0;
    int n = siz - 1;

    ASSERT_NULL(dst, NULL);
    *dst = 0;
    ASSERT_NULL(src, dst);

    if (!*src)
        return dst;

    if (cp) {
        r = WideCharToMultiByte(cp, 0, src, -1, dst, siz, NULL, NULL);
        dst[n] = 0;
    }
    if (r == 0) {
        for (r = 0; *src; src++, r++) {
            if (r == n)
                break;
            dst[r] = *src < 128 ? (char)*src : '?';
        }
        dst[r] = 0;
    }

    return dst;
}

static wchar_t *xmbstowcs(int cp, wchar_t *dst, int siz, const char *src)
{
    int r = 0;
    int n = siz - 1;

    ASSERT_NULL(dst, NULL);
    *dst = 0;
    ASSERT_NULL(src, dst);

    if (!*src)
        return dst;
    if (cp) {
        r = MultiByteToWideChar(cp, 0, src, -1, dst, siz);
        dst[n] = 0;
    }
    if (r == 0) {
        for (r = 0; *src; src++, r++) {
            if (r == n)
                break;
            dst[r] = (BYTE)*src < 128 ? (wchar_t)*src : L'?';
        }
        dst[r] = 0;
    }
    return dst;
}

static void xwchreplace(wchar_t *s, wchar_t c, wchar_t r)
{
    wchar_t *d;

    for (d = s; *s; s++, d++) {
        if (*s == c) {
            if (*(s + 1) == c)
                *d = *(s++);
            else
                *d = r;
        }
        else {
            *d = *s;
        }
    }
    *d = WNUL;
}

static wchar_t *xgetenv(const wchar_t *s)
{
    DWORD    n;
    wchar_t  e[BBUFSIZ];
    wchar_t *d = NULL;

    n = GetEnvironmentVariableW(s, e, BBUFSIZ);
    if (n != 0) {
        d = xwmalloc(n + 1);
        DBG_WCSTAG(d, s, NULL);
        if (n >= BBUFSIZ) {
            n = GetEnvironmentVariableW(s, d, n);
            if (n == 0) {
                xfree(d);
                return NULL;
            }
        }
        else {
            wmemcpy(d, e, n);
        }
    }
    return d;
}

/**
 * Simple atoi with range between 0 and 2147483639.
 * Leading white space characters are ignored.
 * Returns negative value on error.
 */
static int xwcstoi(const wchar_t *sp, wchar_t **ep)
{
    int rv = 0;
    int dc = 0;

    ASSERT_WSTR(sp, -1);
    while(iswspace(*sp))
        sp++;

    while(iswdigit(*sp)) {
        int dv = *sp - L'0';

        if (dv || rv) {
            rv *= 10;
            rv += dv;
        }
        if (rv < 0) {
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
        *ep = (wchar_t *)sp;
    return rv;
}


/**
 * Appends src to string dst of size siz where
 * siz is the full size of dst.
 * At most siz-1 characters will be copied.
 *
 * Always NUL terminates (unless siz == 0).
 * Returns wcslen(initial dst) + wcslen(src),
 * not counting terminating NUL.
 *
 * If retval >= siz, no data will be appended to dst.
 * In that case call the function again with dst
 * string buffer resized to at least retval+1.
 */
static int xwcslcat(wchar_t *dst, int siz, const wchar_t *src)
{
    const wchar_t *s = src;
    wchar_t *p;
    wchar_t *d = dst;
    size_t   z = siz;
    size_t   n = siz;
    size_t   c;
    size_t   r;

    ASSERT_WSTR(src, 0);
    while ((n-- != 0) && (*d != WNUL))
        d++;
    c = d - dst;
    n = z - c;

    if (n-- == 0)
        return (int)(c + wcslen(s));
    p = dst + c;
    while (*s) {
        if (n != 0) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    r = c + (s - src);
    if (r >= z)
        *p = WNUL;
    else
        *d = WNUL;

    return (int)r;
}

static int xwcslcpy(wchar_t *dst, int siz, const wchar_t *src)
{
    const wchar_t *s = src;
    wchar_t *d = dst;
    int      n = siz;

    ASSERT_WSTR(s, 0);
    while (*s) {
        if (n != 1) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    *d = WNUL;

    return (int)(s - src);
}

static int xvsnwprintf(wchar_t *dst, int siz,
                       const wchar_t *fmt, va_list ap)
{
    int  c = siz - 1;
    int  n;

    ASSERT_WSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 4, 0);

    n = _vsnwprintf(dst, c, fmt, ap);
    if (n < 0 || n > c)
        n = c;
    dst[n] = WNUL;
    return n;
}

static int xsnwprintf(wchar_t *dst, int siz, const wchar_t *fmt, ...)
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
                      const char *fmt, va_list ap)
{
    int c = siz - 1;
    int n;

    ASSERT_CSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 4, 0);

    n = _vsnprintf(dst, c, fmt, ap);
    if (n < 0 || n > c)
        n = c;
    dst[n] = '\0';
    return n;
}

static int xsnprintf(char *dst, int siz, const char *fmt, ...)
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

static wchar_t *xwcsconcat(const wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp;
    int l1 = xwcslen(s1);
    int l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;

    cp = xwmalloc(l1 + l2 + 1);
    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    if(l2 > 0)
        wmemcpy(cp + l1, s2, l2);

    return cp;
}

static wchar_t *xwcsmkpath(const wchar_t *ds, const wchar_t *fs)
{
    wchar_t *cp;
    int nd = xwcslen(ds);
    int nf = xwcslen(fs);

    if ((nd == 0) || (nf == 0))
        return NULL;

    if ((fs[0] == L'.') && ((fs[1] == L'\\') || (fs[1] == L'/'))) {
        /**
         * Remove leading './' or '.\'
         */
        fs += 2;
        nf -= 2;
    }
    cp = xwmalloc(nd + nf + 2);

    wmemcpy(cp, ds, nd);
    cp[nd++] = L'\\';
    wmemcpy(cp + nd, fs, nf);

    return cp;
}

static wchar_t *xappendarg(int nq, wchar_t *s1, const wchar_t *s2, const wchar_t *s3)
{
    const wchar_t *c;
    wchar_t *e;
    wchar_t *d;

    int l1, l2, l3, nn;

    l3 = xwcslen(s3);
    if (l3 == 0)
        return s1;

    if (nq) {
        if (wcspbrk(s3, L" \t\"")) {
            int n = 2;
            for (c = s3; ; c++) {
                int b = 0;

                while (*c == L'\\') {
                    b++;
                    c++;
                }

                if (*c == WNUL) {
                    n += b * 2;
                    break;
                }
                else if (*c == L'"') {
                    n += b * 2 + 1;
                    n += 1;
                }
                else {
                    n += b;
                    n += 1;
                }
            }
            l3 = n;
        }
        else {
            nq = 0;
        }
    }
    nn = l3 + 1;
    l1 = xwcslen(s1);
    l2 = xwcslen(s2);
    if (l1)
        nn += (l1 + 1);
    if (l2)
        nn += (l2 + 1);
    e  = (wchar_t *)xrealloc(s1, nn * sizeof(wchar_t));
    d  = e;

    if(l1) {
        d += l1;
        *(d++) = L' ';
    }
    if(l2) {
        wmemcpy(d, s2, l2);
        d += l2;
        *(d++) = L' ';
    }
    if (nq) {
        *(d++) = L'"';
        for (c = s3; ; c++) {
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
            else if (*c == L'"') {
                wmemset(d, L'\\', b * 2 + 1);
                d += b * 2 + 1;
                *(d++) = *c;
            }
            else {
                if (b) {
                    wmemset(d, L'\\', b);
                    d += b;
                }
                *(d++) = *c;
            }
        }
        *(d++) = L'"';
    }
    else {
        wmemcpy(d, s3, l3);
        d += l3;
    }
    *d = WNUL;
    return e;
}

static int xwstartswith(const wchar_t *str, const wchar_t *src)
{
    while (*str) {
        if (towupper(*str) != *src)
            break;
        str++;
        src++;
        if (!*src)
            return 1;
    }
    return 0;
}


static int xwgetopt(int nargc, const wchar_t **nargv, const wchar_t *opts)
{
    static const wchar_t *place = zerostring;
    const wchar_t *oli = NULL;

    xwoptarg = NULL;
    if (*place == WNUL) {

        if (xwoptind >= nargc) {
            /* No more arguments */
            place = zerostring;
            return EOF;
        }
        place = nargv[xwoptind];
        xwoption = *(place++);
        if ((xwoption != L'-') && (xwoption != L'/')) {
            /* Argument is not an option */
            place = zerostring;
            return EOF;
        }
    }
    xwoption = *(place++);

    if (xwoption != L':') {
        oli = wcschr(opts, towlower(xwoption));
    }
    if (oli == NULL) {
        place = zerostring;
        return EINVAL;
    }

    /* Does this option need an argument? */
    if (oli[1] == L':') {
        /*
         * Option-argument is either the rest of this argument
         * or the entire next argument.
         */
        if (*place) {
            /* Skip blanks */
            while (*place == L' ')
                ++place;
        }
        if (*place) {
            xwoptarg = place;
        }
        else if (nargc > ++xwoptind) {
            xwoptarg = nargv[xwoptind];
        }
        ++xwoptind;
        place = zerostring;
        if (IS_EMPTY_WCS(xwoptarg)) {
            /* Option-argument is absent or empty */
            return ENOENT;
        }
    }
    else {
        /* Don't need argument */
        if (*place == WNUL) {
            ++xwoptind;
            place = zerostring;
        }
    }
    return oli[0];
}

static wchar_t *xuuidstring(wchar_t *b)
{
    static wchar_t s[TBUFSIZ];
    unsigned char  d[20];
    const wchar_t  xb16[] = L"0123456789abcdef";
    int i, x;

    if (BCryptGenRandom(NULL, d + 2, 16,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return NULL;

    if (b == NULL)
        b = s;
    d[0] = HIBYTE(svcmainproc->pInfo.dwProcessId);
    d[1] = LOBYTE(svcmainproc->pInfo.dwProcessId);
    for (i = 0, x = 0; i < 18; i++) {
        if (i == 2 || i == 6 || i == 8 || i == 10 || i == 12)
            b[x++] = '-';
        b[x++] = xb16[d[i] >> 4];
        b[x++] = xb16[d[i] & 0x0F];
    }
    b[x] = WNUL;
    return b;
}

static void xmktimedext(wchar_t *buf, int siz)
{
    SYSTEMTIME st;

    if (uselocaltime)
        GetLocalTime(&st);
    else
        GetSystemTime(&st);
    xsnwprintf(buf, siz, L".%.4d%.2d%.2d%.2d%.2d%.2d",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
}

static int xtimehdr(char *wb, int sz)
{
    LARGE_INTEGER ct = {{ 0, 0 }};
    LARGE_INTEGER et = {{ 0, 0 }};
    DWORD   ss, us, mm, hh;

    QueryPerformanceCounter(&ct);
    et.QuadPart = ct.QuadPart - pcstarttime.QuadPart;

    /**
     * Convert to microseconds
     */
    et.QuadPart *= CPP_INT64_C(1000000);
    et.QuadPart /= pcfrequency.QuadPart;
    ct.QuadPart  = et.QuadPart / CPP_INT64_C(1000);

    us = (DWORD)((et.QuadPart % CPP_INT64_C(1000000)));
    ss = (DWORD)((ct.QuadPart / MS_IN_SECOND) % CPP_INT64_C(60));
    mm = (DWORD)((ct.QuadPart / MS_IN_MINUTE) % CPP_INT64_C(60));
    hh = (DWORD)((ct.QuadPart / MS_IN_HOUR));

    return xsnprintf(wb, sz, "[%.2lu:%.2lu:%.2lu.%.6lu] ",
                     hh, mm, ss, us);
}

#if defined(_DEBUG)
/**
 * Runtime debugging functions
 */

static char dbgprocpad[4] = { 0, 0, 0, 0};
static char dbgsvcmode    = 'x';

static void dbginit(void)
{
    DWORD i;

    i = GetCurrentProcessId();
    if (i < 1000) {
        dbgprocpad[0] = ' ';
        if (i < 100)
            dbgprocpad[1] = ' ';
        if (i < 10)
            dbgprocpad[2] = ' ';
    }
#if (_DEBUG > 1)
    InitializeCriticalSection(&dbgmemlock);
    xmemzero(dbgmemory, TBUFSIZ, sizeof(dbgmemslot));
#endif

}

static void dbgprints(const char *funcname, const char *string)
{
    char b[MBUFSIZ];

    xsnprintf(b, MBUFSIZ, "%s[%.4lu] %c %-16s %s", dbgprocpad,
              GetCurrentThreadId(), dbgsvcmode, funcname, string);
    OutputDebugStringA(b);
}

static void dbgprintf(const char *funcname, const char *format, ...)
{
    int     n;
    char    b[MBUFSIZ];
    va_list ap;

    n = xsnprintf(b, MBUFSIZ, "%s[%.4lu] %c %-16s ", dbgprocpad,
                  GetCurrentThreadId(), dbgsvcmode, funcname);

    va_start(ap, format);
    xvsnprintf(b + n, MBUFSIZ - n, format, ap);
    va_end(ap);
    OutputDebugStringA(b);
}

static void xiphandler(const wchar_t *e,
                       const wchar_t *w, const wchar_t *f,
                       unsigned int n, uintptr_t r)
{
    dbgprints(__FUNCTION__,
              "invalid parameter handler called");
}

#endif

static BOOL xwinapierror(wchar_t *buf, int siz, DWORD err)
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
        } while ((n > 0) && ((buf[n] == L'.') || iswspace(buf[n])));

        while (n-- > 0) {
            if (iswspace(buf[n]))
                buf[n] = L' ';
        }
        return TRUE;
    }
    else {
        xsnwprintf(buf, siz,
                   L"Unknown system error code: %u", err);
        return FALSE;
    }
}

static BOOL setupeventlog(void)
{
    static BOOL ssrv = FALSE;
    static volatile LONG eset   = 0;
    static const wchar_t emsg[] = L"%SystemRoot%\\System32\\netmsg.dll\0";
    DWORD c;
    HKEY  k;

    if (InterlockedIncrement(&eset) > 1)
        return ssrv;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Services\\" \
                        L"EventLog\\Application\\" CPP_WIDEN(SVCBATCH_NAME),
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

static DWORD svcsyserror(const char *fn, int line, WORD typ, DWORD ern, const wchar_t *err, const wchar_t *eds)
{
    wchar_t        hdr[MBUFSIZ];
    wchar_t        erb[MBUFSIZ];
    const wchar_t *errarg[10];
    int            c, i = 0;

    errarg[i++] = wnamestamp;
    if (mainservice->lpName)
        errarg[i++] = mainservice->lpName;
    xwcslcpy(hdr, MBUFSIZ, CRLFW);
    if (typ != EVENTLOG_INFORMATION_TYPE) {
        if (typ == EVENTLOG_ERROR_TYPE)
            errarg[i++] = L"\r\nreported the following error:";
        xsnwprintf(hdr + 2, MBUFSIZ - 2,
                   L"svcbatch.c(%.4d, %S) ", line, fn);
    }
    xwcslcat(hdr, MBUFSIZ, err);
    if (eds) {
        if (err)
            xwcslcat(hdr, MBUFSIZ, L": ");
        xwcslcat(hdr, MBUFSIZ, eds);
    }
    errarg[i++] = hdr;
    if (ern == 0) {
        ern = ERROR_INVALID_PARAMETER;
#if defined(_DEBUG)
        dbgprintf(fn, "%S", hdr + 2);
#endif
    }
    else {
        c = xsnwprintf(erb, TBUFSIZ, L"\r\nerror(%lu) ", ern);
        xwinapierror(erb + c, MBUFSIZ - c, ern);
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

static void xclearprocess(LPSVCBATCH_PROCESS p)
{
    DWORD i;

    InterlockedExchange(&p->dwCurrentState, SVCBATCH_PROCESS_STOPPED);
    SAFE_CLOSE_HANDLE(p->pInfo.hProcess);
    SAFE_CLOSE_HANDLE(p->pInfo.hThread);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdError);
    SAFE_MEM_FREE(p->lpCommandLine);
    SAFE_MEM_FREE(p->lpExe);
    for (i = 0; i < p->nArgc; i++) {
        SAFE_MEM_FREE(p->lpArgv[i]);
    }
    p->nArgc = 0;
    DBG_PRINTF("%.4lu %lu", p->pInfo.dwProcessId, p->dwExitCode);
}

static DWORD waitprocess(LPSVCBATCH_PROCESS p, DWORD w)
{
    ASSERT_NULL(p, ERROR_INVALID_PARAMETER);

    if (InterlockedCompareExchange(&p->dwCurrentState, 0, 0) > SVCBATCH_PROCESS_STOPPED) {
        if (w > 0)
            WaitForSingleObject(p->pInfo.hProcess, w);
        GetExitCodeProcess(p->pInfo.hProcess, &p->dwExitCode);
    }
    return p->dwExitCode;
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
        if (r == RBUFSIZ) {
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
    HANDLE pa[RBUFSIZ];

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

static void killprocess(LPSVCBATCH_PROCESS proc, int rc, DWORD ws, DWORD rv)
{

    DBG_PRINTF("proc %.4lu", proc->pInfo.dwProcessId);
    if (InterlockedCompareExchange(&proc->dwCurrentState, 0, 0) == SVCBATCH_PROCESS_STOPPED)
        goto finished;
    InterlockedExchange(&proc->dwCurrentState, SVCBATCH_PROCESS_STOPPING);

    if (killproctree(proc->pInfo.dwProcessId, rc, rv)) {
        if (waitprocess(proc, ws) != STILL_ACTIVE)
            goto finished;
    }
    DBG_PRINTF("kill %.4lu", proc->pInfo.dwProcessId);
    proc->dwExitCode = rv;
    TerminateProcess(proc->pInfo.hProcess, proc->dwExitCode);

finished:
    InterlockedExchange(&proc->dwCurrentState, SVCBATCH_PROCESS_STOPPED);
    DBG_PRINTF("done %.4lu", proc->pInfo.dwProcessId);
}

static DWORD xmdparent(wchar_t *path)
{
    DWORD rc = 0;
    wchar_t *s;

    s = wcsrchr(path, L'\\');
    if (s == NULL)
        return ERROR_BAD_PATHNAME;
    *s = WNUL;
    if (CreateDirectoryW(path, NULL))
        return 0;
    else
        rc = GetLastError();
    if (rc == ERROR_PATH_NOT_FOUND) {
        /**
         * One or more intermediate directories do not exist
         */
        rc = xmdparent(path);
    }
    return rc;
}

static DWORD xcreatedir(const wchar_t *path)
{
    DWORD rc = 0;

    if (CreateDirectoryW(path, NULL))
        return 0;
    else
        rc = GetLastError();
    if (rc == ERROR_PATH_NOT_FOUND) {
        wchar_t pp[SVCBATCH_PATH_MAX];
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
    p->dwDuration = GetTickCount64();
#endif
    p->dwThreadId = GetCurrentThreadId();
    p->dwExitCode = (*p->lpStartAddress)(p->lpParameter);
#if defined(_DEBUG)
    p->dwDuration = GetTickCount64() - p->dwDuration;
#endif
    InterlockedExchange(&p->nStarted, 0);
    ExitThread(p->dwExitCode);

    return p->dwExitCode;
}

static BOOL xcreatethread(SVCBATCH_THREAD_ID id,
                          int suspended,
                          LPTHREAD_START_ROUTINE threadfn,
                          LPVOID param)
{
    if (InterlockedCompareExchange(&svcthread[id].nStarted, 1, 0)) {
        /**
         * Already started
         */
         SetLastError(ERROR_BUSY);
         return FALSE;
    }
    svcthread[id].lpStartAddress = threadfn;
    svcthread[id].lpParameter    = param;

    svcthread[id].hThread = CreateThread(NULL, 0, xrunthread, &svcthread[id],
                                         suspended ? CREATE_SUSPENDED : 0, NULL);
    if (svcthread[id].hThread == NULL) {
        InterlockedExchange(&svcthread[id].nStarted, 0);
        return FALSE;
    }
    return TRUE;
}

static BOOL isabsolutepath(const wchar_t *p)
{
    if ((p != NULL) && (p[0] < 128)) {
        if ((p[0] == L'\\') || (isalpha(p[0]) && (p[1] == L':')))
            return TRUE;
    }
    return FALSE;
}

static BOOL isrelativepath(const wchar_t *p)
{
    return !isabsolutepath(p);
}

static DWORD fixshortpath(wchar_t *buf, DWORD len)
{
    if ((len > 5) && (len < _MAX_FNAME)) {
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

static wchar_t *xgetfullpath(const wchar_t *path, wchar_t *dst, DWORD siz)
{
    DWORD len;
    ASSERT_WSTR(path, NULL);

    len = GetFullPathNameW(path, siz, dst, NULL);
    if ((len == 0) || (len >= siz))
        return NULL;
    fixshortpath(dst, len);
    return dst;
}

static wchar_t *xgetfinalpath(const wchar_t *path, int isdir,
                              wchar_t *dst, DWORD siz)
{
    HANDLE   fh;
    wchar_t  sbb[SVCBATCH_PATH_MAX];
    wchar_t *buf = dst;
    DWORD    len;
    DWORD    atr = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

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
    if (dst == NULL) {
        dst = xwcsdup(buf);
        DBG_STRTAG_FN(dst, "FinalPath");
    }
    return dst;
}

static BOOL resolvebatchname(const wchar_t *bp)
{
    wchar_t *p;

    if (svcxcmdproc->lpArgv[0])
        return TRUE;
    svcxcmdproc->lpArgv[0] = xgetfinalpath(bp, 0, NULL, 0);
    if (IS_EMPTY_WCS(svcxcmdproc->lpArgv[0]))
        return FALSE;
    DBG_STRTAG_FN(svcxcmdproc->lpArgv[0], "BatchFileName");
    p = wcsrchr(svcxcmdproc->lpArgv[0], L'\\');
    if (p) {
        *p = WNUL;
        mainservice->lpBase = xwcsdup(svcxcmdproc->lpArgv[0]);
        *p = L'\\';
        DBG_MEMTAG(mainservice->lpBase, "SVCBATCH_SERVICE_BASE");
        return TRUE;
    }
    else {
        SAFE_MEM_FREE(svcxcmdproc->lpArgv[0]);
        mainservice->lpBase = NULL;
        return FALSE;
    }
}

static void setsvcstatusexit(DWORD e)
{
    SVCBATCH_CS_ENTER(mainservice);
    mainservice->sStatus.dwServiceSpecificExitCode = e;
    SVCBATCH_CS_LEAVE(mainservice);
}

static void reportsvcstatus(DWORD status, DWORD param)
{
    static DWORD cpcnt = 1;

    if (svcmainproc->dwType != SVCBATCH_SERVICE_PROCESS)
        return;
    SVCBATCH_CS_ENTER(mainservice);
    if (InterlockedExchange(&mainservice->dwCurrentState, SERVICE_STOPPED) == SERVICE_STOPPED)
        goto finished;
    mainservice->sStatus.dwControlsAccepted = 0;
    mainservice->sStatus.dwCheckPoint       = 0;
    mainservice->sStatus.dwWaitHint         = 0;

    if (status == SERVICE_RUNNING) {
        mainservice->sStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                                  SERVICE_ACCEPT_SHUTDOWN |
                                                  preshutdown;
        cpcnt = 1;
    }
    else if (status == SERVICE_STOPPED) {
        if (param != 0)
            mainservice->sStatus.dwServiceSpecificExitCode = param;
        if (mainservice->sStatus.dwServiceSpecificExitCode == 0 &&
            mainservice->sStatus.dwCurrentState != SERVICE_STOP_PENDING) {
            mainservice->sStatus.dwServiceSpecificExitCode = ERROR_PROCESS_ABORTED;
            xsyserror(0, L"Service stopped without SERVICE_CONTROL_STOP signal", NULL);
        }
        if (mainservice->sStatus.dwServiceSpecificExitCode != 0)
            mainservice->sStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }
    else {
        mainservice->sStatus.dwCheckPoint = cpcnt++;
        mainservice->sStatus.dwWaitHint   = param;
    }
    mainservice->sStatus.dwCurrentState = status;
    InterlockedExchange(&mainservice->dwCurrentState, status);
    if (!SetServiceStatus(mainservice->hStatus, &mainservice->sStatus)) {
        xsyserror(GetLastError(), L"SetServiceStatus", NULL);
        InterlockedExchange(&mainservice->dwCurrentState, SERVICE_STOPPED);
    }
finished:
    SVCBATCH_CS_LEAVE(mainservice);
}

static BOOL createiopipe(LPHANDLE rd, LPHANDLE wr, DWORD mode)
{
    wchar_t  name[TBUFSIZ];
    SECURITY_ATTRIBUTES sa;

    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    xwcslcpy(name, TBUFSIZ, SVCBATCH_PIPEPFX);
    xuuidstring(name + _countof(SVCBATCH_PIPEPFX) - 1);

    *rd = CreateNamedPipeW(name,
                           PIPE_ACCESS_INBOUND | mode,
                           PIPE_TYPE_BYTE,
                           1,
                           0,
                           HBUFSIZ,
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

    DBG_PRINTF("%c %S", mode ? '0' : '1', name);
    return TRUE;
}

static BOOL createnulpipe(LPHANDLE ph, DWORD mode)
{
    SECURITY_ATTRIBUTES sa;

    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    *ph = CreateFileW(L"NUL",
                      mode,
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
    HANDLE cp = GetCurrentProcess();
    HANDLE rd;
    HANDLE wr;

    xmemzero(si, 1, sizeof(STARTUPINFOW));
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
        if (!createnulpipe(&rd, GENERIC_READ))
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
        if (!createnulpipe(&wr, GENERIC_WRITE))
            return GetLastError();
    }
    si->hStdOutput = wr;
    si->hStdError  = wr;

    return 0;
}

static BOOL xseekfend(HANDLE h)
{
    LARGE_INTEGER ee = {{ 0, 0 }};
    return SetFilePointerEx(h, ee, NULL, FILE_END);
}

static DWORD logappend(HANDLE h, LPCVOID buf, DWORD len)
{
    DWORD wr;

    if (xseekfend(h)) {
        if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0)) {
            InterlockedAdd64(&svcbatchlog->nWritten, wr);
            return 0;
        }
    }
    return GetLastError();
}

static BOOL logfflush(HANDLE h)
{
    DWORD wr;

    if (xseekfend(h)) {
        if (WriteFile(h, CRLFA, 2, &wr, NULL) && (wr != 0)) {
            FlushFileBuffers(h);
            return xseekfend(h);
        }
    }
    return FALSE;
}

static void logwrline(HANDLE h, const char *sb)
{
    char    wb[TBUFSIZ];
    DWORD   wr;
    DWORD   nw;

    nw = xtimehdr(wb, TBUFSIZ);

    if (nw > 0) {
        WriteFile(h, wb, nw, &wr, NULL);
        nw = xstrlen(sb);
        WriteFile(h, sb, nw, &wr, NULL);
        WriteFile(h, CRLFA, 2, &wr, NULL);
    }
}

static void logprintf(HANDLE h, const char *format, ...)
{
    int     c;
    char    buf[FBUFSIZ];
    va_list ap;

    va_start(ap, format);
    c = xvsnprintf(buf, FBUFSIZ, format, ap);
    va_end(ap);
    if (c > 0)
        logwrline(h, buf);
}

static void logwransi(HANDLE h, const char *hdr, const wchar_t *wcs)
{
    int     n;
    char    buf[FBUFSIZ];

    n = xstrlen(hdr);
    memcpy(buf, hdr, n);
    xwcstombs(svccodepage, buf + n, FBUFSIZ - n, wcs);
    logwrline(h, buf);
}

static void logwrtime(HANDLE h, const char *hdr)
{
    SYSTEMTIME tt;

    if (uselocaltime)
        GetLocalTime(&tt);
    else
        GetSystemTime(&tt);
    logprintf(h, "%-16s : %.4d-%.2d-%.2d %.2d:%.2d:%.2d",
              hdr, tt.wYear, tt.wMonth, tt.wDay,
              tt.wHour, tt.wMinute, tt.wSecond);
}

static void logconfig(HANDLE h)
{
    logwransi(h, "Service name     : ", mainservice->lpName);
    logwransi(h, "Service uuid     : ", mainservice->lpUuid);
    logwransi(h, "Batch file       : ", svcxcmdproc->lpArgv[0]);
    if (svcstopproc)
        logwransi(h, "Shutdown batch   : ", svcstopproc->lpArgv[0]);
    logwransi(h, "Program directory: ", svcmainproc->lpDir);
    logwransi(h, "Base directory   : ", mainservice->lpBase);
    logwransi(h, "Home directory   : ", mainservice->lpHome);
    logwransi(h, "Logs directory   : ", svcbatchlog->szDir);
    logwransi(h, "Work directory   : ", mainservice->lpWork);

    logfflush(h);
}

static BOOL canrotatelogs(void)
{
    BOOL rv = FALSE;

    SVCBATCH_CS_ENTER(svcbatchlog);
    if (InterlockedCompareExchange(&svcbatchlog->dwCurrentState, 0, 0) == 0)
        rv = TRUE;
    SVCBATCH_CS_LEAVE(svcbatchlog);
    return rv;
}

static DWORD createlogsdir(void)
{
    wchar_t *p;
    wchar_t dp[SVCBATCH_PATH_MAX];

    if (xgetfullpath(outdirparam, dp, SVCBATCH_PATH_MAX) == NULL) {
        xsyserror(0, L"xgetfullpath", outdirparam);
        return ERROR_BAD_PATHNAME;
    }
    p = xgetfinalpath(dp, 1, svcbatchlog->szDir, SVCBATCH_PATH_MAX);
    if (p == NULL) {
        DWORD rc = GetLastError();

        if (rc > ERROR_PATH_NOT_FOUND)
            return xsyserror(rc, L"xgetfinalpath", dp);

        rc = xcreatedir(dp);
        if (rc != 0)
            return xsyserror(rc, L"xcreatedir", dp);
        p = xgetfinalpath(dp, 1, svcbatchlog->szDir, SVCBATCH_PATH_MAX);
        if (p == NULL)
            return xsyserror(GetLastError(), L"xgetfinalpath", dp);
    }
    if (_wcsicmp(svcbatchlog->szDir, mainservice->lpHome) == 0) {
        xsyserror(0, L"Logs directory cannot be the same as home directory",
                  svcbatchlog->szDir);
        return ERROR_INVALID_PARAMETER;
    }
    return 0;
}

static DWORD makelogfile(const wchar_t *logfn, BOOL ssp)
{
    wchar_t ewb[BBUFSIZ];
    struct  tm *ctm;
    time_t  ctt;
    DWORD   rc;
    HANDLE  h;
    int  i, x;

    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, 0);

    if (svcbatchlog->lpFileName && !truncatelogs) {
        SAFE_MEM_FREE(svcbatchlog->lpFileName);
    }
    if (svcbatchlog->lpFileName == NULL) {
        time(&ctt);
        if (uselocaltime)
            ctm = localtime(&ctt);
        else
            ctm = gmtime(&ctt);
        if (wcsftime(ewb, BBUFSIZ, logfn, ctm) == 0)
            return xsyserror(0, L"Invalid format code", logfn);

        for (i = 0, x = 0; ewb[i]; i++) {
            wchar_t c = ewb[i];

            if ((c > 127) || (xfnchartype[c] & 1))
                ewb[x++] = c;
        }
        ewb[x] = WNUL;
        svcbatchlog->lpFileName = xwcsmkpath(svcbatchlog->szDir, ewb);
        DBG_STRTAG_FN(svcbatchlog->lpFileName, "SVCBATCH_LOG");
    }

    h = CreateFileW(svcbatchlog->lpFileName,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(h)) {
        xsyserror(rc, L"CreateFile", svcbatchlog->lpFileName);
        return rc;
    }
#if defined(_DEBUG)
    if (rc == ERROR_ALREADY_EXISTS) {
        DBG_PRINTF("truncated %S", svcbatchlog->lpFileName);
    }
#endif
    if (haslogstatus) {
        logwrline(h, cnamestamp);
        if (ssp) {
            if (rc == ERROR_ALREADY_EXISTS)
                logwrtime(h, "Log truncated");
            else
                logwrtime(h, "Log created");
        }
    }
    InterlockedExchange64(&svcbatchlog->nWritten, 0);
    InterlockedExchangePointer(&svcbatchlog->hFile, h);
    return 0;
}

static DWORD openlogfile(BOOL ssp)
{
    wchar_t  logfn[BBUFSIZ];
    wchar_t *logpb    = NULL;
    HANDLE h          = NULL;
    BOOL   renameprev = FALSE;
    BOOL   rotateprev = FALSE;
    DWORD  rc;

    if (svclogfname)
        xwcslcpy(logfn, BBUFSIZ, svclogfname);
    else
        xwcslcpy(logfn, BBUFSIZ, SVCBATCH_LOGNAME);
    xwcslcat(logfn, BBUFSIZ, svclogfnext);
    if (wcschr(logfn, L'%'))
        return makelogfile(logfn, ssp);

    if (!truncatelogs) {
        renameprev = TRUE;
        if (svcmaxlogs > 0)
            rotateprev = TRUE;
    }
    if (svcbatchlog->lpFileName == NULL)
        svcbatchlog->lpFileName = xwcsmkpath(svcbatchlog->szDir, logfn);
    if (svcbatchlog->lpFileName == NULL)
        return xsyserror(ERROR_FILE_NOT_FOUND, logfn, NULL);
    DBG_STRTAG_FN(svcbatchlog->lpFileName, "SVCBATCH_LOG");
    if (renameprev) {
        if (GetFileAttributesW(svcbatchlog->lpFileName) != INVALID_FILE_ATTRIBUTES) {
            if (ssp)
                reportsvcstatus(SERVICE_START_PENDING, 0);
            if (rotateprev) {
                logpb = xwcsconcat(svcbatchlog->lpFileName, L".0");
            }
            else {
                wchar_t sb[TBUFSIZ];

                xmktimedext(sb, TBUFSIZ);
                logpb = xwcsconcat(svcbatchlog->lpFileName, sb);
            }
            DBG_STRTAG_FN(logpb, "SVCBATCH_LOG");
            if (!MoveFileExW(svcbatchlog->lpFileName, logpb, MOVEFILE_REPLACE_EXISTING)) {
                rc = GetLastError();
                xsyserror(rc, svcbatchlog->lpFileName, logpb);
                xfree(logpb);
                return rc;
            }
        }
        else {
            rc = GetLastError();
            if (rc != ERROR_FILE_NOT_FOUND) {
                return xsyserror(rc, svcbatchlog->lpFileName, NULL);
            }
        }
    }
    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, 0);
    if (rotateprev) {
        int i;
        /**
         * Rotate previous log files
         */
        for (i = svcmaxlogs; i > 0; i--) {
            wchar_t *logpn  = logpb;
            wchar_t  sfx[4] = { L'.', WNUL, WNUL, WNUL };

            sfx[1] = L'0' + i - 1;
            if (i > 1) {
                logpn = xwcsconcat(svcbatchlog->lpFileName, sfx);
                DBG_STRTAG_FN(logpn, NULL);
            }
            if (GetFileAttributesW(logpn) != INVALID_FILE_ATTRIBUTES) {
                wchar_t *lognn;
                BOOL     logmv = TRUE;

                if (i > 2) {
                    /**
                     * Check for gap
                     */
                    sfx[1] = L'0' + i - 2;
                    lognn = xwcsconcat(svcbatchlog->lpFileName, sfx);
                    if (GetFileAttributesW(lognn) == INVALID_FILE_ATTRIBUTES)
                        logmv = FALSE;
                    DBG_STRTAG_FN(lognn, NULL);
                    xfree(lognn);
                }
                if (logmv) {
                    sfx[1] = L'0' + i;
                    lognn = xwcsconcat(svcbatchlog->lpFileName, sfx);
                    DBG_STRTAG_FN(lognn, NULL);
                    if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING)) {
                        rc = GetLastError();
                        xsyserror(rc, logpn, lognn);
                        xfree(logpn);
                        xfree(lognn);
                        goto failed;
                    }
                    xfree(lognn);
                    if (ssp)
                        reportsvcstatus(SERVICE_START_PENDING, 0);
                }
            }
            if (logpn != logpb)
                xfree(logpn);
        }
    }

    h = CreateFileW(svcbatchlog->lpFileName,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(h)) {
        xsyserror(rc, svcbatchlog->lpFileName, NULL);
        goto failed;
    }
#if defined(_DEBUG)
    if (rc == ERROR_ALREADY_EXISTS) {
        DBG_PRINTF("truncated %S", svcbatchlog->lpFileName);
    }
#endif
    if (haslogstatus) {
        logwrline(h, cnamestamp);
        if (ssp) {
            if (rc == ERROR_ALREADY_EXISTS)
                logwrtime(h, "Log truncated");
            else
                logwrtime(h, "Log created");
        }
    }
    InterlockedExchange64(&svcbatchlog->nWritten, 0);
    InterlockedExchangePointer(&svcbatchlog->hFile, h);
    xfree(logpb);
    return 0;

failed:
    SAFE_CLOSE_HANDLE(h);
    if (logpb) {
        MoveFileExW(logpb, svcbatchlog->lpFileName, MOVEFILE_REPLACE_EXISTING);
        xfree(logpb);
    }
    return rc;
}

static DWORD rotatelogs(void)
{
    DWORD  rc = 0;
    LONG   nr;
    HANDLE h  = NULL;

    SVCBATCH_CS_ENTER(svcbatchlog);
    InterlockedExchange(&svcbatchlog->dwCurrentState, 1);

    if (InterlockedCompareExchange64(&svcbatchlog->nWritten, 0, 0) == 0) {
        DBG_PRINTS("nothing to rotate");
        goto finished;
    }
    h = InterlockedExchangePointer(&svcbatchlog->hFile, NULL);
    if (h == NULL) {
        InterlockedExchange64(&svcbatchlog->nWritten, 0);
        rc = ERROR_FILE_NOT_FOUND;
        goto finished;
    }
    nr = InterlockedIncrement(&svcbatchlog->nRotateCount);
    QueryPerformanceCounter(&pcstarttime);
    FlushFileBuffers(h);
    if (truncatelogs) {
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (SetFilePointerEx(h, ee, NULL, FILE_BEGIN)) {
            if (SetEndOfFile(h)) {
                if (haslogstatus) {
                    logwrline(h, cnamestamp);
                    logwrtime(h, "Log truncated");
                    logprintf(h, "Log generation   : %lu", nr);
                    logconfig(h);
                }
                DBG_PRINTS("truncated");
                InterlockedExchangePointer(&svcbatchlog->hFile, h);
                goto finished;
            }
        }
        rc = GetLastError();
        xsyserror(rc, svcbatchlog->lpFileName, NULL);
        CloseHandle(h);
    }
    else {
        CloseHandle(h);
        rc = openlogfile(FALSE);
    }
    if (rc == 0) {
        if (haslogstatus) {
            logwrtime(svcbatchlog->hFile, "Log rotated");
            logprintf(svcbatchlog->hFile, "Log generation   : %lu", nr);
            logconfig(svcbatchlog->hFile);
        }
        DBG_PRINTS("rotated");
    }
    else {
        setsvcstatusexit(rc);
    }

finished:
    InterlockedExchange64(&svcbatchlog->nWritten, 0);
    SVCBATCH_CS_LEAVE(svcbatchlog);
    return rc;
}

static void closelogfile(void)
{
    HANDLE h;

    DBG_PRINTS("started");
    SVCBATCH_CS_ENTER(svcbatchlog);
    InterlockedExchange(&svcbatchlog->dwCurrentState, 1);
    h = InterlockedExchangePointer(&svcbatchlog->hFile, NULL);
    if (h) {
        DBG_PRINTS("flushing");
        if (haslogstatus) {
            logfflush(h);
            logwrtime(h, "Log closed");
        }
        FlushFileBuffers(h);
        DBG_PRINTS("flushed");
        CloseHandle(h);
    }
    xfree(svcbatchlog->lpFileName);
    SVCBATCH_CS_LEAVE(svcbatchlog);
    DeleteCriticalSection(&svcbatchlog->csLock);
    SAFE_MEM_FREE(svcbatchlog);
    DBG_PRINTS("done");
}

static void resolvetimeout(int hh, int mm, int ss, int od)
{
    SYSTEMTIME     st;
    FILETIME       ft;
    ULARGE_INTEGER ui;

    rotateint = od ? ONE_DAY : ONE_HOUR;
    if (uselocaltime && od)
        GetLocalTime(&st);
    else
        GetSystemTime(&st);

    SystemTimeToFileTime(&st, &ft);
    ui.HighPart  = ft.dwHighDateTime;
    ui.LowPart   = ft.dwLowDateTime;
    ui.QuadPart += rotateint;
    ft.dwHighDateTime = ui.HighPart;
    ft.dwLowDateTime  = ui.LowPart;
    FileTimeToSystemTime(&ft, &st);
    if (od)
        st.wHour = hh;
    st.wMinute = mm;
    st.wSecond = ss;
    SystemTimeToFileTime(&st, &ft);
    rotatetmo.HighPart = ft.dwHighDateTime;
    rotatetmo.LowPart  = ft.dwLowDateTime;

    rotatebytime = TRUE;
}

static BOOL resolverotate(const wchar_t *rp)
{
    wchar_t *ep;

    ASSERT_WSTR(rp, FALSE);

    if (wcspbrk(rp, L"BKMG")) {
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
            DBG_PRINTF("rotate size %S is less then %dK",
                       rp, SVCBATCH_MIN_ROTATE_SIZ);
            rotatesiz.QuadPart = CPP_INT64_C(0);
            rotatebysize = FALSE;
        }
        else {
            DBG_PRINTF("rotate if larger then %S", rp);
            rotatesiz.QuadPart = siz;
            rotatebysize = TRUE;
        }
    }
    else {
        rotateint    = CPP_INT64_C(0);
        rotatebytime = FALSE;
        rotatetmo.QuadPart = CPP_INT64_C(0);

        if (wcschr(rp, L':')) {
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
                rotateint = mm * ONE_MINUTE * CPP_INT64_C(-1);
                DBG_PRINTF("rotate each %d minutes", mm);
                rotatetmo.QuadPart = rotateint;
                rotatebytime = TRUE;
            }
        }
    }
    return TRUE;
}

static DWORD runshutdown(DWORD rt)
{
    const wchar_t *exe;
    wchar_t  rp[TBUFSIZ];
    DWORD    rc = 0;
    DWORD i, ip = 4;

    DBG_PRINTS("started");

    rc = createiopipes(&svcstopproc->sInfo, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }
    exe = svcmainproc->lpExe;
    svcstopproc->lpCommandLine = xappendarg(1, NULL, NULL, exe);
    ip = xsnwprintf(rp, TBUFSIZ, L"@@ -k%d -", stoptimeout / 1000);
    if (havestoplogs) {
        i = ip;
        if (uselocaltime)
            rp[ip++] = L'l';
        if (truncatelogs)
            rp[ip++] = L't';
        if (haslogstatus)
            rp[ip++] = L'v';
        if (i == ip)
            rp[i - 2] = WNUL;
    }
    else {
        rp[ip++] = L'q';
    }
    rp[ip++] = WNUL;
    svcstopproc->lpCommandLine = xappendarg(0, svcstopproc->lpCommandLine, NULL,  rp);
    svcstopproc->lpCommandLine = xappendarg(0, svcstopproc->lpCommandLine, L"-c", codepageopt);
    if (svclogfname && havestoplogs)
        svcstopproc->lpCommandLine = xappendarg(1, svcstopproc->lpCommandLine, L"-n", svclogfname);
    for (i = 0; i < svcstopproc->nArgc; i++)
        svcstopproc->lpCommandLine = xappendarg(1, svcstopproc->lpCommandLine, NULL, svcstopproc->lpArgv[i]);
    DBG_PRINTF("cmdline %S", svcstopproc->lpCommandLine);
    DBG_MEMTAG(svcstopproc->lpCommandLine, "StopCommandLine");
    svcstopproc->dwCreationFlags = CREATE_UNICODE_ENVIRONMENT |
                                   CREATE_NEW_CONSOLE;
    if (!CreateProcessW(exe, svcstopproc->lpCommandLine,
                        NULL, NULL, TRUE,
                        svcstopproc->dwCreationFlags,
                        NULL, NULL,
                        &svcstopproc->sInfo,
                        &svcstopproc->pInfo)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", exe);
        goto finished;
    }
    SAFE_CLOSE_HANDLE(svcstopproc->pInfo.hThread);
    SAFE_CLOSE_HANDLE(svcstopproc->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(svcstopproc->sInfo.hStdError);

    DBG_PRINTF("waiting %lu ms for shutdown process %lu",
               stoptimeout + rt, svcstopproc->pInfo.dwProcessId);
    rc = WaitForSingleObject(svcstopproc->pInfo.hProcess, stoptimeout + rt);
    if (rc == WAIT_TIMEOUT) {
        DBG_PRINTS("killing shutdown process tree");
        killproctree(svcstopproc->pInfo.dwProcessId, killdepth, rc);
    }
    else {
        rc = ERROR_INVALID_FUNCTION;
        GetExitCodeProcess(svcstopproc->pInfo.hProcess, &rc);
    }
finished:
    svcstopproc->dwExitCode = rc;
    xclearprocess(svcstopproc);
    DBG_PRINTF("done %lu", rc);
    return rc;
}

static DWORD WINAPI stopthread(void *msg)
{
    DWORD rc = 0;
    DWORD ws = WAIT_OBJECT_1;

    ResetEvent(svcstopended);
    SetEvent(svcstopstart);

    if (msg == NULL)
        reportsvcstatus(SERVICE_STOP_PENDING, stoptimeout + SVCBATCH_STOP_HINT);
    DBG_PRINTS("started");
    if (svcbatchlog) {
        SVCBATCH_CS_ENTER(svcbatchlog);
        InterlockedExchange(&svcbatchlog->dwCurrentState, 1);

        if (haslogstatus && msg) {
            HANDLE h;
            h = InterlockedExchangePointer(&svcbatchlog->hFile, NULL);
            if (h) {
                logfflush(h);
                logprintf(h, "Service signaled : %s", (const char *)msg);
            }
            InterlockedExchangePointer(&svcbatchlog->hFile, h);
        }
        SVCBATCH_CS_LEAVE(svcbatchlog);
    }
    if (svcstopproc) {
        int   ri;
        ULONGLONG rs;

        DBG_PRINTS("creating shutdown process");
        rs = GetTickCount64();
        rc = runshutdown(SVCBATCH_STOP_WAIT);
        ri = (int)(GetTickCount64() - rs);
        DBG_PRINTF("shutdown finished in %d ms", ri);
        reportsvcstatus(SERVICE_STOP_PENDING, 0);
        ri = stoptimeout - ri;
        if (ri < SVCBATCH_STOP_SYNC)
            ri = SVCBATCH_STOP_SYNC;
        DBG_PRINTF("waiting %d ms for worker", ri);
        ws = WaitForSingleObject(workfinished, ri);
    }
    if (ws != WAIT_OBJECT_0) {
        reportsvcstatus(SERVICE_STOP_PENDING, 0);
        DBG_PRINTS("generating CTRL_C_EVENT");
        SetConsoleCtrlHandler(NULL, TRUE);
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        ws = WaitForSingleObject(workfinished, stoptimeout);
        SetConsoleCtrlHandler(NULL, FALSE);
    }

    reportsvcstatus(SERVICE_STOP_PENDING, 0);
    if (ws != WAIT_OBJECT_0) {
        DBG_PRINTS("worker process is still running ... terminating");
        killprocess(svcxcmdproc, killdepth, SVCBATCH_STOP_SYNC, WAIT_TIMEOUT);
    }
    else {
        DBG_PRINTS("worker process ended");
        killproctree(svcxcmdproc->pInfo.dwProcessId, killdepth, ERROR_ARENA_TRASHED);
    }
    reportsvcstatus(SERVICE_STOP_PENDING, 0);
    SetEvent(svcstopended);
    DBG_PRINTS("done");
    return rc;
}

static void createstopthread(DWORD rv)
{
    if (svcmainproc->dwType == SVCBATCH_SERVICE_PROCESS) {
        xcreatethread(SVCBATCH_STOP_THREAD, 0, stopthread, NULL);
    }
    if (rv)
        setsvcstatusexit(rv);
}

static void stopshutdown(DWORD rt)
{
    DWORD ws;

    DBG_PRINTS("started");
    SetConsoleCtrlHandler(NULL, TRUE);
    DBG_PRINTS("generating CTRL_C_EVENT");
    GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    ws = WaitForSingleObject(workfinished, rt);
    SetConsoleCtrlHandler(NULL, FALSE);

    if (ws != WAIT_OBJECT_0) {
        DBG_PRINTS("worker process is still running ... terminating");
        killprocess(svcxcmdproc, killdepth, SVCBATCH_STOP_SYNC, ws);
    }
    else {
        DBG_PRINTS("worker process ended");
        killproctree(svcxcmdproc->pInfo.dwProcessId, killdepth, ERROR_ARENA_TRASHED);
    }

    DBG_PRINTS("done");
}


static DWORD logwrdata(BYTE *buf, DWORD len)
{
    DWORD  rc;
    HANDLE h;

    SVCBATCH_CS_ENTER(svcbatchlog);
    h = InterlockedExchangePointer(&svcbatchlog->hFile, NULL);

    if (h)
        rc = logappend(h, buf, len);
    else
        rc = ERROR_NO_MORE_FILES;

    if (rc) {
        InterlockedExchangePointer(&svcbatchlog->hFile, h);
        SVCBATCH_CS_LEAVE(svcbatchlog);

        if (rc != ERROR_NO_MORE_FILES) {
            xsyserror(rc, L"Log write", NULL);
            createstopthread(rc);
        }
        return rc;
    }
    if (haslogrotate && rotatebysize) {
        if (InterlockedCompareExchange64(&svcbatchlog->nWritten, 0, 0) >= rotatesiz.QuadPart) {
            if (canrotatelogs()) {
                InterlockedExchange(&svcbatchlog->dwCurrentState, 1);
                DBG_PRINTS("rotating by size");
                SetEvent(logrotatesig);
            }
        }
    }
    InterlockedExchangePointer(&svcbatchlog->hFile, h);
    SVCBATCH_CS_LEAVE(svcbatchlog);
    return 0;
}

static DWORD WINAPI wrpipethread(void *wh)
{
    DWORD wr;
    DWORD rc = 0;

    DBG_PRINTS("started");
    if (WriteFile(wh, YYES, 3, &wr, NULL) && (wr != 0)) {
        if (!FlushFileBuffers(wh))
            rc = GetLastError();
    }
    else {
        rc = GetLastError();
    }
    CloseHandle(wh);
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

static DWORD WINAPI monitorthread(void *unused)
{
    HANDLE wh[3];
    BOOL   rc = TRUE;

    DBG_PRINTS("started");

    wh[0] = workfinished;
    wh[1] = svcstopstart;
    wh[2] = ctrlbreaksig;

    do {
        DWORD ws;

        ws = WaitForMultipleObjects(3, wh, FALSE, INFINITE);
        switch (ws) {
            case WAIT_OBJECT_0:
                DBG_PRINTS("workfinished signaled");
                rc = FALSE;
            break;
            case WAIT_OBJECT_1:
                DBG_PRINTS("svcstopstart signaled");
                rc = FALSE;
            break;
            case WAIT_OBJECT_2:
                DBG_PRINTS("service SVCBATCH_CTRL_BREAK signaled");
                if (haslogstatus) {
                    HANDLE h;

                    SVCBATCH_CS_ENTER(svcbatchlog);
                    h = InterlockedExchangePointer(&svcbatchlog->hFile, NULL);

                    if (h) {
                        logfflush(h);
                        logwrline(h, "Signaled SVCBATCH_CTRL_BREAK");
                    }
                    InterlockedExchangePointer(&svcbatchlog->hFile, h);
                    SVCBATCH_CS_LEAVE(svcbatchlog);
                }
                /**
                 * Danger Zone!!!
                 *
                 * Send CTRL_BREAK_EVENT to the child process.
                 * This is useful if batch file is running java
                 * CTRL_BREAK signal tells JDK to dump thread stack
                 *
                 */
                GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
                ResetEvent(ctrlbreaksig);
            break;
            default:
                rc = FALSE;
            break;
        }
    } while (rc);

    DBG_PRINTS("done");
    return 0;
}


static DWORD WINAPI rotatethread(void *unused)
{
    HANDLE wh[4];
    HANDLE wt = NULL;
    DWORD  rc = 0;
    DWORD  nw = 3;
    DWORD  rw = SVCBATCH_ROTATE_READY;

    DBG_PRINTF("started");

    wh[0] = workfinished;
    wh[1] = svcstopstart;
    wh[2] = logrotatesig;
    wh[3] = NULL;

    InterlockedExchange(&svcbatchlog->dwCurrentState, 1);
    if (rotatebytime) {
        wt = CreateWaitableTimer(NULL, TRUE, NULL);
        if (IS_INVALID_HANDLE(wt)) {
            rc = xsyserror(GetLastError(), L"CreateWaitableTimer", NULL);
            goto failed;
        }
        if (!SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE)) {
            rc = xsyserror(GetLastError(), L"SetWaitableTimer", NULL);
            goto failed;
        }
        wh[nw++] = wt;
    }

    while (rc == 0) {
        DWORD wc = WaitForMultipleObjects(nw, wh, FALSE, rw);

        switch (wc) {
            case WAIT_OBJECT_0:
                rc = 1;
                DBG_PRINTS("workfinished signaled");
            break;
            case WAIT_OBJECT_1:
                rc = 1;
                DBG_PRINTS("svcstopstart signaled");
            break;
            case WAIT_OBJECT_2:
                DBG_PRINTS("logrotatesig signaled");
                rc = rotatelogs();
                if (rc == 0) {
                    if (IS_VALID_HANDLE(wt) && (rotateint < 0)) {
                        CancelWaitableTimer(wt);
                        SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE);
                    }
                    ResetEvent(logrotatesig);
                    rw = SVCBATCH_ROTATE_READY;
                }
            break;
            case WAIT_OBJECT_3:
                DBG_PRINTS("rotatetimer signaled");
                if (canrotatelogs()) {
                    DBG_PRINTS("rotate by time");
                    rc = rotatelogs();
                    rw = SVCBATCH_ROTATE_READY;
                }
#if defined(_DEBUG)
                else {
                    DBG_PRINTS("rotate is busy ... canceling timer");
                }
#endif
                if (rc == 0) {
                    CancelWaitableTimer(wt);
                    if (rotateint > 0)
                        rotatetmo.QuadPart += rotateint;
                    SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE);
                    ResetEvent(logrotatesig);
                }
            break;
            case WAIT_TIMEOUT:
                DBG_PRINTS("rotate ready");
                SVCBATCH_CS_ENTER(svcbatchlog);
                InterlockedExchange(&svcbatchlog->dwCurrentState, 0);
                SVCBATCH_CS_LEAVE(svcbatchlog);
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
    if (WaitForSingleObject(workfinished, SVCBATCH_STOP_SYNC) == WAIT_TIMEOUT)
        createstopthread(rc);

finished:
    SAFE_CLOSE_HANDLE(wt);
    DBG_PRINTS("done");
    return rc > 1 ? rc : 0;
}

static DWORD WINAPI workerthread(void *unused)
{
    DWORD i;
    HANDLE   wr = NULL;
    HANDLE   rd = NULL;
    LPHANDLE rp = NULL;
    DWORD    rc = 0;
    SVCBATCH_PIPE op;

    DBG_PRINTS("started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    InterlockedExchange(&svcxcmdproc->dwCurrentState, SVCBATCH_PROCESS_STARTING);

    xmemzero(&op, 1, sizeof(SVCBATCH_PIPE));
    if (svcbatchlog)
        rp = &rd;
    rc = createiopipes(&svcxcmdproc->sInfo, &wr, rp, FILE_FLAG_OVERLAPPED);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        setsvcstatusexit(rc);
        svcxcmdproc->dwExitCode = rc;
        goto finished;
    }
    svcxcmdproc->lpCommandLine = xappendarg(1, NULL, NULL, svcxcmdproc->lpExe);
    svcxcmdproc->lpCommandLine = xappendarg(1, svcxcmdproc->lpCommandLine, L"/D /C", svcxcmdproc->lpArgv[0]);
    for (i = 1; i < svcxcmdproc->nArgc; i++) {
        xwchreplace(svcxcmdproc->lpArgv[i], L'@', L'%');
        svcxcmdproc->lpCommandLine = xappendarg(1, svcxcmdproc->lpCommandLine, NULL, svcxcmdproc->lpArgv[i]);
    }
    DBG_MEMTAG(svcxcmdproc->lpCommandLine, "CommandLine");
    if (svcbatchlog) {
        op.hPipe = rd;
        op.oOverlap.hEvent = CreateEventEx(NULL, NULL,
                                           CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET,
                                           EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(op.oOverlap.hEvent)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"CreateEvent", NULL);
            svcxcmdproc->dwExitCode = rc;
            goto finished;
        }
    }
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    svcxcmdproc->dwCreationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    DBG_PRINTF("cmdline %S", svcxcmdproc->lpCommandLine);
    if (!CreateProcessW(svcxcmdproc->lpExe,
                        svcxcmdproc->lpCommandLine,
                        NULL, NULL, TRUE,
                        svcxcmdproc->dwCreationFlags,
                        NULL,
                        mainservice->lpWork,
                       &svcxcmdproc->sInfo,
                       &svcxcmdproc->pInfo)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        xsyserror(rc, L"CreateProcess", svcxcmdproc->lpExe);
        svcxcmdproc->dwExitCode = rc;
        goto finished;
    }
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(svcxcmdproc->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(svcxcmdproc->sInfo.hStdError);

    if (!xcreatethread(SVCBATCH_WRITE_THREAD,
                       1, wrpipethread, wr)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        xsyserror(rc, L"Write thread", NULL);
        TerminateProcess(svcxcmdproc->pInfo.hProcess, rc);
        svcxcmdproc->dwExitCode = rc;
        goto finished;
    }
    if (haslogrotate) {
        if (!xcreatethread(SVCBATCH_ROTATE_THREAD,
                           1, rotatethread, NULL)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"Rotate thread", NULL);
            TerminateProcess(svcxcmdproc->pInfo.hProcess, rc);
            svcxcmdproc->dwExitCode = rc;
            goto finished;
        }
    }
   if (ctrlbreaksig) {
        if (!xcreatethread(SVCBATCH_MONITOR_THREAD,
                           1, monitorthread, NULL)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"Monitor thread", NULL);
            TerminateProcess(svcxcmdproc->pInfo.hProcess, rc);
            svcxcmdproc->dwExitCode = rc;
        }
    }

    ResumeThread(svcxcmdproc->pInfo.hThread);
    ResumeThread(svcthread[SVCBATCH_WRITE_THREAD].hThread);
    InterlockedExchange(&svcxcmdproc->dwCurrentState, SVCBATCH_PROCESS_RUNNING);

    SAFE_CLOSE_HANDLE(svcxcmdproc->pInfo.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    if (ctrlbreaksig)
        ResumeThread(svcthread[SVCBATCH_MONITOR_THREAD].hThread);
    if (haslogrotate)
        ResumeThread(svcthread[SVCBATCH_ROTATE_THREAD].hThread);
    DBG_PRINTF("running process %lu", svcxcmdproc->pInfo.dwProcessId);
    if (svcbatchlog) {
        HANDLE wh[2];
        DWORD  nw = 2;

        wh[0] = svcxcmdproc->pInfo.hProcess;
        wh[1] = op.oOverlap.hEvent;
        do {
            DWORD ws;

            ws = WaitForMultipleObjects(nw, wh, FALSE, INFINITE);
            switch (ws) {
                case WAIT_OBJECT_0:
                    nw = 0;
                    DBG_PRINTS("process signaled");
                break;
                case WAIT_OBJECT_1:
                    if (op.dwState == ERROR_IO_PENDING) {
                        if (!GetOverlappedResult(op.hPipe, (LPOVERLAPPED)&op,
                                                &op.nRead, FALSE)) {
                            op.dwState = GetLastError();
                        }
                        else {
                            op.dwState = 0;
                            rc = logwrdata(op.bBuffer, op.nRead);
                        }
                    }
                    else {
                        if (ReadFile(op.hPipe, op.bBuffer, DSIZEOF(op.bBuffer),
                                    &op.nRead, (LPOVERLAPPED)&op) && op.nRead) {
                            op.dwState = 0;
                            rc = logwrdata(op.bBuffer, op.nRead);
                            SetEvent(op.oOverlap.hEvent);
                        }
                        else {
                            op.dwState = GetLastError();
                            if (op.dwState != ERROR_IO_PENDING)
                                rc = op.dwState;
                        }
                    }
                    if (rc) {
                        SAFE_CLOSE_HANDLE(op.hPipe);
                        ResetEvent(op.oOverlap.hEvent);
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
        WaitForSingleObject(svcxcmdproc->pInfo.hProcess, INFINITE);
    }
    InterlockedExchange(&svcxcmdproc->dwCurrentState, SVCBATCH_PROCESS_STOPPING);
    if (WaitForSingleObject(svcthread[SVCBATCH_WRITE_THREAD].hThread, SVCBATCH_STOP_STEP)) {
        DBG_PRINTS("wrpipethread is still active ... canceling sync io");
        CancelSynchronousIo(svcthread[SVCBATCH_WRITE_THREAD].hThread);
        WaitForSingleObject(svcthread[SVCBATCH_WRITE_THREAD].hThread, SVCBATCH_STOP_STEP);
    }
    if (!GetExitCodeProcess(svcxcmdproc->pInfo.hProcess, &rc))
        rc = GetLastError();
    if (rc) {
        if (rc != 255) {
            /**
              * 255 is exit code when CTRL_C is send to cmd.exe
              */
            setsvcstatusexit(rc);
            svcxcmdproc->dwExitCode = rc;
        }
    }
    DBG_PRINTF("finished process %lu with %lu",
               svcxcmdproc->pInfo.dwProcessId,
               svcxcmdproc->dwExitCode);

finished:
    xclearprocess(svcxcmdproc);

    SAFE_CLOSE_HANDLE(op.hPipe);
    SAFE_CLOSE_HANDLE(op.oOverlap.hEvent);

    DBG_PRINTS("done");
    SetEvent(workfinished);
    return svcxcmdproc->dwExitCode;
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
    static const char *msg = "UNKNOWN";

    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
            msg = "SERVICE_CONTROL_PRESHUTDOWN";
        break;
        case SERVICE_CONTROL_SHUTDOWN:
            msg = "SERVICE_CONTROL_SHUTDOWN";
        break;
        case SERVICE_CONTROL_STOP:
            msg = "SERVICE_CONTROL_STOP";
        break;
    }
    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
            /* fall through */
        case SERVICE_CONTROL_SHUTDOWN:
            /* fall through */
            killdepth = 0;
        case SERVICE_CONTROL_STOP:
            DBG_PRINTF("service %s", msg);
            reportsvcstatus(SERVICE_STOP_PENDING, stoptimeout + SVCBATCH_STOP_HINT);
            xcreatethread(SVCBATCH_STOP_THREAD, 0, stopthread, (LPVOID)msg);
        break;
        case SVCBATCH_CTRL_BREAK:
            if (ctrlbreaksig) {
                DBG_PRINTS("raising SVCBATCH_CTRL_BREAK");
                SetEvent(ctrlbreaksig);
            }
            else {
                DBG_PRINTS("ctrl+break is disabled");
                return ERROR_CALL_NOT_IMPLEMENTED;
            }
        break;
        case SVCBATCH_CTRL_ROTATE:
            if (haslogrotate) {
                /**
                 * Signal to rotatethread that
                 * user send custom service control
                 */
                if (canrotatelogs()) {
                    DBG_PRINTS("signaling SVCBATCH_CTRL_ROTATE");
                    SetEvent(logrotatesig);
                }
                else {
                    DBG_PRINTS("rotatelogs is busy");
                    return ERROR_CALL_NOT_IMPLEMENTED;
                }
            }
            else {
                DBG_PRINTS("log rotation is disabled");
                return ERROR_CALL_NOT_IMPLEMENTED;
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

static void __cdecl threadscleanup(void)
{
    int i;

    for(i = 0; i < SVCBATCH_MAX_THREADS; i++) {
        if (InterlockedCompareExchange(&svcthread[i].nStarted, 0, 0) > 0) {
            DBG_PRINTF("svcthread[%d]    x", i);
            svcthread[i].dwExitCode = ERROR_DISCARDED;
            TerminateThread(svcthread[i].hThread, svcthread[i].dwExitCode);
        }
        if (svcthread[i].hThread) {
#if defined(_DEBUG)
            DBG_PRINTF("svcthread[%d] %4lu %10llu", i,
                        svcthread[i].dwExitCode,
                        svcthread[i].dwDuration);
#endif
            CloseHandle(svcthread[i].hThread);
            svcthread[i].hThread = NULL;
        }
    }
}

static void __cdecl cconsolecleanup(void)
{
    FreeConsole();
}

static void __cdecl objectscleanup(void)
{
    SAFE_CLOSE_HANDLE(workfinished);
    SAFE_CLOSE_HANDLE(svcstopended);
    SAFE_CLOSE_HANDLE(svcstopstart);
    SAFE_CLOSE_HANDLE(ctrlbreaksig);
    SAFE_CLOSE_HANDLE(logrotatesig);

    SVCBATCH_CS_DELETE(mainservice);
}

static DWORD createevents(void)
{
    svcstopended = CreateEventEx(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(svcstopended))
        return GetLastError();
    svcstopstart = CreateEventEx(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(svcstopstart))
        return GetLastError();
    if (hasctrlbreak) {
        ctrlbreaksig = CreateEventEx(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(ctrlbreaksig))
            return GetLastError();
    }
    if (haslogrotate) {
        logrotatesig = CreateEventEx(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(logrotatesig))
            return GetLastError();
    }
    return 0;
}

static void WINAPI servicemain(DWORD argc, wchar_t **argv)
{
    DWORD rv = 0;
    DWORD i;

    mainservice->sStatus.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    mainservice->sStatus.dwCurrentState = SERVICE_START_PENDING;

    if (argc > 0) {
        xfree(mainservice->lpName);
        mainservice->lpName = xwcsdup(argv[0]);
    }
    if (IS_EMPTY_WCS(mainservice->lpName)) {
        xsyserror(ERROR_INVALID_PARAMETER, L"Service name", NULL);
        exit(1);
    }
    DBG_STRTAG(mainservice->lpName, "SVCBATCH_SERVICE_NAME");
    mainservice->hStatus = RegisterServiceCtrlHandlerExW(mainservice->lpName, servicehandler, NULL);
    if (IS_INVALID_HANDLE(mainservice->hStatus)) {
        xsyserror(GetLastError(), L"RegisterServiceCtrlHandlerEx", mainservice->lpName);
        exit(1);
    }
    DBG_PRINTF("%S", mainservice->lpName);
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    rv = createevents();
    if (rv) {
        xsyserror(rv, L"CreateEvent", NULL);
        reportsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    if (svcbatchlog) {
        if (outdirparam == NULL)
            outdirparam = SVCBATCH_LOGSDIR;
        rv = createlogsdir();
        if (rv) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        SetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS", svcbatchlog->szDir);
    }
    else {
        if (outdirparam == NULL) {
#if defined(_DEBUG)
            xsyswarn(ERROR_INVALID_PARAMETER, L"log directory", NULL);
            xsysinfo(L"Use -o option with parameter set to the exiting directory",
                     L"failing over to SVCBATCH_SERVICE_WORK");
#endif
            SetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS", mainservice->lpWork);
        }
        else {
            wchar_t *op = xgetfinalpath(outdirparam, 1, NULL, 0);
            if (op == NULL) {
                rv = xsyserror(GetLastError(), L"xgetfinalpath", outdirparam);
                reportsvcstatus(SERVICE_STOPPED, rv);
                return;
            }
            else {
                SetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS", op);
                xfree(op);
            }
        }
    }
    /**
     * Add additional arguments for batch file
     * from the application that started the service.
     *
     * eg, sc.exe start myservice param1 param2 ...
     */
    for (i = 1; i < argc; i++) {
        DBG_PRINTF("argv[%lu] %S", svcxcmdproc->nArgc, argv[i]);
        if (svcxcmdproc->nArgc < SVCBATCH_MAX_ARGS)
            svcxcmdproc->lpArgv[svcxcmdproc->nArgc++] = xwcsdup(argv[i]);
        else
            xsyswarn(0, L"The argument has exceeded the argument number limit", argv[i]);
    }

    /**
     * Add additional environment variables
     * They are unique to this service instance
     */
    SetEnvironmentVariableW(L"SVCBATCH_APP_BIN",      svcmainproc->lpExe);
    SetEnvironmentVariableW(L"SVCBATCH_APP_VER",      SVCBATCH_VERSION_VER);

    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_BASE", mainservice->lpBase);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_HOME", mainservice->lpHome);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_NAME", mainservice->lpName);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_UUID", mainservice->lpUuid);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_WORK", mainservice->lpWork);

    if (svcbatchlog) {
        reportsvcstatus(SERVICE_START_PENDING, 0);

        rv = openlogfile(TRUE);
        if (rv != 0) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        if (haslogstatus)
            logconfig(svcbatchlog->hFile);
    }
    reportsvcstatus(SERVICE_START_PENDING, 0);

    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rv = xsyserror(GetLastError(), L"Worker thread", NULL);
        goto finished;
    }
    WaitForSingleObject(svcthread[SVCBATCH_WORKER_THREAD].hThread, INFINITE);
    DBG_PRINTS("waiting for stop to finish");
    WaitForSingleObject(svcstopended, stoptimeout);

finished:
    DBG_PRINTS("finishing");
    if (svcbatchlog)
        closelogfile();
    threadscleanup();
    reportsvcstatus(SERVICE_STOPPED, rv);
    DBG_PRINTS("done");
}

static DWORD svcstopmain(void)
{
    DWORD  rv = 0;
    DWORD  ws;

    DBG_PRINTF("%S", mainservice->lpName);
    if (svcbatchlog)
        GetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS",
                                svcbatchlog->szDir, SVCBATCH_PATH_MAX);
    if (svcbatchlog) {
        rv = openlogfile(TRUE);
        if (rv != 0)
            return rv;
        if (haslogstatus)
            logconfig(svcbatchlog->hFile);
    }

    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rv = xsyserror(GetLastError(), L"Worker thread", NULL);
        setsvcstatusexit(rv);
        goto finished;
    }
    ws = WaitForSingleObject(svcthread[SVCBATCH_WORKER_THREAD].hThread, stoptimeout);
    if (ws == WAIT_TIMEOUT) {
        DBG_PRINTS("stop timeout");
        stopshutdown(SVCBATCH_STOP_SYNC);
        setsvcstatusexit(WAIT_TIMEOUT);
    }

finished:
    DBG_PRINTS("finishing");
    if (svcbatchlog)
        closelogfile();
    threadscleanup();
    DBG_PRINTS("done");
    return mainservice->sStatus.dwServiceSpecificExitCode;
}

static int xwmaininit(void)
{
    wchar_t  bb[SVCBATCH_PATH_MAX];
    DWORD    nn;

    /**
     * On systems that run Windows XP or later,
     * this functions will always succeed.
     */
    QueryPerformanceFrequency(&pcfrequency);
    QueryPerformanceCounter(&pcstarttime);

#if defined(_DEBUG)
    dbginit();
#endif
    xmemzero(svcthread, SVCBATCH_MAX_THREADS,     sizeof(SVCBATCH_THREAD));
    mainservice = (LPSVCBATCH_SERVICE)xmcalloc(1, sizeof(SVCBATCH_SERVICE));
    svcmainproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
    svcxcmdproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));

#if defined(_DEBUG) && (_DEBUG > 1)
    DBG_MEMTAG(mainservice, "SVCBATCH_SERVICE");
    DBG_MEMTAG(svcmainproc, "SVCBATCH_PROCESS");
    DBG_MEMTAG(svcxcmdproc, "SVCBATCH_SHELL_PROCESS");
#endif
    mainservice->dwCurrentState    = SERVICE_START_PENDING;
    svcmainproc->dwCurrentState    = SVCBATCH_PROCESS_RUNNING;
    svcmainproc->pInfo.hProcess    = GetCurrentProcess();
    svcmainproc->pInfo.dwProcessId = GetCurrentProcessId();
    svcmainproc->pInfo.dwThreadId  = GetCurrentThreadId();
    svcmainproc->sInfo.cb          = DSIZEOF(STARTUPINFOW);
    GetStartupInfoW(&svcmainproc->sInfo);

    nn = GetModuleFileNameW(NULL, bb, SVCBATCH_PATH_MAX);
    if ((nn == 0) || (nn >= SVCBATCH_PATH_MAX))
        return ERROR_BAD_PATHNAME;
    nn = fixshortpath(bb, nn);
    svcmainproc->lpExe = xwcsdup(bb);
    while (--nn > 2) {
        if (bb[nn] == L'\\') {
            bb[nn] = WNUL;
            svcmainproc->lpDir = xwcsdup(bb);
            break;
        }
    }
    ASSERT_WSTR(svcmainproc->lpExe, ERROR_BAD_PATHNAME);
    ASSERT_WSTR(svcmainproc->lpDir, ERROR_BAD_PATHNAME);
    nn = GetEnvironmentVariableW(L"COMSPEC", bb, SVCBATCH_PATH_MAX);
    if ((nn == 0) || (nn >= SVCBATCH_PATH_MAX))
        return GetLastError();
    svcxcmdproc->lpExe = xgetfinalpath(bb, 0, NULL, 0);
    ASSERT_WSTR(svcxcmdproc->lpExe, ERROR_BAD_PATHNAME);

#if defined(_DEBUG) && (_DEBUG > 1)
    DBG_MEMTAG(svcxcmdproc->lpExe, "COMSPEC");
    DBG_MEMTAG(svcmainproc->lpExe, "SVCBATCH_PROCESS ImageFile");
    DBG_MEMTAG(svcmainproc->lpDir, "SVCBATCH_PROCESS Directory");
#endif
    /* Reserve lpArgv[0] for batch file */
    svcxcmdproc->nArgc  = 1;
    svcxcmdproc->dwType = SVCBATCH_SHELL_PROCESS;
    SVCBATCH_CS_CREATE(mainservice);

    return 0;
}

int wmain(int argc, const wchar_t **wargv)
{
    int         i;
    int         opt;
    int         rcnt = 0;
    int         scnt = 0;
    int         qcnt = 0;
    int         rv;
    HANDLE      h;
    wchar_t     bb[BBUFSIZ] = { L'-', WNUL, WNUL, WNUL };

    const wchar_t *maxlogsparam = NULL;
    const wchar_t *batchparam   = NULL;
    const wchar_t *svchomeparam = NULL;
    const wchar_t *svcworkparam = NULL;
    const wchar_t *sparam[SVCBATCH_MAX_ARGS];
    const wchar_t *rparam[2];


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
    rv = xwmaininit();
    if (rv)
        return rv;

    h = GetStdHandle(STD_INPUT_HANDLE);
    if (argc == 1) {
        if (IS_VALID_HANDLE(h)) {
            fputs(cnamestamp, stdout);
            fputs("\n\nVisit " SVCBATCH_PROJECT_URL " for more details\n", stdout);
            return 0;
        }
        else {
            return ERROR_INVALID_PARAMETER;
        }
    }
    if (IS_INVALID_HANDLE(h)) {
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
            return GetLastError();
        }
        svcmainproc->sInfo.hStdInput  = h;
        svcmainproc->sInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        svcmainproc->sInfo.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }
    /**
     * Check if running as service or as a child process.
     */
    if (argc > 1) {
        const wchar_t *p = wargv[1];
        if (p[0] == L'@') {
            if ((p[1] == L'@') && (p[2] == WNUL)) {
                mainservice->lpName = xgetenv(L"SVCBATCH_SERVICE_NAME");
                if (mainservice->lpName == NULL)
                    return ERROR_BAD_ENVIRONMENT;
                mainservice->lpUuid = xgetenv(L"SVCBATCH_SERVICE_UUID");
                if (mainservice->lpUuid == NULL)
                    return ERROR_BAD_ENVIRONMENT;
                cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT;
                wnamestamp  = CPP_WIDEN(SHUTDOWN_APPNAME) L" " SVCBATCH_VERSION_WCS;
                cwsappname  = CPP_WIDEN(SHUTDOWN_APPNAME);
                svcmainproc->dwType = SVCBATCH_SHUTDOWN_PROCESS;
            }
            else {
                mainservice->lpName = xwcsdup(p + 1);
                if (IS_EMPTY_WCS(mainservice->lpName))
                    return ERROR_INVALID_PARAMETER;
                DBG_STRTAG(mainservice->lpName, "ServiceName");
                svcmainproc->dwType = SVCBATCH_SERVICE_PROCESS;
            }
            wargv[1] = wargv[0];
            argc    -= 1;
            wargv   += 1;
        }
    }
    if (svcmainproc->dwType == 0)
        svcmainproc->dwType = SVCBATCH_SERVICE_PROCESS;

#if defined(_DEBUG)
    dbgsvcmode = '0' + (char)(svcmainproc->dwType - 1);
    dbgprints(__FUNCTION__, cnamestamp);
#endif

    while ((opt = xwgetopt(argc, wargv, L"bc:h:k:lm:n:o:pqr:s:tvw:")) != EOF) {
        switch (opt) {
            case L'b':
                hasctrlbreak = TRUE;
            break;
            case L'l':
                uselocaltime = TRUE;
            break;
            case L'p':
                preshutdown  = SERVICE_ACCEPT_PRESHUTDOWN;
            break;
            case L't':
                truncatelogs = TRUE;
            break;
            case L'v':
                haslogstatus = TRUE;
            break;
            /**
             * Options with arguments
             */
            case L'c':
                codepageopt  = xwoptarg;
            break;
            case L'm':
                maxlogsparam = xwoptarg;
            break;
            case L'n':
                svclogfname  = xwoptarg;
            break;
            case L'o':
                outdirparam  = xwoptarg;
            break;
            case L'h':
                svchomeparam = xwoptarg;
            break;
            case L'w':
                svcworkparam = xwoptarg;
            break;
            case L'k':
                stoptimeout  = xwcstoi(xwoptarg, NULL);
                if ((stoptimeout < SVCBATCH_STOP_TMIN) || (stoptimeout > SVCBATCH_STOP_TMAX))
                    return xsyserror(0, L"The -k command option value is outside valid range", xwoptarg);
                stoptimeout  = stoptimeout * 1000;
            break;
            /**
             * Options that can be defined
             * multiple times
             */
            case L'q':
                havestoplogs = FALSE;
                havemainlogs = FALSE;
                qcnt++;
            break;
            case L's':
                if (scnt < SVCBATCH_MAX_ARGS)
                    sparam[scnt++] = xwoptarg;
                else
                    return xsyserror(0, L"Too many -s arguments", xwoptarg);
            break;
            case L'r':
                if (rcnt < 2)
                    rparam[rcnt++] = xwoptarg;
                else
                    return xsyserror(0, L"Too many -r options", xwoptarg);
            break;
            case ENOENT:
                bb[1] = xwoption;
                return xsyserror(0, L"Missing argument for command line option", bb);
            break;
            default:
                bb[1] = xwoption;
                return xsyserror(0, L"Invalid command line option", bb);
            break;
        }
    }

    if (codepageopt) {
        if (xwstartswith(codepageopt, L"UTF")) {
            if (wcschr(codepageopt + 3, L'8'))
                svccodepage = 65001;
            else
                return xsyserror(0, L"Invalid -c command option value", codepageopt);
        }
        else {
            svccodepage = xwcstoi(codepageopt, NULL);
            if (svccodepage < 0)
                return xsyserror(0, L"Invalid -c command option value", codepageopt);
        }
        DBG_PRINTF("using code page %d", svccodepage);
    }

    argc  -= xwoptind;
    wargv += xwoptind;
    if (argc == 0) {
        /**
         * No batch file defined.
         * If @ServiceName was defined, try with ServiceName.bat
         */
        if (IS_EMPTY_WCS(mainservice->lpName))
            return xsyserror(0, L"Missing batch file", NULL);
        else
            batchparam = xwcsconcat(mainservice->lpName, L".bat");

        if (wcspbrk(batchparam, L"/\\:;<>?*|\""))
            return xsyserror(0, L"Batch filename has invalid characters", batchparam);
        DBG_STRTAG_FN(batchparam, "BatchFileName");
    }
    else {
        batchparam = wargv[0];
        for (i = 1; i < argc; i++, svcxcmdproc->nArgc++) {
            /**
             * Add arguments for batch file
             */
            if (svcxcmdproc->nArgc < SVCBATCH_MAX_ARGS)
                svcxcmdproc->lpArgv[svcxcmdproc->nArgc] = xwcsdup(wargv[i]);
            else
                return xsyserror(0, L"Too many batch arguments", wargv[i]);
            DBG_STRTAG(svcxcmdproc->lpArgv[svcxcmdproc->nArgc], "BatchArgument");
        }
    }
    if (svcmainproc->dwType == SVCBATCH_SERVICE_PROCESS)
        mainservice->lpUuid = xuuidstring(NULL);
    if (IS_EMPTY_WCS(mainservice->lpUuid))
        return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_UUID", NULL);

    if (scnt && qcnt < 2) {
        /**
         * Use -qq to disable both service and shutdown logging
         */
        havemainlogs = TRUE;
    }
    if (havemainlogs) {
        svcbatchlog = (LPSVCBATCH_LOG)xmcalloc(1, sizeof(SVCBATCH_LOG));
        svcbatchlog->dwType = SVCBATCH_LOG_FILE;
        SVCBATCH_CS_CREATE(svcbatchlog);

        DBG_MEMTAG(svcbatchlog, "SVCBATCH_LOG");
        if (svcmainproc->dwType == SVCBATCH_SERVICE_PROCESS) {
            if (maxlogsparam) {
                if (truncatelogs) {
#if defined(_DEBUG) && (_DEBUG > 1)
                    xsyswarn(0, L"Configuration error",
                             L"Option -t is mutually exclusive with option -m");
#else
                    return
                    xsyserror(0, L"Configuration error",
                             L"Option -t is mutually exclusive with option -m");
#endif
                }
                svcmaxlogs = xwcstoi(maxlogsparam, NULL);
                if ((svcmaxlogs < 1) || (svcmaxlogs > SVCBATCH_MAX_LOGS))
                    return xsyserror(0, L"Invalid -m command option value", maxlogsparam);
            }
            svclogfnext = SVCBATCH_LOGFEXT;
        }
        else {
            svclogfnext = SHUTDOWN_LOGFEXT;
            svcmaxlogs  = SHUTDOWN_MAX_LOGS;
        }
    }
    else {
        /**
         * Ensure that log related command options
         * are not defined when -q is defined
         */
        bb[0] = WNUL;
        if (maxlogsparam)
            xwcslcat(bb, TBUFSIZ, L"-m ");
        if (svclogfname)
            xwcslcat(bb, TBUFSIZ, L"-n ");
        if (rcnt)
            xwcslcat(bb, TBUFSIZ, L"-r ");
        if (truncatelogs)
            xwcslcat(bb, TBUFSIZ, L"-t ");
        if (haslogstatus)
            xwcslcat(bb, TBUFSIZ, L"-v ");
        if (bb[0]) {
#if defined(_DEBUG) && (_DEBUG > 1)
            xsyswarn(0, L"Option -q is mutually exclusive with option(s)", bb);
            rcnt         = 0;
            haslogstatus = FALSE;
            haslogrotate = FALSE;
            truncatelogs = FALSE;
#else
            return xsyserror(0, L"Option -q is mutually exclusive with option(s)", bb);
#endif
        }
    }
    if (svcmainproc->dwType == SVCBATCH_SERVICE_PROCESS) {
        /**
         * Find the location of SVCBATCH_SERVICE_HOME
         * all relative paths are resolved against it.
         *
         * 1. If -w is defined and is absolute path
         *    set it as SetCurrentDirectory and use it as
         *    home directory for resolving other relative paths
         *
         * 2. If batch file is defined as absolute path
         *    set it as SetCurrentDirectory and resolve -w parameter
         *    if defined as relative path. If -w was defined and
         *    and is resolved as valid path set it as home directory.
         *    If -w was defined and cannot be resolved fail.
         *
         * 3. Use running svcbatch.exe directory and set it as
         *    SetCurrentDirectory.
         *    If -w parameter was defined, resolve it and set as home
         *    directory or fail.
         *    In case -w was not defined resolve batch file and set its
         *    directory as home directory or fail if it cannot be resolved.
         *
         */

        if (svchomeparam == NULL)
            svchomeparam = svcworkparam;
        if (svcworkparam == NULL)
            svcworkparam = svchomeparam;

        if (svchomeparam) {
            if (isabsolutepath(svchomeparam)) {
                mainservice->lpHome = xgetfinalpath(svchomeparam, 1, NULL, 0);
                if (IS_EMPTY_WCS(mainservice->lpHome))
                    return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
                DBG_MEMTAG(mainservice->lpHome, "SVCBATCH_SERVICE_HOME");
            }
        }
        if (mainservice->lpHome == NULL) {
            if (isabsolutepath(batchparam)) {
                if (!resolvebatchname(batchparam))
                    return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);

                if (svchomeparam == NULL) {
                    mainservice->lpHome = mainservice->lpBase;
                }
                else {
                    SetCurrentDirectoryW(mainservice->lpBase);
                    mainservice->lpHome = xgetfinalpath(svchomeparam, 1, NULL, 0);
                    if (IS_EMPTY_WCS(mainservice->lpHome))
                        return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
                    DBG_MEMTAG(mainservice->lpHome, "SVCBATCH_SERVICE_HOME");
                }
            }
        }
        if (mainservice->lpHome == NULL) {
            if (svchomeparam == NULL) {
                mainservice->lpHome = svcmainproc->lpDir;
            }
            else {
                SetCurrentDirectoryW(svcmainproc->lpDir);
                mainservice->lpHome = xgetfinalpath(svchomeparam, 1, NULL, 0);
                if (IS_EMPTY_WCS(mainservice->lpHome))
                    return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
                DBG_MEMTAG(mainservice->lpHome, "SVCBATCH_SERVICE_HOME");
            }
        }
        SetCurrentDirectoryW(mainservice->lpHome);
        if (svchomeparam == svcworkparam) {
            /* Use the same directories for home and work */
            mainservice->lpWork = mainservice->lpHome;
        }
        else {
            mainservice->lpWork = xgetfinalpath(svcworkparam, 1, NULL, 0);
            if (IS_EMPTY_WCS(mainservice->lpWork))
                return xsyserror(ERROR_FILE_NOT_FOUND, svcworkparam, NULL);
            SetCurrentDirectoryW(mainservice->lpWork);
            DBG_MEMTAG(mainservice->lpWork, "SVCBATCH_SERVICE_WORK");
        }
        if (!resolvebatchname(batchparam))
            return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);
    }
    else {
        mainservice->lpHome = xgetenv(L"SVCBATCH_SERVICE_HOME");
        if (mainservice->lpHome == NULL)
            return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_HOME", NULL);
        mainservice->lpBase = xgetenv(L"SVCBATCH_SERVICE_BASE");
        if (mainservice->lpBase == NULL)
            return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_BASE", NULL);
        mainservice->lpWork = xgetenv(L"SVCBATCH_SERVICE_WORK");
        if (mainservice->lpWork == NULL)
            return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_WORK", NULL);
        svcxcmdproc->lpArgv[0] = xwcsdup(batchparam);
        DBG_STRTAG_FN(svcxcmdproc->lpArgv[0], "BatchFileName");
    }

    if (svcmainproc->dwType == SVCBATCH_SERVICE_PROCESS) {
        if (rcnt) {
            for (i = 0; i < rcnt; i++) {
                if (rparam[i][0] == L'S') {
                    DBG_PRINTS("rotate by signal");
                    rcnt = 0;
                    break;
                }
            }
            for (i = 0; i < rcnt; i++) {
                if (!resolverotate(rparam[i]))
                    return xsyserror(0, L"Invalid rotate parameter", rparam[i]);
            }
            haslogrotate = TRUE;
            svcmaxlogs   = 0;
        }
        if (svclogfname) {
            if (wcspbrk(svclogfname, L"/\\:;<>?*|\""))
                return xsyserror(0, L"Found invalid filename characters", svclogfname);

            if (wcschr(svclogfname, L'@')) {
                wchar_t *p = xwcsdup(svclogfname);
                /**
                 * If name is strftime formatted
                 * replace @ with % so it can be used by strftime
                 */
                xwchreplace(p, L'@', L'%');
                DBG_STRTAG(p, "FormattedLogName");
                svclogfname = p;
            }
        }
        if (scnt) {
            svcstopproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
            DBG_MEMTAG(svcstopproc, "SVCBATCH_SHUTDOWN_PROCESS");
            svcstopproc->lpArgv[0] = xgetfinalpath(sparam[0], 0, NULL, 0);
            if (svcstopproc->lpArgv[0] == NULL)
                return xsyserror(ERROR_FILE_NOT_FOUND, sparam[0], NULL);
            DBG_STRTAG_FN(svcstopproc->lpArgv[0],  "StopBatchFile");
            for (i = 1; i < scnt; i++) {
                svcstopproc->lpArgv[i] = xwcsdup(sparam[i]);
                DBG_STRTAG(svcstopproc->lpArgv[i], "StopArgument ");
            }
            svcstopproc->nArgc  = scnt;
            svcstopproc->dwType = SVCBATCH_SHUTDOWN_PROCESS;
        }
        if (mainservice->lpHome != mainservice->lpWork)
            SetCurrentDirectoryW(mainservice->lpHome);
    }

    /**
     * Create logic state events
     */
    workfinished = CreateEventEx(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(workfinished))
        return xsyserror(GetLastError(), L"CreateEvent", NULL);
    atexit(objectscleanup);

    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consolehandler, TRUE);
#if defined(_DEBUG) && (_DEBUG > 1)
    dbgmemdump("memory");
#endif
    if (svcmainproc->dwType == SVCBATCH_SERVICE_PROCESS) {
        SERVICE_TABLE_ENTRYW se[2];

        se[0].lpServiceName = zerostring;
        se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
        se[1].lpServiceName = NULL;
        se[1].lpServiceProc = NULL;
        DBG_PRINTS("starting service");
        if (!StartServiceCtrlDispatcherW(se))
            rv = xsyserror(GetLastError(), L"StartServiceCtrlDispatcher", NULL);
    }
    else {
        DBG_PRINTS("starting shutdown");
        rv = svcstopmain();

    }
#if defined(_DEBUG)
# if (_DEBUG > 1)
# if (_DEBUG > 2)
    xfree(mainservice->lpName);
    xfree(mainservice->lpBase);
    xfree(mainservice->lpUuid);
    if (mainservice->lpHome != svcmainproc->lpDir)
        xfree(mainservice->lpHome);
    if (mainservice->lpWork != mainservice->lpHome)
        xfree(mainservice->lpWork);
    xfree(svcmainproc->lpExe);
    xfree(svcmainproc->lpDir);
    SAFE_MEM_FREE(svcstopproc);
    SAFE_MEM_FREE(svcxcmdproc);
    SAFE_MEM_FREE(mainservice);
    SAFE_MEM_FREE(svcmainproc);
# endif
    dbgmemdump("memory");
# endif
    dbgprintf(__FUNCTION__, "done %lu", rv);
#endif
    return rv;
}
