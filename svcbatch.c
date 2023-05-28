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

typedef enum {
    SVCBATCH_WORKER_THREAD = 0,
    SVCBATCH_WRPIPE_THREAD,
    SVCBATCH_STOP_THREAD,
    SVCBATCH_ROTATE_THREAD,
    SVCBATCH_MAX_THREADS
} SVCBATCH_THREAD_ID;

typedef struct _SVCBATCH_THREAD {
    volatile LONG          started;
    LPTHREAD_START_ROUTINE startAddress;
    HANDLE                 thread;
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
    LPWSTR              commandLine;
    LPWSTR              application;
    LPWSTR              directory;
    LPWSTR              args[SVCBATCH_MAX_ARGS];
} SVCBATCH_PROCESS, *LPSVCBATCH_PROCESS;

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
    WCHAR                   logs[SVCBATCH_PATH_MAX];

} SVCBATCH_SERVICE, *LPSVCBATCH_SERVICE;

typedef struct _SVCBATCH_IPC {
    DWORD   processId;
    DWORD   options;
    DWORD   timeout;
    DWORD   argc;
    DWORD   killdepth;
    WCHAR   uuid[SVCBATCH_UUID_MAX];
    WCHAR   name[SVCBATCH_NAME_MAX];
    WCHAR   logName[SVCBATCH_NAME_MAX];
    WCHAR   home[SVCBATCH_PATH_MAX];
    WCHAR   work[SVCBATCH_PATH_MAX];
    WCHAR   logs[SVCBATCH_PATH_MAX];
    WCHAR   application[SVCBATCH_PATH_MAX];
    WCHAR   batchFile[SVCBATCH_PATH_MAX];
    WCHAR   args[SVCBATCH_MAX_ARGS][SVCBATCH_NAME_MAX];

} SVCBATCH_IPC, *LPSVCBATCH_IPC;

static LPSVCBATCH_PROCESS    program     = NULL;
static LPSVCBATCH_SERVICE    service     = NULL;

static LPSVCBATCH_PROCESS    svcstop     = NULL;
static LPSVCBATCH_PROCESS    cmdproc     = NULL;
static LPSVCBATCH_LOG        outputlog   = NULL;
static LPSVCBATCH_LOG        statuslog   = NULL;
static LPSVCBATCH_IPC        sharedmem   = NULL;

static volatile LONG         killdepth   = 4;

static LONGLONG              counterbase    = CPP_INT64_C(0);
static LONGLONG              counterfreq    = CPP_INT64_C(0);
static LONGLONG              rotateinterval = CPP_INT64_C(0);
static LONGLONG              rotatesize     = CPP_INT64_C(0);
static LARGE_INTEGER         rotatetime     = {{ 0, 0 }};

static BOOL      rotatebysize   = FALSE;
static BOOL      rotatebytime   = FALSE;
static BOOL      rotatebysignal = FALSE;
static BOOL      servicemode    = TRUE;

static DWORD     svcoptions     = 0;
static DWORD     preshutdown    = 0;
static int       stoptimeout    = SVCBATCH_STOP_TIMEOUT;

static HANDLE    stopstarted    = NULL;
static HANDLE    svcstopdone    = NULL;
static HANDLE    workerended    = NULL;
static HANDLE    dologrotate    = NULL;
static HANDLE    sharedmmap     = NULL;

static SVCBATCH_THREAD threads[SVCBATCH_MAX_THREADS];

static LPWSTR       svclogfname   = NULL;
static WCHAR        zerostring[2] = { WNUL, WNUL };
static LPCWSTR      CRLFW         = L"\r\n";
static LPCSTR       CRLFA         =  "\r\n";
static LPCSTR       YYES          = "Y\r\n";

static LPCSTR  cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;
static LPCWSTR wnamestamp  = CPP_WIDEN(SVCBATCH_NAME) L" " SVCBATCH_VERSION_WCS;
static LPCWSTR cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);
static LPCWSTR outdirparam = NULL;

static int     xwoptind    = 1;
static WCHAR   xwoption    = WNUL;
static LPCWSTR xwoptarg    = NULL;

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

