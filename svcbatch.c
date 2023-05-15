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

static char dbgsvcmode = 'x';
#endif

typedef enum {
    SVCBATCH_WORKER_THREAD = 0,
    SVCBATCH_WRPIPE_THREAD,
    SVCBATCH_STOP_THREAD,
    SVCBATCH_SIGNAL_THREAD,
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
    volatile LONG64     nWritten;
    volatile HANDLE     hFile;
    volatile LONG       dwCurrentState;
    volatile LONG       nRotateCount;
    int                 nMaxLogs;
    CRITICAL_SECTION    csLock;
    LPWSTR              lpFileName;
    LPCWSTR             lpFileExt;
    WCHAR               szName[_MAX_FNAME];
    WCHAR               szDir[SVCBATCH_PATH_MAX];

} SVCBATCH_LOG, *LPSVCBATCH_LOG;

typedef struct _SVCBATCH_SERVICE {
    volatile LONG           dwCurrentState;
    SERVICE_STATUS_HANDLE   hStatus;
    SERVICE_STATUS          sStatus;
    CRITICAL_SECTION        csLock;

    LPWSTR                  lpLogName;
    LPWSTR                  lpBase;
    LPWSTR                  lpHome;
    LPWSTR                  lpLogs;
    LPWSTR                  lpName;
    LPWSTR                  lpUuid;
    LPWSTR                  lpWork;

} SVCBATCH_SERVICE, *LPSVCBATCH_SERVICE;

typedef struct _SVCBATCH_IPC {
    DWORD   dwProcessId;
    DWORD   dwCodePage;
    DWORD   dwOptions;
    DWORD   dwTimeout;
    DWORD   nArgc;
    WCHAR   szUuid[TBUFSIZ];
    WCHAR   szServiceName[_MAX_FNAME];
    WCHAR   szLogName[_MAX_FNAME];
    WCHAR   szHomeDir[SVCBATCH_PATH_MAX];
    WCHAR   szWorkDir[SVCBATCH_PATH_MAX];
    WCHAR   szLogsDir[SVCBATCH_PATH_MAX];
    WCHAR   szShell[SVCBATCH_PATH_MAX];
    WCHAR   szBatch[SVCBATCH_PATH_MAX];
    WCHAR   lpArgv[SVCBATCH_MAX_ARGS][_MAX_FNAME];

} SVCBATCH_IPC, *LPSVCBATCH_IPC;

static LPSVCBATCH_PROCESS    svcstopproc = NULL;
static LPSVCBATCH_PROCESS    svcmainproc = NULL;
static LPSVCBATCH_PROCESS    svcxcmdproc = NULL;
static LPSVCBATCH_SERVICE    mainservice = NULL;
static LPSVCBATCH_LOG        svcbatchlog = NULL;
static LPSVCBATCH_LOG        svcbstatlog = NULL;
static LPSVCBATCH_IPC        shutdownmem = NULL;

static LONGLONG              rotateint   = CPP_INT64_C(0);
static LARGE_INTEGER         rotatetmo   = {{ 0, 0 }};
static LARGE_INTEGER         rotatesiz   = {{ 0, 0 }};
static LARGE_INTEGER         pcfrequency = {{ 0, 0 }};
static LARGE_INTEGER         pcstarttime = {{ 0, 0 }};

static BOOL      rotatebysize     = FALSE;
static BOOL      rotatebytime     = FALSE;
static BOOL      havemainlogs     = TRUE;
static BOOL      havestoplogs     = TRUE;
static BOOL      servicemode      = TRUE;

static DWORD     svcoptions       = 0;
static DWORD     preshutdown      = 0;
static int       svccodepage      = 0;
static int       stoptimeout      = SVCBATCH_STOP_TIME;
static int       killdepth        = 2;

static HANDLE    svcstopstart     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    workfinished     = NULL;
static HANDLE    ctrlbreaksig     = NULL;
static HANDLE    logrotatesig     = NULL;
static HANDLE    shutdownmmap     = NULL;

static SVCBATCH_THREAD svcthread[SVCBATCH_MAX_THREADS];

static wchar_t      zerostring[2] = { WNUL, WNUL };
static const wchar_t *CRLFW       = L"\r\n";
static const char    *YYES        =  "Y\r\n";

static const char    *cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;
static const wchar_t *wnamestamp  = CPP_WIDEN(SVCBATCH_NAME) L" " SVCBATCH_VERSION_WCS;
static const wchar_t *cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);
static const wchar_t *outdirparam = NULL;

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

static int xfatalerr(const char *func, int err)
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
    wchar_t *p = (wchar_t *)xmmalloc((size + 1) * sizeof(wchar_t));

    p[size] = WNUL;
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
        d = xwmalloc(n);
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
    wchar_t *d = dst;
    int      n = siz;
    int      c;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 1, 0);

    while ((n-- != 0) && (*d != WNUL))
        d++;
    c = (int)(d - dst);
    n = siz - c;
    if ((n == 0) || IS_EMPTY_WCS(src))
        return c;
    while ((n-- != 0) && (*s != WNUL))
        *d++ = *s++;

    *d = WNUL;
    return (int)(d - dst);
}

static int xwcslcpy(wchar_t *dst, int siz, const wchar_t *src)
{
    const wchar_t *s = src;
    wchar_t *d = dst;
    int      n = siz;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 1, 0);
    *d = WNUL;
    ASSERT_WSTR(src, 0);

    while ((n-- != 0) && (*s != WNUL))
        *d++ = *s++;

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

    dst[0] = WNUL;
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

    dst[0] = '\0';
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

    cp = xwmalloc(l1 + l2);
    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    if(l2 > 0)
        wmemcpy(cp + l1, s2, l2);

    return cp;
}

static wchar_t *xwcsmkpath(const wchar_t *ds,
                           const wchar_t *fs,
                           const wchar_t *fx)
{
    wchar_t *cp;
    int nd;
    int nf;
    int nx;

    nd = xwcslen(ds);
    if (nd == 0)
        return NULL;

    nf = xwcslen(fs);
    nx = xwcslen(fx);

    cp = xwmalloc(nd + nf + nx + 1);
    wmemcpy(cp, ds, nd);
    cp[nd++] = L'\\';
    if (nf)
        wmemcpy(cp + nd, fs, nf);
    if (nx)
        wmemcpy(cp + nd + nf, fx, nx);

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
        nq = 0;
        if (wcspbrk(s3, L" \t\"")) {
            for (c = s3; ; c++, nq++) {
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
            l3 = nq + 2;
        }
    }
    l1 = xwcslen(s1);
    l2 = xwcslen(s2);
    nn = l1 + l2 + l3 + 3;
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
        for (c = s3; ; c++, d++) {
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

static wchar_t *xmktimedext(void)
{
    static wchar_t b[QBUFSIZ];
    SYSTEMTIME st;

    if (IS_SET(SVCBATCH_OPT_LOCALTIME))
        GetLocalTime(&st);
    else
        GetSystemTime(&st);
    xsnwprintf(b, QBUFSIZ, L".%.4d%.2d%.2d%.2d%.2d%.2d",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return b;
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
    hh = (DWORD)((ct.QuadPart / MS_IN_HOUR)   % CPP_INT64_C(24));

    return xsnprintf(wb, sz, "%.2lu:%.2lu:%.2lu.%.6lu",
                     hh, mm, ss, us);
}

#if defined(_DEBUG)
/**
 * Runtime debugging functions
 */

static void dbgprintf(const char *funcname, const char *format, ...)
{
    int     n;
    char    b[MBUFSIZ];
    va_list ap;

    n = xsnprintf(b, MBUFSIZ, "[%.4lu] %c %-16s ",
                  GetCurrentThreadId(),
                  dbgsvcmode, funcname);

    va_start(ap, format);
    xvsnprintf(b + n, MBUFSIZ - n, format, ap);
    va_end(ap);
    OutputDebugStringA(b);
}

static void dbgprints(const char *funcname, const char *string)
{
    dbgprintf(funcname, "%s", string);
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
    InterlockedExchange(&p->dwCurrentState, SVCBATCH_PROCESS_STOPPED);

    DBG_PRINTF("%.4lu %lu %S", p->pInfo.dwProcessId, p->dwExitCode, p->lpExe);
    SAFE_CLOSE_HANDLE(p->pInfo.hProcess);
    SAFE_CLOSE_HANDLE(p->pInfo.hThread);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdError);
    SAFE_MEM_FREE(p->lpCommandLine);
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
    DBG_PRINTF("%lu ended %lu", p->dwId, p->dwExitCode);
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
    svcthread[id].dwId           = id;
    svcthread[id].lpStartAddress = threadfn;
    svcthread[id].lpParameter    = param;
    svcthread[id].hThread        = CreateThread(NULL, 0, xrunthread, &svcthread[id],
                                                suspended ? CREATE_SUSPENDED : 0, NULL);
    if (svcthread[id].hThread == NULL) {
        svcthread[id].dwExitCode = GetLastError();
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
    if (dst == NULL)
        dst = xwcsdup(buf);
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
    p = wcsrchr(svcxcmdproc->lpArgv[0], L'\\');
    if (p) {
        *p = WNUL;
        mainservice->lpBase = xwcsdup(svcxcmdproc->lpArgv[0]);
        *p = L'\\';
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

    if (!servicemode)
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

static DWORD logappend(HANDLE h, LPCVOID buf, DWORD len)
{
    DWORD wr;

    if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0)) {
        InterlockedAdd64(&svcbatchlog->nWritten, wr);
        return 0;
    }
    return GetLastError();
}

static BOOL logwlines(HANDLE h, int nl, const char *sb, const char *xb)
{
    char    wb[TBUFSIZ];
    DWORD   wr;
    DWORD   nw;

    ASSERT_HANDLE(h, FALSE);
    nw = xtimehdr(wb, TBUFSIZ);
    if (nl) {
        wb[nw] = '\n';
        WriteFile(h, wb, nw + 1, &wr, NULL);
    }
    if (sb || xb) {
        wb[nw] = ' ';
        WriteFile(h, wb, nw + 1, &wr, NULL);

        nw = xstrlen(sb);
        if (nw)
            WriteFile(h, sb, nw, &wr, NULL);
        nw = xstrlen(xb);
        if (nw)
            WriteFile(h, xb, nw, &wr, NULL);
        wb[0] = '\n';
        WriteFile(h, wb, 1, &wr, NULL);
    }
    return TRUE;
}

static BOOL logwrline(HANDLE h, int nl, const char *sb)
{
    return logwlines(h, nl, sb, NULL);
}

static void logprintf(HANDLE h, int nl, const char *format, ...)
{
    int     c;
    char    buf[FBUFSIZ];
    va_list ap;

    if (IS_INVALID_HANDLE(h))
        return;
    va_start(ap, format);
    c = xvsnprintf(buf, FBUFSIZ, format, ap);
    va_end(ap);
    if (c > 0)
        logwrline(h, nl, buf);
}

static void logwransi(HANDLE h, int nl, const char *hdr, const wchar_t *wcs)
{
    char buf[FBUFSIZ];

    if (IS_INVALID_HANDLE(h))
        return;
    xwcstombs(svccodepage, buf, FBUFSIZ, wcs);
    logwlines(h, nl, hdr, buf);
}

static void logwrtime(HANDLE h, int nl, const char *hdr)
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
    char             nb[NBUFSIZ] = { 0, 0};
    char             vb[NBUFSIZ] = { 0, 0};
    const char      *pv = NULL;
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

    sz = NBUFSIZ - 1;
    if (RegGetValueA(hk, NULL, "ProductName",
                     RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                     NULL, nb, &sz) != ERROR_SUCCESS)
        goto finished;
    sz = NBUFSIZ - 1;
    if (RegGetValueA(hk, NULL, "ReleaseId",
                     RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                     NULL, vb, &sz) == ERROR_SUCCESS)
        pv = vb;
    sz = NBUFSIZ - 1;
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
    logwrline(h, 0, cnamestamp);
    logwinver(h);
    logwransi(h, 1, "Service name     : ", mainservice->lpName);
    logwransi(h, 0, "Service uuid     : ", mainservice->lpUuid);
    logwransi(h, 0, "Batch file       : ", svcxcmdproc->lpArgv[0]);
    if (svcstopproc)
        logwransi(h, 0, "Shutdown batch   : ", svcstopproc->lpArgv[0]);
    logwransi(h, 0, "Program directory: ", svcmainproc->lpDir);
    logwransi(h, 0, "Base directory   : ", mainservice->lpBase);
    logwransi(h, 0, "Home directory   : ", mainservice->lpHome);
    logwransi(h, 0, "Logs directory   : ", svcbatchlog->szDir);
    logwransi(h, 0, "Work directory   : ", mainservice->lpWork);

    FlushFileBuffers(h);
}

static void logwrstat(LPSVCBATCH_LOG log, int nl, int wt, const char *hdr)
{
    HANDLE h;

    if (log == NULL)
        return;
    SVCBATCH_CS_ENTER(log);
    h = InterlockedExchangePointer(&log->hFile, NULL);
    if (h == NULL)
        goto finished;
    if (nl && (InterlockedIncrement(&log->nRotateCount) > 1))
        nl--;
    if (wt)
        logwrtime(h, nl, hdr);
    else
        logwrline(h, nl, hdr);
finished:
    InterlockedExchangePointer(&log->hFile, h);
    SVCBATCH_CS_LEAVE(log);
}

static BOOL canrotatelogs(LPSVCBATCH_LOG log)
{
    BOOL rv = FALSE;

    SVCBATCH_CS_ENTER(log);
    if (InterlockedCompareExchange(&log->dwCurrentState, 0, 0) == 0) {
        if (InterlockedCompareExchange64(&log->nWritten, 0, 0)) {
            InterlockedExchange(&log->dwCurrentState, 1);
            rv = TRUE;
        }
    }
    SVCBATCH_CS_LEAVE(log);
    return rv;
}

static DWORD createlogsdir(LPSVCBATCH_LOG log)
{
    wchar_t *p;
    wchar_t dp[SVCBATCH_PATH_MAX];

    if (xgetfullpath(outdirparam, dp, SVCBATCH_PATH_MAX) == NULL) {
        xsyserror(0, L"xgetfullpath", outdirparam);
        return ERROR_BAD_PATHNAME;
    }
    p = xgetfinalpath(dp, 1, log->szDir, SVCBATCH_PATH_MAX);
    if (p == NULL) {
        DWORD rc = GetLastError();

        if (rc > ERROR_PATH_NOT_FOUND)
            return xsyserror(rc, L"xgetfinalpath", dp);

        rc = xcreatedir(dp);
        if (rc != 0)
            return xsyserror(rc, L"xcreatedir", dp);
        p = xgetfinalpath(dp, 1, log->szDir, SVCBATCH_PATH_MAX);
        if (p == NULL)
            return xsyserror(GetLastError(), L"xgetfinalpath", dp);
    }
    if (_wcsicmp(log->szDir, mainservice->lpHome) == 0) {
        xsyserror(0, L"Logs directory cannot be the same as home directory", log->szDir);
        return ERROR_INVALID_PARAMETER;
    }
    return 0;
}

static DWORD rotateprevlogs(LPSVCBATCH_LOG log, BOOL ssp, HANDLE ssh)
{
    DWORD   rc;
    int     i;
    int     x;
    wchar_t lognn[SVCBATCH_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA ad;

    xwcslcpy(lognn, SVCBATCH_PATH_MAX, log->lpFileName);
    if (log->nMaxLogs > 1)
        x = xwcslcat(lognn, SVCBATCH_PATH_MAX, L".0");
    else
        x = xwcslcat(lognn, SVCBATCH_PATH_MAX, xmktimedext());
    if (x >= SVCBATCH_PATH_MAX)
        return xsyserror(ERROR_BAD_PATHNAME, lognn, NULL);

    if (GetFileAttributesExW(log->lpFileName, GetFileExInfoStandard, &ad)) {
        if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
            DBG_PRINTS("empty log");
            if (ssh) {
                logwrline(ssh, 0, "Empty");
                logwransi(ssh, 0, "- ", log->lpFileName);
            }
            return 0;
        }
        if (!MoveFileExW(log->lpFileName, lognn, MOVEFILE_REPLACE_EXISTING))
            return xsyserror(GetLastError(), log->lpFileName, lognn);
        if (ssp)
            reportsvcstatus(SERVICE_START_PENDING, 0);
        if (ssh) {
            logwrline(ssh, 0, "Moving");
            logwransi(ssh, 0, "  ", log->lpFileName);
            logwransi(ssh, 0, "> ", lognn);
        }
    }
    else {
        rc = GetLastError();
        if (rc != ERROR_FILE_NOT_FOUND)
            return xsyserror(rc, log->lpFileName, NULL);
        else
            return 0;
    }
    if (log->nMaxLogs > 1) {
        int n = log->nMaxLogs;
        wchar_t logpn[SVCBATCH_PATH_MAX];

        wmemcpy(logpn, lognn, x + 1);
        x--;
        for (i = 2; i < log->nMaxLogs; i++) {
            lognn[x] = L'0' + i;

            if (!GetFileAttributesExW(lognn, GetFileExInfoStandard, &ad))
                break;
            if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0))
                break;
        }
        n = i;
        if (ssh)
            logprintf(ssh, 0, "Rotating %d of %d", n, log->nMaxLogs);
        /**
         * Rotate previous log files
         */
        for (i = n; i > 0; i--) {
            logpn[x] = L'0' + i - 1;
            lognn[x] = L'0' + i;
            if (GetFileAttributesExW(logpn, GetFileExInfoStandard, &ad)) {
                if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
                    if (ssh)
                        logwransi(ssh, 0, "- ", logpn);
                }
                else {
                    if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING))
                        return xsyserror(GetLastError(), logpn, lognn);
                    if (ssp)
                        reportsvcstatus(SERVICE_START_PENDING, 0);
                    if (ssh) {
                        logwransi(ssh, 0, "  ", logpn);
                        logwransi(ssh, 0, "> ", lognn);
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

static DWORD makelogname(LPWSTR dst, int siz, LPCWSTR src)
{
    struct  tm *ctm;
    time_t  ctt;
    int     i, x;

    time(&ctt);
    if (IS_SET(SVCBATCH_OPT_LOCALTIME))
        ctm = localtime(&ctt);
    else
        ctm = gmtime(&ctt);
    if (wcsftime(dst, siz, src, ctm) == 0)
        return xsyserror(0, L"Invalid format code", src);

    for (i = 0, x = 0; dst[i]; i++) {
        wchar_t c = dst[i];

        if ((c > 127) || (xfnchartype[c] & 1))
            dst[x++] = c;
    }
    dst[x] = WNUL;
    DBG_PRINTF("%S -> %S", src, dst);
    return 0;
}

static DWORD openlogfile(LPSVCBATCH_LOG log, BOOL ssp, HANDLE ssh)
{
    HANDLE  fh = NULL;
    DWORD   rc;
    WCHAR   nb[_MAX_FNAME];
    BOOL    rp = TRUE;
    LPCWSTR np = log->szName;


#if defined(_DEBUG)
    if (log->lpFileName) {
        DBG_PRINTF("!found %S", log->lpFileName);
    }
#endif
    if (ssp) {
        if (ssh)
            logwrtime(ssh, 1, "Log create");
    }
    else {
        rp = IS_NOT(SVCBATCH_OPT_TRUNCATE);
    }
    if (wcschr(np, L'%')) {
        rc = makelogname(nb, _MAX_FNAME, np);
        if (rc)
            return rc;
        rp = FALSE;
        np = nb;
    }

    log->lpFileName = xwcsmkpath(log->szDir, np, log->lpFileExt);
    if (log->lpFileName == NULL)
        return xsyserror(ERROR_FILE_NOT_FOUND, np, NULL);

    if (rp) {
        rc = rotateprevlogs(log, ssp, ssh);
        if (rc)
            return rc;
    }
    fh = CreateFileW(log->lpFileName,
                     GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ, NULL,
                     CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(fh))
        return xsyserror(rc, log->lpFileName, NULL);
    if (ssh) {
        if (rc == ERROR_ALREADY_EXISTS)
            logwransi(ssh, 0, "x ", log->lpFileName);
        else
            logwransi(ssh, 0, "+ ", log->lpFileName);
        if (ssp)
            logwrtime(ssh, 0, "Log opened");
    }
#if defined(_DEBUG)
    if (rc == ERROR_ALREADY_EXISTS)
        dbgprintf(ssp ? "createlogfile" : "openlogfile",
                  "truncated %S", log->lpFileName);
    else
        dbgprintf(ssp ? "createlogfile" : "openlogfile",
                  "created %S",   log->lpFileName);
#endif
    InterlockedExchange64(&log->nWritten, 0);
    InterlockedExchangePointer(&log->hFile, fh);

    return 0;
}

static DWORD rotatelogs(LPSVCBATCH_LOG log)
{
    DWORD  rc = 0;
    LONG   nr = 0;
    HANDLE h  = NULL;
    HANDLE s  = NULL;

    SVCBATCH_CS_ENTER(log);
    InterlockedExchange(&log->dwCurrentState, 1);

    if (svcbstatlog) {
        SVCBATCH_CS_ENTER(svcbstatlog);
        s = InterlockedExchangePointer(&svcbstatlog->hFile, NULL);
        logwrline(s, 0, "Rotating");
    }
    h = InterlockedExchangePointer(&log->hFile, NULL);
    if (h == NULL) {
        rc = ERROR_FILE_NOT_FOUND;
        goto finished;
    }
    nr = InterlockedIncrement(&log->nRotateCount);
    FlushFileBuffers(h);
    if (s) {
        LARGE_INTEGER sz;

        logwransi(s, 0, "  ", log->lpFileName);
        if (GetFileSizeEx(h, &sz))
        logprintf(s, 0, "  size           : %lld", sz.QuadPart);
        if (rotatebysize)
        logprintf(s, 0, "  rotate size    : %lld", rotatesiz.QuadPart);
        logprintf(s, 0, "Log generation   : %lu", nr);
    }
    if (IS_SET(SVCBATCH_OPT_TRUNCATE)) {
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (SetFilePointerEx(h, ee, NULL, FILE_BEGIN)) {
            if (SetEndOfFile(h)) {
                logwrtime(s, 0, "Log truncated");
                InterlockedExchangePointer(&log->hFile, h);
                goto finished;
            }
        }
        rc = GetLastError();
        xsyserror(rc, log->lpFileName, NULL);
        CloseHandle(h);
    }
    else {
        CloseHandle(h);
        SAFE_MEM_FREE(log->lpFileName);
        rc = openlogfile(log, FALSE, s);
    }
    if (rc)
        setsvcstatusexit(rc);
    logwrtime(s, 0, "Log rotated");

finished:
    if (svcbstatlog) {
        InterlockedExchangePointer(&svcbstatlog->hFile, s);
        InterlockedExchange(&svcbstatlog->nRotateCount, 0);
        SVCBATCH_CS_LEAVE(svcbstatlog);
    }
    InterlockedExchange64(&log->nWritten, 0);
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
    DBG_PRINTF("%d %S", log->dwCurrentState, log->lpFileName);
    InterlockedExchange(&log->dwCurrentState, 1);
    if (svcbstatlog && (log != svcbstatlog)) {

        SVCBATCH_CS_ENTER(svcbstatlog);
        s = InterlockedExchangePointer(&svcbstatlog->hFile, NULL);
    }

    h = InterlockedExchangePointer(&log->hFile, NULL);
    if (h) {
        if (log == svcbstatlog)
            logwrtime(h, 0, "Status closed");
        if (s) {
            logwrline(s, 0, "Closing");
            logwransi(s, 0, "  ", log->lpFileName);
        }
        FlushFileBuffers(h);
        CloseHandle(h);
        if (s)
            logwrtime(s, 0, "Log closed");
    }
    if (svcbstatlog && (log != svcbstatlog)) {
        InterlockedExchangePointer(&svcbstatlog->hFile, s);
        SVCBATCH_CS_LEAVE(svcbstatlog);
    }

    xfree(log->lpFileName);
    SVCBATCH_CS_LEAVE(log);
    SVCBATCH_CS_CLOSE(log);
    SAFE_MEM_FREE(log);
    DBG_PRINTS("done");
    return 0;
}

static void resolvetimeout(int hh, int mm, int ss, int od)
{
    SYSTEMTIME     st;
    FILETIME       ft;
    ULARGE_INTEGER ui;

    rotateint = od ? ONE_DAY : ONE_HOUR;
    if (IS_SET(SVCBATCH_OPT_LOCALTIME) && od)
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
    wchar_t  rb[TBUFSIZ];
    DWORD i, rc = 0;

    DBG_PRINTS("started");
    i = xwcslcpy(rb, TBUFSIZ, L"@@" SVCBATCH_MMAPPFX);
    xuuidstring(rb + i);
    shutdownmmap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                      PAGE_READWRITE, 0,
                                      DSIZEOF(SVCBATCH_IPC), rb + 2);
    if (shutdownmmap == NULL)
        return GetLastError();
    shutdownmem = (LPSVCBATCH_IPC)MapViewOfFile(
                                    shutdownmmap,
                                    FILE_MAP_ALL_ACCESS,
                                    0, 0, DSIZEOF(SVCBATCH_IPC));
    if (shutdownmem == NULL)
        return GetLastError();
    ZeroMemory(shutdownmem, sizeof(SVCBATCH_IPC));
    shutdownmem->dwProcessId = GetCurrentProcessId();
    shutdownmem->dwOptions   = svcoptions & 0x0000000F;
    shutdownmem->dwTimeout   = stoptimeout;
    shutdownmem->dwCodePage  = svccodepage;
    if (svcbatchlog)
        xwcslcpy(shutdownmem->szLogName, _MAX_FNAME,    svcbatchlog->szName);
    xwcslcpy(shutdownmem->szServiceName, _MAX_FNAME,    mainservice->lpName);
    xwcslcpy(shutdownmem->szHomeDir, SVCBATCH_PATH_MAX, mainservice->lpHome);
    xwcslcpy(shutdownmem->szWorkDir, SVCBATCH_PATH_MAX, mainservice->lpWork);
    xwcslcpy(shutdownmem->szLogsDir, SVCBATCH_PATH_MAX, mainservice->lpLogs);
    xwcslcpy(shutdownmem->szUuid,    TBUFSIZ,           mainservice->lpUuid);
    xwcslcpy(shutdownmem->szBatch,   SVCBATCH_PATH_MAX, svcstopproc->lpArgv[0]);
    xwcslcpy(shutdownmem->szShell,   SVCBATCH_PATH_MAX, svcxcmdproc->lpExe);
    shutdownmem->nArgc = svcstopproc->nArgc;
    for (i = 1; i < svcstopproc->nArgc; i++)
        wmemcpy(shutdownmem->lpArgv[i], svcstopproc->lpArgv[i], _MAX_FNAME);

    rc = createiopipes(&svcstopproc->sInfo, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }
    svcstopproc->lpExe = svcmainproc->lpExe;
    svcstopproc->lpCommandLine = xappendarg(1, NULL, NULL, svcstopproc->lpExe);
    svcstopproc->lpCommandLine = xappendarg(0, svcstopproc->lpCommandLine, NULL,  rb);
    DBG_PRINTF("cmdline %S", svcstopproc->lpCommandLine);
    svcstopproc->dwCreationFlags = CREATE_UNICODE_ENVIRONMENT |
                                   CREATE_NEW_CONSOLE;
    if (!CreateProcessW(svcstopproc->lpExe,
                        svcstopproc->lpCommandLine,
                        NULL, NULL, TRUE,
                        svcstopproc->dwCreationFlags,
                        NULL, NULL,
                        &svcstopproc->sInfo,
                        &svcstopproc->pInfo)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", svcstopproc->lpExe);
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
        SVCBATCH_CS_LEAVE(svcbatchlog);
        if (svcbstatlog && msg)
            logwrstat(svcbstatlog, 2, 0, msg);
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
    if (servicemode) {
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


static DWORD logwrdata(LPSVCBATCH_LOG log, BYTE *buf, DWORD len)
{
    DWORD  rc;
    HANDLE h;

#if defined(_DEBUG) && (_DEBUG > 2)
    DBG_PRINTF("writing %4lu bytes", len);
#endif
    SVCBATCH_CS_ENTER(log);
    h = InterlockedExchangePointer(&log->hFile, NULL);

    if (h)
        rc = logappend(h, buf, len);
    else
        rc = ERROR_NO_MORE_FILES;

    InterlockedExchangePointer(&log->hFile, h);
    SVCBATCH_CS_LEAVE(log);
    if (rc) {
        if (rc != ERROR_NO_MORE_FILES) {
            xsyserror(rc, L"Log write", NULL);
        }
#if defined(_DEBUG)
        else {
            DBG_PRINTS("logfile closed");
        }
#endif
        return rc;
    }
#if defined(_DEBUG) && (_DEBUG > 2)
    DBG_PRINTF("wrote   %4lu bytes", len);
#endif
    if (IS_SET(SVCBATCH_OPT_ROTATE) && rotatebysize) {
        if (InterlockedCompareExchange64(&log->nWritten, 0, 0) >= rotatesiz.QuadPart) {
            if (canrotatelogs(log)) {
                DBG_PRINTS("rotating by size");
                logwrstat(svcbstatlog, 0, 1, "Rotate by size");
                SetEvent(logrotatesig);
            }
        }
    }
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

static DWORD WINAPI signalthread(void *unused)
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
                logwrstat(svcbstatlog, 2, 0, "Service signaled : SVCBATCH_CTRL_BREAK");
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
        DWORD wc;

        wc = WaitForMultipleObjects(nw, wh, FALSE, rw);
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
                rc = rotatelogs(svcbatchlog);
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
                logwrstat(svcbstatlog, 0, 1, "Rotate by time");
                if (canrotatelogs(svcbatchlog)) {
                    DBG_PRINTS("rotate by time");
                    rc = rotatelogs(svcbatchlog);
                    rw = SVCBATCH_ROTATE_READY;
                }
                else {
                    DBG_PRINTS("rotate is busy ... canceling timer");
                    if (InterlockedCompareExchange64(&svcbatchlog->nWritten, 0, 0))
                        logwrstat(svcbstatlog, 0, 0, "Canceling timer  : Rotate busy");
                    else
                        logwrstat(svcbstatlog, 0, 0, "Canceling timer  : Log empty");
                }
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
                logwrstat(svcbstatlog, 1, 1, "Rotate ready");
                if (rotatebysize) {
                    if (InterlockedCompareExchange64(&svcbatchlog->nWritten, 0, 0) >= rotatesiz.QuadPart) {
                        InterlockedExchange(&svcbatchlog->dwCurrentState, 1);
                        DBG_PRINTS("rotating by size");
                        logwrstat(svcbstatlog, 0, 0, "Rotating by size");
                        SetEvent(logrotatesig);
                    }
                }
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

    if (!xcreatethread(SVCBATCH_WRPIPE_THREAD,
                       1, wrpipethread, wr)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        xsyserror(rc, L"Write thread", NULL);
        TerminateProcess(svcxcmdproc->pInfo.hProcess, rc);
        svcxcmdproc->dwExitCode = rc;
        goto finished;
    }
    if (IS_SET(SVCBATCH_OPT_ROTATE)) {
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
        if (!xcreatethread(SVCBATCH_SIGNAL_THREAD,
                           1, signalthread, NULL)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"Monitor thread", NULL);
            TerminateProcess(svcxcmdproc->pInfo.hProcess, rc);
            svcxcmdproc->dwExitCode = rc;
        }
    }

    ResumeThread(svcxcmdproc->pInfo.hThread);
    ResumeThread(svcthread[SVCBATCH_WRPIPE_THREAD].hThread);
    InterlockedExchange(&svcxcmdproc->dwCurrentState, SVCBATCH_PROCESS_RUNNING);

    SAFE_CLOSE_HANDLE(svcxcmdproc->pInfo.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    if (ctrlbreaksig)
        ResumeThread(svcthread[SVCBATCH_SIGNAL_THREAD].hThread);
    if (IS_SET(SVCBATCH_OPT_ROTATE))
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
                            rc = logwrdata(svcbatchlog, op.bBuffer, op.nRead);
                        }
                    }
                    else {
                        if (ReadFile(op.hPipe, op.bBuffer, DSIZEOF(op.bBuffer),
                                    &op.nRead, (LPOVERLAPPED)&op) && op.nRead) {
                            op.dwState = 0;
                            rc = logwrdata(svcbatchlog, op.bBuffer, op.nRead);
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
    DBG_PRINTS("stopping");
    InterlockedExchange(&svcxcmdproc->dwCurrentState, SVCBATCH_PROCESS_STOPPING);
    if (WaitForSingleObject(svcthread[SVCBATCH_WRPIPE_THREAD].hThread, SVCBATCH_STOP_STEP)) {
        DBG_PRINTS("wrpipethread is still active ... calling CancelSynchronousIo");
        CancelSynchronousIo(svcthread[SVCBATCH_WRPIPE_THREAD].hThread);
        WaitForSingleObject(svcthread[SVCBATCH_WRPIPE_THREAD].hThread, SVCBATCH_STOP_STEP);
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
    SAFE_CLOSE_HANDLE(op.hPipe);
    SAFE_CLOSE_HANDLE(op.oOverlap.hEvent);
    xclearprocess(svcxcmdproc);

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
            msg = "Service signaled : SERVICE_CONTROL_PRESHUTDOWN";
        break;
        case SERVICE_CONTROL_SHUTDOWN:
            msg = "Service signaled : SERVICE_CONTROL_SHUTDOWN";
        break;
        case SERVICE_CONTROL_STOP:
            msg = "Service signaled : SERVICE_CONTROL_STOP";
        break;
        case SVCBATCH_CTRL_ROTATE:
            msg = "Service signaled : SVCBATCH_CTRL_ROTATE";
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
            if (IS_SET(SVCBATCH_OPT_ROTATE)) {
                BOOL rb = canrotatelogs(svcbatchlog);
                /**
                 * Signal to rotatethread that
                 * user send custom service control
                 */
                logwrstat(svcbstatlog, 1, 0, msg);
                if (rb) {
                    DBG_PRINTS("signaling SVCBATCH_CTRL_ROTATE");
                    SetEvent(logrotatesig);
                }
                else {
                    DBG_PRINTS("rotatelogs is busy");
                    if (InterlockedCompareExchange64(&svcbatchlog->nWritten, 0, 0))
                        logwrstat(svcbstatlog, 0, 1, "Log is busy");
                    else
                        logwrstat(svcbstatlog, 0, 1, "Log is empty");
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
            DBG_PRINTF("svcthread[%d] %4lu %10llums", i,
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
    if (shutdownmem)
        UnmapViewOfFile(shutdownmem);
    SAFE_CLOSE_HANDLE(shutdownmmap);

    SVCBATCH_CS_CLOSE(mainservice);
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
    if (IS_SET(SVCBATCH_OPT_BREAK)) {
        ctrlbreaksig = CreateEventEx(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(ctrlbreaksig))
            return GetLastError();
    }
    if (IS_SET(SVCBATCH_OPT_ROTATE)) {
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
    DWORD  rv = 0;
    DWORD  i;
    HANDLE sh = NULL;

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
        rv = createlogsdir(svcbatchlog);
        if (rv) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        SetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS", svcbatchlog->szDir);
        if (IS_SET(SVCBATCH_OPT_VERBOSE)) {
            svcbstatlog = (LPSVCBATCH_LOG)xmmalloc(sizeof(SVCBATCH_LOG));

            memcpy(svcbstatlog, svcbatchlog, sizeof(SVCBATCH_LOG));
            svcbstatlog->nMaxLogs  = SVCBATCH_DEF_LOGS;
            svcbstatlog->lpFileExt = SBSTATUS_LOGFEXT;
            SVCBATCH_CS_INIT(svcbstatlog);
        }
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
    SetEnvironmentVariableW(L"SVCBATCH_APP_DIR",      svcmainproc->lpDir);
    SetEnvironmentVariableW(L"SVCBATCH_APP_VER",      SVCBATCH_VERSION_VER);

    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_BASE", mainservice->lpBase);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_HOME", mainservice->lpHome);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_NAME", mainservice->lpName);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_UUID", mainservice->lpUuid);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_WORK", mainservice->lpWork);

    if (svcbstatlog) {
        rv = openlogfile(svcbstatlog, TRUE, NULL);
        if (rv != 0) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        sh = svcbstatlog->hFile;
        logconfig(sh);
    }
    if (svcbatchlog) {
        reportsvcstatus(SERVICE_START_PENDING, 0);

        rv = openlogfile(svcbatchlog, TRUE, sh);
        if (rv != 0) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
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
    xclearprocess(svcxcmdproc);
    closelogfile(svcbatchlog);
    closelogfile(svcbstatlog);
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
        rv = openlogfile(svcbatchlog, FALSE, NULL);
        if (rv != 0)
            return rv;
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

    closelogfile(svcbatchlog);
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

    xmemzero(svcthread, SVCBATCH_MAX_THREADS,     sizeof(SVCBATCH_THREAD));
    mainservice = (LPSVCBATCH_SERVICE)xmcalloc(1, sizeof(SVCBATCH_SERVICE));
    svcmainproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));

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
    SVCBATCH_CS_INIT(mainservice);

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
    const wchar_t *codepageopt  = NULL;
    const wchar_t *svclogfname  = NULL;
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
            if (p[1] == L'@') {
                DWORD x;
                servicemode  = FALSE;
                shutdownmmap = OpenFileMappingW(FILE_MAP_ALL_ACCESS,
                                                FALSE, p + 2);
                if (shutdownmmap == NULL)
                    return GetLastError();
                shutdownmem = (LPSVCBATCH_IPC)MapViewOfFile(
                                                shutdownmmap,
                                                FILE_MAP_ALL_ACCESS,
                                                0, 0, DSIZEOF(SVCBATCH_IPC));
                if (shutdownmem == NULL)
                    return GetLastError();
                cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT;
                wnamestamp  = CPP_WIDEN(SHUTDOWN_APPNAME) L" " SVCBATCH_VERSION_WCS;
                cwsappname  = CPP_WIDEN(SHUTDOWN_APPNAME);
                stoptimeout = shutdownmem->dwTimeout;
                svcoptions  = shutdownmem->dwOptions;
                svccodepage = shutdownmem->dwCodePage;
#if defined(_DEBUG)
                dbgsvcmode = '1';
                dbgprints(__FUNCTION__, cnamestamp);
                dbgprintf(__FUNCTION__, "ppid %lu", shutdownmem->dwProcessId);
                dbgprintf(__FUNCTION__, "opts 0x%08x", shutdownmem->dwOptions);
                dbgprintf(__FUNCTION__, "time %lu", stoptimeout);
#endif
                svcxcmdproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
                svcxcmdproc->dwType = SVCBATCH_SHELL_PROCESS;
                svcxcmdproc->lpExe  = shutdownmem->szShell;
                svcxcmdproc->nArgc  = shutdownmem->nArgc;
                svcxcmdproc->lpArgv[0] = shutdownmem->szBatch;
                for (x = 1; x < svcxcmdproc->nArgc; x++)
                    svcxcmdproc->lpArgv[x] = shutdownmem->lpArgv[x];
                mainservice->lpHome = shutdownmem->szHomeDir;
                mainservice->lpWork = shutdownmem->szWorkDir;
                mainservice->lpName = shutdownmem->szServiceName;
                mainservice->lpUuid = shutdownmem->szUuid;
                if (IS_NOT(SVCBATCH_OPT_QUIET)) {
                    svcbatchlog = (LPSVCBATCH_LOG)xmcalloc(1, sizeof(SVCBATCH_LOG));

                    xwcslcpy(svcbatchlog->szName, _MAX_FNAME, shutdownmem->szLogName);
                    svcbatchlog->nMaxLogs  = SVCBATCH_DEF_LOGS;
                    svcbatchlog->lpFileExt = SHUTDOWN_LOGFEXT;
                    SVCBATCH_CS_INIT(svcbatchlog);
                }
            }
            else {
                mainservice->lpName = xwcsdup(p + 1);
                if (IS_EMPTY_WCS(mainservice->lpName))
                    return ERROR_INVALID_PARAMETER;
            }
            wargv[1] = wargv[0];
            argc    -= 1;
            wargv   += 1;
        }
    }
    if (servicemode)
        svcmainproc->dwType = SVCBATCH_SERVICE_PROCESS;
    else
        svcmainproc->dwType = SVCBATCH_SHUTDOWN_PROCESS;

#if defined(_DEBUG)
    if (servicemode) {
        dbgsvcmode = '0';
        dbgprints(__FUNCTION__, cnamestamp);
    }
#endif
    if (servicemode) {
        DWORD nn;
        WCHAR cb[SVCBATCH_PATH_MAX];
        svcxcmdproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
        svcxcmdproc->dwType = SVCBATCH_SHELL_PROCESS;
        /* Reserve lpArgv[0] for batch file */
        svcxcmdproc->nArgc  = 1;
        nn = GetEnvironmentVariableW(L"COMSPEC", cb, SVCBATCH_PATH_MAX);
        if ((nn == 0) || (nn >= SVCBATCH_PATH_MAX))
            return GetLastError();
        svcxcmdproc->lpExe = xgetfinalpath(cb, 0, NULL, 0);
        ASSERT_WSTR(svcxcmdproc->lpExe, ERROR_BAD_PATHNAME);


        while ((opt = xwgetopt(argc, wargv, L"bc:h:k:lm:n:o:pqr:s:tvw:")) != EOF) {
            switch (opt) {
                case L'b':
                    svcoptions  |= SVCBATCH_OPT_BREAK;
                break;
                case L'l':
                    svcoptions  |= SVCBATCH_OPT_LOCALTIME;
                break;
                case L'p':
                    preshutdown  = SERVICE_ACCEPT_PRESHUTDOWN;
                break;
                case L't':
                    svcoptions  |= SVCBATCH_OPT_TRUNCATE;
                break;
                case L'v':
                    svcoptions  |= SVCBATCH_OPT_VERBOSE;
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
                    if (wcspbrk(svclogfname, L"/\\:;<>?*|\""))
                        return xsyserror(0, L"Found invalid filename characters", svclogfname);

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
                    svcoptions |= SVCBATCH_OPT_QUIET;

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
        }
        else {
            batchparam = wargv[0];
            for (i = 1; i < argc; i++) {
                /**
                 * Add arguments for batch file
                 */
                if (xwcslen(wargv[i]) >= _MAX_FNAME)
                    return xsyserror(0, L"The argument is too large", wargv[i]);

                if (i < SVCBATCH_MAX_ARGS)
                    svcxcmdproc->lpArgv[i] = xwcsdup(wargv[i]);
                else
                    return xsyserror(0, L"Too many batch arguments", wargv[i]);
            }
            svcxcmdproc->nArgc = i;
        }
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
            const wchar_t *logn = svclogfname ? svclogfname : SVCBATCH_LOGNAME;
            svcbatchlog = (LPSVCBATCH_LOG)xmcalloc(1, sizeof(SVCBATCH_LOG));

            xwcslcpy(svcbatchlog->szName, _MAX_FNAME, logn);
            svcbatchlog->nMaxLogs  = SVCBATCH_MAX_LOGS;
            svcbatchlog->lpFileExt = SVCBATCH_LOGFEXT;
            if (maxlogsparam) {
                svcbatchlog->nMaxLogs = xwcstoi(maxlogsparam, NULL);
                if ((svcbatchlog->nMaxLogs < 1) || (svcbatchlog->nMaxLogs > SVCBATCH_MAX_LOGS))
                    return xsyserror(0, L"Invalid -m command option value", maxlogsparam);
            }
            SVCBATCH_CS_INIT(svcbatchlog);
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
            if (IS_SET(SVCBATCH_OPT_TRUNCATE))
                xwcslcat(bb, TBUFSIZ, L"-t ");
            if (IS_SET(SVCBATCH_OPT_VERBOSE))
                xwcslcat(bb, TBUFSIZ, L"-v ");
            if (bb[0]) {
#if defined(_DEBUG) && (_DEBUG > 1)
                xsyswarn(0, L"Option -q is mutually exclusive with option(s)", bb);
                rcnt       = 0;
                svcoptions = 0;
#else
                return xsyserror(0, L"Option -q is mutually exclusive with option(s)", bb);
#endif
            }
        }
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
        }
        if (!resolvebatchname(batchparam))
            return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);
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
            if (rotatebysize || rotatebytime)
                svcbatchlog->nMaxLogs = 0;

            svcoptions |= SVCBATCH_OPT_ROTATE;
        }
        if (svclogfname) {
            if (wcschr(svcbatchlog->szName, L'@')) {
                /**
                 * If name is strftime formatted
                 * replace @ with % so it can be used by strftime
                 */
                xwchreplace(svcbatchlog->szName, L'@', L'%');
            }
        }
        if (scnt) {
            svcstopproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
            svcstopproc->lpArgv[0] = xgetfinalpath(sparam[0], 0, NULL, 0);
            if (svcstopproc->lpArgv[0] == NULL)
                return xsyserror(ERROR_FILE_NOT_FOUND, sparam[0], NULL);
            for (i = 1; i < scnt; i++)
                svcstopproc->lpArgv[i] = xwcsdup(sparam[i]);
            svcstopproc->nArgc  = scnt;
            svcstopproc->dwType = SVCBATCH_SHUTDOWN_PROCESS;
        }
        if (mainservice->lpHome != mainservice->lpWork)
            SetCurrentDirectoryW(mainservice->lpHome);
    } /* servicemode */

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

    if (servicemode) {
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
    DBG_PRINTF("done %lu", rv);
#endif
    return rv;
}