static __inline LPWSTR xwmalloc(size_t size)
{
    return (LPWSTR )xmmalloc(size * sizeof(WCHAR));
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

#if defined(_DEBUG) && (_DEBUG > 1)
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
#else
static __inline LPWSTR xwcsdup(LPCWSTR s)
{
    if (IS_EMPTY_WCS(s))
        return NULL;
    return _wcsdup(s);
}
#endif

static void xwchreplace(LPWSTR s, WCHAR c, WCHAR r)
{
    LPWSTR d;

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

static LPWSTR xgetenv(LPCWSTR s)
{
    DWORD  n;
    WCHAR  e[BBUFSIZ];
    LPWSTR d = NULL;

    n = GetEnvironmentVariableW(s, e, BBUFSIZ);
    if (n != 0) {
        if (n >= BBUFSIZ) {
            d = xwmalloc(n);
            n = GetEnvironmentVariableW(s, d, n);
            if (n == 0) {
                xfree(d);
                return NULL;
            }
        }
        else {
            d = xwcsdup(e);
        }
    }
    return d;
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
    while(iswblank(*sp))
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

static LPWSTR xappendarg(int nq, LPWSTR s1, LPCWSTR s2, LPCWSTR s3)
{
    LPCWSTR c;
    LPWSTR  e;
    LPWSTR  d;

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
    e  = (LPWSTR )xrealloc(s1, nn * sizeof(WCHAR));
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

static int xwstartswith(LPCWSTR str, LPCWSTR src)
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


static int xwgetopt(int nargc, LPCWSTR *nargv, LPCWSTR opts)
{
    static LPCWSTR place = zerostring;
    LPCWSTR oli = NULL;

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
        xwoptind++;
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
            while (iswblank(*place))
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
        w = LOWORD(GetCurrentProcessId());
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

static LPWSTR xmktimedext(void)
{
    static WCHAR b[TBUFSIZ];
    SYSTEMTIME st;

    if (IS_SET(SVCBATCH_OPT_LOCALTIME))
        GetLocalTime(&st);
    else
        GetSystemTime(&st);
    xsnwprintf(b, TBUFSIZ, L".%.4d%.2d%.2d%.2d%.2d%.2d",
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

    return xsnprintf(wb, sz, "%.2lu:%.2lu:%.2lu.%.6lu",
                     hh, mm, ss, us);
}

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
    static volatile LONG eset = 0;
    static const WCHAR emsg[] = L"%SystemRoot%\\System32\\netmsg.dll\0";
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
    DBG_PRINTF("%lu ended %lu", p->id, p->exitCode);
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

static BOOL resolvebatchname(LPCWSTR bp)
{
    LPWSTR p;

    if (cmdproc->args[0])
        return TRUE;
    cmdproc->args[0] = xgetfinalpath(bp, 0, NULL, 0);
    if (IS_EMPTY_WCS(cmdproc->args[0]))
        return FALSE;
    p = wcsrchr(cmdproc->args[0], L'\\');
    if (p) {
        *p = WNUL;
        service->base = xwcsdup(cmdproc->args[0]);
        *p = L'\\';
        return TRUE;
    }
    else {
        SAFE_MEM_FREE(cmdproc->args[0]);
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
        InterlockedExchange(&cpcnt, 0);
    }
    else if (status == SERVICE_STOPPED) {
        if (service->status.dwCurrentState != SERVICE_STOP_PENDING) {
            xsyserror(param, L"Service stopped without SERVICE_CONTROL_STOP signal", NULL);
            exit(1);
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
    WCHAR  name[SVCBATCH_UUID_MAX];
    SECURITY_ATTRIBUTES sa;

    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    xwcslcpy(name, SVCBATCH_UUID_MAX, SVCBATCH_PIPEPFX);
    xuuidstring(name + _countof(SVCBATCH_PIPEPFX) - 1);

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
    logwrline(h, 0, cnamestamp);
    logwinver(h);
    logwwrite(h, 1, "Service name     : ", service->name);
    logwwrite(h, 0, "Service uuid     : ", service->uuid);
    logwwrite(h, 0, "Batch file       : ", cmdproc->args[0]);
    if (svcstop)
        logwwrite(h, 0, "Shutdown batch   : ", svcstop->args[0]);
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
    WCHAR  dp[SVCBATCH_PATH_MAX];

    if (xgetfullpath(outdirparam, dp, SVCBATCH_PATH_MAX) == NULL) {
        xsyserror(0, L"xgetfullpath", outdirparam);
        return ERROR_BAD_PATHNAME;
    }
    p = xgetfinalpath(dp, 1, service->logs, SVCBATCH_PATH_MAX);
    if (p == NULL) {
        DWORD rc = GetLastError();

        if (rc > ERROR_PATH_NOT_FOUND)
            return xsyserror(rc, L"xgetfinalpath", dp);

        rc = xcreatedir(dp);
        if (rc != 0)
            return xsyserror(rc, L"xcreatedir", dp);
        p = xgetfinalpath(dp, 1, service->logs, SVCBATCH_PATH_MAX);
        if (p == NULL)
            return xsyserror(GetLastError(), L"xgetfinalpath", dp);
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

    xwcslcpy(lognn, SVCBATCH_PATH_MAX, log->logFile);
    if (log->maxLogs > 1)
        x = xwcslcat(lognn, SVCBATCH_PATH_MAX, L".0");
    else
        x = xwcslcat(lognn, SVCBATCH_PATH_MAX, xmktimedext());
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

static DWORD makelogname(LPWSTR dst, int siz, LPCWSTR src)
{
    struct tm *ctm;
    time_t ctt;
    int    i, x;

    time(&ctt);
    if (IS_SET(SVCBATCH_OPT_LOCALTIME))
        ctm = localtime(&ctt);
    else
        ctm = gmtime(&ctt);
    if (wcsftime(dst, siz, src, ctm) == 0)
        return xsyserror(0, L"Invalid format code", src);

    for (i = 0, x = 0; dst[i]; i++) {
        WCHAR c = dst[i];

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
    if (wcschr(np, L'%')) {
        rc = makelogname(nb, SVCBATCH_NAME_MAX, np);
        if (rc)
            return rc;
        rp = FALSE;
        np = nb;
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
                     FILE_SHARE_READ, NULL,
                     CREATE_ALWAYS,
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
#if defined(_DEBUG)
    if (rc == ERROR_ALREADY_EXISTS)
        dbgprintf(ssp ? "createlogfile" : "openlogfile",
                  "truncated %S", log->logFile);
    else
        dbgprintf(ssp ? "createlogfile" : "openlogfile",
                  "created %S",   log->logFile);
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
    DBG_PRINTS("done");
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
    if (wcspbrk(rp, L"BKMG")) {
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
    i = xwcslcpy(rb, SVCBATCH_UUID_MAX, L"@@" SVCBATCH_MMAPPFX);
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
    sharedmem->processId = GetCurrentProcessId();
    sharedmem->options   = svcoptions & 0x0000000F;
    sharedmem->timeout   = stoptimeout;
    sharedmem->killdepth = killdepth;
    if (outputlog)
        xwcslcpy(sharedmem->logName, SVCBATCH_NAME_MAX, outputlog->logName);
    xwcslcpy(sharedmem->name, SVCBATCH_NAME_MAX, service->name);
    xwcslcpy(sharedmem->home, SVCBATCH_PATH_MAX, service->home);
    xwcslcpy(sharedmem->work, SVCBATCH_PATH_MAX, service->work);
    xwcslcpy(sharedmem->logs, SVCBATCH_PATH_MAX, service->logs);
    xwcslcpy(sharedmem->uuid, SVCBATCH_UUID_MAX, service->uuid);
    xwcslcpy(sharedmem->batchFile,   SVCBATCH_PATH_MAX, svcstop->args[0]);
    xwcslcpy(sharedmem->application, SVCBATCH_PATH_MAX, cmdproc->application);
    sharedmem->argc = svcstop->argc;
    for (i = 1; i < svcstop->argc; i++)
        wmemcpy(sharedmem->args[i], svcstop->args[i], SVCBATCH_NAME_MAX);

    rc = createiopipes(&svcstop->sInfo, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }
    svcstop->application = program->application;
    svcstop->commandLine = xappendarg(1, NULL, NULL, svcstop->application);
    svcstop->commandLine = xappendarg(0, svcstop->commandLine, NULL,  rb);
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
    if (ws != WAIT_OBJECT_0) {
        reportsvcstatus(SERVICE_STOP_PENDING, 0);
        SetConsoleCtrlHandler(NULL, TRUE);
        if (IS_SET(SVCBATCH_OPT_BREAK)) {
            DBG_PRINTS("generating CTRL_BREAK_EVENT");
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
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
    if (IS_SET(SVCBATCH_OPT_BREAK))
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
    else
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
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

static DWORD WINAPI workerthread(void *unused)
{
    DWORD    i;
    HANDLE   wr = NULL;
    HANDLE   rd = NULL;
    LPHANDLE rp = NULL;
    DWORD    rc = 0;
    LPSVCBATCH_PIPE op = NULL;

    DBG_PRINTS("started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_STARTING);

    if (outputlog)
        rp = &rd;
    rc = createiopipes(&cmdproc->sInfo, &wr, rp, FILE_FLAG_OVERLAPPED);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        setsvcstatusexit(rc);
        cmdproc->exitCode = rc;
        goto finished;
    }
    cmdproc->commandLine = xappendarg(1, NULL, NULL, cmdproc->application);
    cmdproc->commandLine = xappendarg(1, cmdproc->commandLine,
                                          SVCBATCH_DEF_ARGS  L" /C",
                                          cmdproc->args[0]);
    for (i = 1; i < cmdproc->argc; i++) {
        xwchreplace(cmdproc->args[i], L'@', L'%');
        cmdproc->commandLine = xappendarg(1, cmdproc->commandLine, NULL, cmdproc->args[i]);
    }
    if (outputlog) {
        op = (LPSVCBATCH_PIPE)xmcalloc(1, sizeof(SVCBATCH_PIPE));
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
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    DBG_PRINTF("cmdline %S", cmdproc->commandLine);
    if (!CreateProcessW(cmdproc->application,
                        cmdproc->commandLine,
                        NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        NULL,
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

    if (!xcreatethread(SVCBATCH_WRPIPE_THREAD,
                       1, wrpipethread, wr)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        xsyserror(rc, L"Write thread", NULL);
        TerminateProcess(cmdproc->pInfo.hProcess, rc);
        cmdproc->exitCode = rc;
        goto finished;
    }
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

    ResumeThread(cmdproc->pInfo.hThread);
    ResumeThread(threads[SVCBATCH_WRPIPE_THREAD].thread);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_RUNNING);

    SAFE_CLOSE_HANDLE(cmdproc->pInfo.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    if (IS_SET(SVCBATCH_OPT_ROTATE))
        ResumeThread(threads[SVCBATCH_ROTATE_THREAD].thread);
    DBG_PRINTF("running process %lu", cmdproc->pInfo.dwProcessId);
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
    if (WaitForSingleObject(threads[SVCBATCH_WRPIPE_THREAD].thread, SVCBATCH_STOP_STEP)) {
        DBG_PRINTS("wrpipethread is still active ... calling CancelSynchronousIo");
        CancelSynchronousIo(threads[SVCBATCH_WRPIPE_THREAD].thread);
    }
    if (!GetExitCodeProcess(cmdproc->pInfo.hProcess, &rc))
        rc = GetLastError();
    if (rc) {
        if (rc != 255) {
            /**
             * 255 is exit code when CTRL_C is send to cmd.exe
             */
            setsvcstatusexit(rc);
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
        case SVCBATCH_CTRL_ROTATE:
            msg = "Service signaled : SVCBATCH_CTRL_ROTATE";
        break;
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
                return ERROR_CALL_NOT_IMPLEMENTED;
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

static void threadscleanup(void)
{
    int i;

    DBG_PRINTS("started");
    for(i = 0; i < SVCBATCH_MAX_THREADS; i++) {
        if (threads[i].started) {
#if defined(_DEBUG)
            DBG_PRINTF("threads[%d]    x", i);
            threads[i].duration = GetTickCount64() - threads[i].duration;
#endif
            threads[i].exitCode = ERROR_DISCARDED;
            TerminateThread(threads[i].thread, threads[i].exitCode);
        }
        if (threads[i].thread) {
#if defined(_DEBUG)
            DBG_PRINTF("threads[%d] %4lu %10llums", i,
                        threads[i].exitCode,
                        threads[i].duration);
#endif
            CloseHandle(threads[i].thread);
            threads[i].thread = NULL;
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
            DBG_PRINTF("threads[%d]", i);
#endif
            wh[nw++] = threads[i].thread;
        }
    }
    if (nw)
        WaitForMultipleObjects(nw, wh, TRUE, ms);
    DBG_PRINTF("done %d", nw);
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
    SAFE_CLOSE_HANDLE(dologrotate);
    if (sharedmem)
        UnmapViewOfFile(sharedmem);
    SAFE_CLOSE_HANDLE(sharedmmap);
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
    if (IS_SET(SVCBATCH_OPT_ROTATE)) {
        dologrotate = CreateEventEx(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(dologrotate))
            return GetLastError();
    }
    return 0;
}

static void WINAPI servicemain(DWORD argc, LPWSTR *argv)
{
    DWORD  rv = 0;
    DWORD  i;
    HANDLE sh = NULL;

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

    rv = createevents();
    if (rv) {
        xsyserror(rv, L"CreateEvent", NULL);
        reportsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    if (IS_SET(SVCBATCH_OPT_VERBOSE)) {
        statuslog = (LPSVCBATCH_LOG)xmcalloc(1, sizeof(SVCBATCH_LOG));

        statuslog->logName = svclogfname ? svclogfname : SVCBATCH_LOGNAME;
        statuslog->maxLogs = SVCBATCH_DEF_LOGS;
        statuslog->fileExt = SBSTATUS_LOGFEXT;
        SVCBATCH_CS_INIT(statuslog);
    }
    if (outputlog) {
        if (outdirparam == NULL)
            outdirparam = SVCBATCH_LOGSDIR;
        rv = createlogsdir(outputlog);
        if (rv) {
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
    }
    else {
        if (statuslog) {
            if (outdirparam == NULL)
                outdirparam = SVCBATCH_LOGSDIR;
            rv = createlogsdir(statuslog);
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
    }
    /**
     * Add additional arguments for batch file
     * from the application that started the service.
     *
     * eg, sc.exe start myservice param1 param2 ...
     */
    for (i = 1; i < argc; i++) {
        DBG_PRINTF("argv[%lu] %S", cmdproc->argc, argv[i]);
        if (cmdproc->argc < SVCBATCH_MAX_ARGS)
            cmdproc->args[cmdproc->argc++] = xwcsdup(argv[i]);
        else
            xsyswarn(0, L"The argument has exceeded the argument number limit", argv[i]);
    }

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
        logwrstat(statuslog, 2, 0, "Service stopped without SERVICE_CONTROL_STOP");
    }
    SVCBATCH_CS_LEAVE(service);
    DBG_PRINTS("waiting for stop to finish");
    WaitForSingleObject(svcstopdone, stoptimeout);
    waitforthreads(SVCBATCH_STOP_STEP);

finished:
    DBG_PRINTS("closing");
    closeprocess(cmdproc);
    closelogfile(outputlog);
    closelogfile(statuslog);
    threadscleanup();
    reportsvcstatus(SERVICE_STOPPED, rv);
    DBG_PRINTS("done");
}

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

static int xwmaininit(void)
{
    WCHAR bb[SVCBATCH_PATH_MAX];
    DWORD nn;
    LARGE_INTEGER i;

    /**
     * On systems that run Windows XP or later,
     * this functions will always succeed.
     */
    QueryPerformanceFrequency(&i);
    counterfreq = i.QuadPart;
    QueryPerformanceCounter(&i);
    counterbase = i.QuadPart;

    xmemzero(threads, SVCBATCH_MAX_THREADS,   sizeof(SVCBATCH_THREAD));
    service = (LPSVCBATCH_SERVICE)xmcalloc(1, sizeof(SVCBATCH_SERVICE));
    program = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));

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
    while (--nn > 2) {
        if (bb[nn] == L'\\') {
            bb[nn] = WNUL;
            program->directory = xwcsdup(bb);
            break;
        }
    }
    ASSERT_WSTR(program->application, ERROR_BAD_PATHNAME);
    ASSERT_WSTR(program->directory,   ERROR_BAD_PATHNAME);

    return 0;
}

int wmain(int argc, LPCWSTR *wargv)
{
    int    i;
    int    opt;
    int    rcnt = 0;
    int    scnt = 0;
    int    qcnt = 0;
    int    bcnt = 0;
    int    rv;
    HANDLE h;
    WCHAR  bb[TBUFSIZ] = { L'-', WNUL, WNUL, WNUL };

    LPCWSTR maxlogsparam = NULL;
    LPCWSTR batchparam   = NULL;
    LPCWSTR svchomeparam = NULL;
    LPCWSTR svcworkparam = NULL;
    LPCWSTR sparam[SVCBATCH_MAX_ARGS];
    LPCWSTR rparam[3];


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
        program->sInfo.hStdInput  = h;
        program->sInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        program->sInfo.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }
    /**
     * Check if running as service or as a child process.
     */
    if (argc > 1) {
        LPCWSTR p = wargv[1];
        if (p[0] == L'@') {
            if (p[1] == L'@') {
                DWORD x;
                servicemode = FALSE;
                sharedmmap  = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, p + 2);
                if (sharedmmap == NULL)
                    return GetLastError();
                sharedmem = (LPSVCBATCH_IPC)MapViewOfFile(
                                                sharedmmap,
                                                FILE_MAP_ALL_ACCESS,
                                                0, 0, DSIZEOF(SVCBATCH_IPC));
                if (sharedmem == NULL)
                    return GetLastError();
                cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT;
                wnamestamp  = CPP_WIDEN(SHUTDOWN_APPNAME) L" " SVCBATCH_VERSION_WCS;
                cwsappname  = CPP_WIDEN(SHUTDOWN_APPNAME);
                stoptimeout = sharedmem->timeout;
                svcoptions  = sharedmem->options;
                killdepth   = sharedmem->killdepth;
#if defined(_DEBUG)
                dbgsvcmode = '1';
                dbgprints(__FUNCTION__, cnamestamp);
                dbgprintf(__FUNCTION__, "ppid %lu", sharedmem->processId);
                dbgprintf(__FUNCTION__, "opts 0x%08x", sharedmem->options);
                dbgprintf(__FUNCTION__, "time %lu", stoptimeout);
#endif
                cmdproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
                cmdproc->application = sharedmem->application;
                cmdproc->argc    = sharedmem->argc;
                cmdproc->args[0] = sharedmem->batchFile;
                for (x = 1; x < cmdproc->argc; x++)
                    cmdproc->args[x] = sharedmem->args[x];
                service->home = sharedmem->home;
                service->work = sharedmem->work;
                service->name = sharedmem->name;
                service->uuid = sharedmem->uuid;
                if (IS_NOT(SVCBATCH_OPT_QUIET)) {
                    outputlog = (LPSVCBATCH_LOG)xmcalloc(1, sizeof(SVCBATCH_LOG));

                    outputlog->logName = sharedmem->logName;
                    outputlog->maxLogs = SVCBATCH_DEF_LOGS;
                    outputlog->fileExt = SHUTDOWN_LOGFEXT;
                    SVCBATCH_CS_INIT(outputlog);
                }
                xwcslcpy(service->logs, SVCBATCH_PATH_MAX, sharedmem->logs);
            }
            else {
                service->name = p + 1;
                if (IS_EMPTY_WCS(service->name))
                    return ERROR_INVALID_PARAMETER;
            }
            wargv[1] = wargv[0];
            argc    -= 1;
            wargv   += 1;
        }
    }

#if defined(_DEBUG)
    if (servicemode) {
        dbgsvcmode = '0';
        dbgprints(__FUNCTION__, cnamestamp);
    }
#endif
    if (servicemode) {
        DWORD  nn;
        WCHAR  wb[SVCBATCH_PATH_MAX];
        LPWSTR wp;

        nn = GetEnvironmentVariableW(L"COMSPEC", wb, SVCBATCH_PATH_MAX);
        if (nn == 0)
            return GetLastError();
        if (nn >= SVCBATCH_PATH_MAX)
            return ERROR_INSUFFICIENT_BUFFER;
        wp = xgetfinalpath(wb, 0, NULL, 0);
        if (wp == NULL)
            return ERROR_BAD_ENVIRONMENT;

        cmdproc = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
        cmdproc->argc        = 1;
        cmdproc->application = wp;


        while ((opt = xwgetopt(argc, wargv, L"bh:k:lm:n:o:pqr:s:tvw:")) != EOF) {
            switch (opt) {
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
                case L'm':
                    maxlogsparam = xwoptarg;
                break;
                case L'n':
                    if (wcspbrk(xwoptarg, L"/\\:;<>?*|\""))
                        return xsyserror(0, L"Found invalid filename characters", xwoptarg);
                    svclogfname  = xwcsdup(xwoptarg);
                    if (wcschr(svclogfname, L'@')) {
                        /**
                         * If name is strftime formatted
                         * replace @ with % so it can be used by strftime
                         */
                        xwchreplace(svclogfname, L'@', L'%');
                    }
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
                case L'b':
                    bcnt++;
                break;
                case L'q':
                    svcoptions |= SVCBATCH_OPT_QUIET;
                    qcnt++;
                break;
                case L's':
                    if (scnt < SVCBATCH_MAX_ARGS)
                        sparam[scnt++] = xwoptarg;
                    else
                        return xsyserror(0, L"Too many -s arguments", xwoptarg);
                break;
                case L'r':
                    if (rcnt < 3)
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

        argc  -= xwoptind;
        wargv += xwoptind;
        if (argc == 0) {
            /**
             * No batch file defined.
             * If @ServiceName was defined, try with ServiceName.bat
             */
            if (IS_EMPTY_WCS(service->name) || wcspbrk(service->name, L"/\\:;<>?*|\""))
                return xsyserror(0, L"Missing batch file", NULL);

            i = xwcsncat(wb, BBUFSIZ, 0, service->name);
            i = xwcsncat(wb, BBUFSIZ, i, L".bat");
            batchparam = wb;
        }
        else {
            batchparam = wargv[0];
            for (i = 1; i < argc; i++) {
                /**
                 * Add arguments for batch file
                 */
                if (xwcslen(wargv[i]) >= SVCBATCH_NAME_MAX)
                    return xsyserror(0, L"The argument is too large", wargv[i]);

                if (i < SVCBATCH_MAX_ARGS)
                    cmdproc->args[i] = xwcsdup(wargv[i]);
                else
                    return xsyserror(0, L"Too many batch arguments", wargv[i]);
            }
            cmdproc->argc = i;
        }
        service->uuid = xuuidstring(NULL);
        if (IS_EMPTY_WCS(service->uuid))
            return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_UUID", NULL);
        if (bcnt) {
            if (bcnt > 1)
                svcoptions |= SVCBATCH_OPT_BREAK;
             else
                svcoptions |= SVCBATCH_OPT_CTRL_BREAK;
        }
        if (scnt && qcnt < 2) {
            /**
             * Use -qq to disable both service and shutdown logging
             */
            qcnt = 0;
        }
        if (qcnt == 0) {
            outputlog = (LPSVCBATCH_LOG)xmcalloc(1, sizeof(SVCBATCH_LOG));

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
            /**
             * Ensure that log related command options
             * are not defined when -q is defined
             */
            bb[0] = WNUL;
            if (maxlogsparam)
                xwcslcat(bb, TBUFSIZ, L"-m ");
            if (rcnt)
                xwcslcat(bb, TBUFSIZ, L"-r ");
            if (IS_SET(SVCBATCH_OPT_TRUNCATE))
                xwcslcat(bb, TBUFSIZ, L"-t ");
            if (bb[0]) {
#if defined(_DEBUG) && (_DEBUG > 1)
                xsyswarn(0, L"Option -q is mutually exclusive with option(s)", bb);
                rcnt       = 0;
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
                service->home = xgetfinalpath(svchomeparam, 1, NULL, 0);
                if (IS_EMPTY_WCS(service->home))
                    return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
            }
        }
        if (service->home == NULL) {
            if (isabsolutepath(batchparam)) {
                if (!resolvebatchname(batchparam))
                    return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);

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
            SetCurrentDirectoryW(service->work);
        }
        if (!resolvebatchname(batchparam))
            return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);
        if (rcnt) {
            for (i = 0; i < rcnt; i++) {
                if (!resolverotate(rparam[i]))
                    return xsyserror(0, L"Invalid rotate parameter", rparam[i]);
            }
            if (rotatebysize || rotatebytime)
                outputlog->maxLogs = 0;

            svcoptions |= SVCBATCH_OPT_ROTATE;
        }
        if (scnt) {
            svcstop = (LPSVCBATCH_PROCESS)xmcalloc(1, sizeof(SVCBATCH_PROCESS));
            svcstop->args[0] = xgetfinalpath(sparam[0], 0, NULL, 0);
            if (svcstop->args[0] == NULL)
                return xsyserror(ERROR_FILE_NOT_FOUND, sparam[0], NULL);
            for (i = 1; i < scnt; i++)
                svcstop->args[i] = xwcsdup(sparam[i]);
            svcstop->argc  = scnt;
        }
        if (service->home != service->work)
            SetCurrentDirectoryW(service->home);
    }

    /**
     * Create logic state events
     */
    workerended = CreateEventEx(NULL, NULL,
                                CREATE_EVENT_MANUAL_RESET,
                                EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(workerended))
        return xsyserror(GetLastError(), L"CreateEvent", NULL);
    SVCBATCH_CS_INIT(service);
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
