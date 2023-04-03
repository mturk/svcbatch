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
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <process.h>
#include <io.h>
#include <errno.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif
#include "svcbatch.h"

#if defined(_DEBUG)
static void dbgprintf(const char *, const char *, ...);
static void dbgprints(const char *, const char *);
# define DBG_PRINTF(Fmt, ...)   dbgprintf(__FUNCTION__, Fmt, ##__VA_ARGS__)
# define DBG_PRINTS(Msg)        dbgprints(__FUNCTION__, Msg)
# if (_DEBUG > 1)
static FILE *dbgfile = NULL;
# endif
#else
# define DBG_PRINTF(Fmt, ...)   (void)0
# define DBG_PRINTS(Msg)        (void)0
#endif

#define xsyserror(_n, _e, _d)   svcsyserror(__FUNCTION__, __LINE__, (_n), (_e), (_d))

typedef struct _SVCBATCH_THREAD {
    LPTHREAD_START_ROUTINE threadfn;
    LPVOID                 param;
    HANDLE                 h;
    DWORD                  id;
} SVCBATCH_THREAD, *LPSVCBATCH_THREAD;

typedef struct _SVCBATCH_PIPE {
    OVERLAPPED  o;
    HANDLE      pipe;
    DWORD       stat;
    BYTE        buff[HBUFSIZ];
} SVCBATCH_PIPE, *LPSVCBATCH_PIPE;

static volatile LONG         monitorsig  = 0;
static volatile LONG         rotateruns  = 0;
static volatile LONG         sstarted    = 0;
static volatile LONG         sscstate    = SERVICE_START_PENDING;
static volatile LONG         rotatecount = 0;
static volatile LONG64       logwritten  = 0;
static volatile HANDLE       logfhandle  = NULL;
static SERVICE_STATUS_HANDLE hsvcstatus  = NULL;
static SERVICE_STATUS        ssvcstatus;
static CRITICAL_SECTION      servicelock;
static CRITICAL_SECTION      logfilelock;
static SECURITY_ATTRIBUTES   sazero;
static LONGLONG              rotateint   = CPP_INT64_C(0);
static LARGE_INTEGER         rotatetmo   = {{ 0, 0 }};
static LARGE_INTEGER         rotatesiz   = {{ 0, 0 }};
static LARGE_INTEGER         pcfrequency = {{ 0, 0 }};
static LARGE_INTEGER         pcstarttime = {{ 0, 0 }};

static wchar_t  *comspec          = NULL;

static BOOL      haslogstatus     = FALSE;
static BOOL      hasctrlbreak     = FALSE;
static BOOL      haslogrotate     = FALSE;
static BOOL      haspipedlogs     = FALSE;
static BOOL      rotatebysize     = FALSE;
static BOOL      rotatebytime     = FALSE;
static BOOL      uselocaltime     = FALSE;
static BOOL      truncatelogs     = FALSE;
static BOOL      truncateslog     = FALSE;
static BOOL      havelogging      = TRUE;
static BOOL      servicemode      = TRUE;

static DWORD     preshutdown      = 0;
static DWORD     childprocpid     = 0;
static DWORD     pipedprocpid     = 0;

static int       svcmaxlogs       = 0;
static int       xwoptind         = 1;
static wchar_t   xwoption         = WNUL;
static int       logredirargc     = 0;
static int       svcstopwargc     = 0;
static int       svcbatchargc     = 0;
static int       svccodepage      = 0;
static _locale_t localeobject     = NULL;
static wchar_t  *svcbatchexe      = NULL;
static wchar_t  *svcbatchfile     = NULL;
static wchar_t  *svcbatchname     = NULL;
static wchar_t  *exelocation      = NULL;
static wchar_t  *servicebase      = NULL;
static wchar_t  *servicename      = NULL;
static wchar_t  *servicehome      = NULL;
static wchar_t  *serviceuuid      = NULL;

static wchar_t  *servicelogs      = NULL;
static wchar_t  *logfilename      = NULL;
static wchar_t  *svclogfname      = NULL;
static wchar_t  *svcendlogfn      = NULL;

static HANDLE    childprocess     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    ssignalevent     = NULL;
static HANDLE    processended     = NULL;
static HANDLE    monitorevent     = NULL;
static HANDLE    logrotatesig     = NULL;
static HANDLE    pipedprocess     = NULL;

static wchar_t  *logredirargv[SVCBATCH_MAX_ARGS];
static wchar_t  *svcstopwargv[SVCBATCH_MAX_ARGS];
static wchar_t  *svcbatchargv[SVCBATCH_MAX_ARGS];

static SVCBATCH_THREAD svcthread[SVCBATCH_MAX_THREADS];

static wchar_t      wnamestamp[RBUFSIZ];
static wchar_t      zerostring[2] = { WNUL, WNUL };
static const wchar_t *CRLFW       = L"\r\n";
static const char    *CRLFA       =  "\r\n";
static const char    *YYES        =  "Y";

static const char    *cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;
static const wchar_t *cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);
static const wchar_t *outdirparam = NULL;
static const wchar_t *localeparam = NULL;
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

static __inline void xmemzero(void *mem, size_t number, size_t size)
{
    memset(mem, 0, number * size);
}

static __inline wchar_t *xwcsdup(const wchar_t *s)
{
    if (IS_EMPTY_WCS(s))
        return NULL;
    return _wcsdup(s);
}

static __inline int xwcslen(const wchar_t *s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    else
        return (int)wcslen(s);
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

        /* Check if accumulated value is smaller then
         * INT_MAX/10, otherwise overflow would occur.
         */
        if (rv > 214748363) {
            SetLastError(ERROR_INVALID_DATA);
            return -1;
        }
        if (dv || rv) {
            rv *= 10;
            rv += dv;
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

static void xcleanwinpath(wchar_t *s, int isdir)
{
    int i;

    if (IS_EMPTY_WCS(s))
        return;

    for (i = 0; s[i]; i++) {
        if (s[i] == L'/')
            s[i] =  L'\\';
    }
    --i;
    while (i > 1) {
        if (iswspace(s[i]))
            s[i--] = WNUL;
        else
            break;

    }
    if (isdir) {
        while (i > 1) {
            if ((s[i] ==  L'\\') && (s[i - 1] != L'.'))
                s[i--] = WNUL;
            else
                break;
        }
    }
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
    d[0] = HIBYTE(svcthread[0].id);
    d[1] = LOBYTE(svcthread[0].id);
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

    if (pcfrequency.QuadPart) {
        QueryPerformanceCounter(&ct);
        et.QuadPart = ct.QuadPart - pcstarttime.QuadPart;

        /**
         * Convert to microseconds
         */
        et.QuadPart *= CPP_INT64_C(1000000);
        et.QuadPart /= pcfrequency.QuadPart;
        ct.QuadPart  = et.QuadPart / CPP_INT64_C(1000);
    }
    us = (DWORD)((et.QuadPart % CPP_INT64_C(1000000)));
    ss = (DWORD)((ct.QuadPart / MS_IN_SECOND) % 60);
    mm = (DWORD)((ct.QuadPart / MS_IN_MINUTE) % 60);
    hh = (DWORD)((ct.QuadPart / MS_IN_HOUR));

    return xsnprintf(wb, sz, "[%.2lu:%.2lu:%.2lu.%.6lu] ",
                     hh, mm, ss, us);
}

#if defined(_DEBUG)
/**
 * Runtime debugging functions
 */
static void dbgprints(const char *funcname, const char *string)
{
    char b[MBUFSIZ];

#if (_DEBUG > 1)
    if (dbgfile) {
        xsnprintf(b, MBUFSIZ, "[%.4lu] [%.4lu] %d %-16s %s",
                  GetCurrentProcessId(),
                  GetCurrentThreadId(),
                  servicemode, funcname, string);
    }
    else
#endif
    {
        xsnprintf(b, MBUFSIZ, "[%.4lu] %d %-16s %s",
                  GetCurrentThreadId(),
                  servicemode, funcname, string);
    }
#if (_DEBUG > 1)
    if (dbgfile) {
        SYSTEMTIME st;

        GetSystemTime(&st);
        fprintf(dbgfile, "[%.2d:%.2d:%.2d] %s\n",
                st.wHour, st.wMinute, st.wSecond, b);
        fflush(dbgfile);
    }
    else
#endif
    {
        OutputDebugStringA(b);
    }
}

static void dbgprintf(const char *funcname, const char *format, ...)
{
    int     n;
    char    b[MBUFSIZ];
    va_list ap;

#if (_DEBUG > 1)
    if (dbgfile) {
        n = xsnprintf(b, MBUFSIZ, "[%.4lu] [%.4lu] %d %-16s ",
                      GetCurrentProcessId(),
                      GetCurrentThreadId(),
                      servicemode, funcname);
    }
    else
#endif
    {
        n = xsnprintf(b, MBUFSIZ, "[%.4lu] %d %-16s ",
                      GetCurrentThreadId(),
                      servicemode, funcname);
    }

    va_start(ap, format);
    xvsnprintf(b + n, MBUFSIZ - n, format, ap);
    va_end(ap);
#if (_DEBUG > 1)
    if (dbgfile) {
        SYSTEMTIME st;

        GetSystemTime(&st);
        fprintf(dbgfile, "[%.2d:%.2d:%.2d] %s\n",
                st.wHour, st.wMinute, st.wSecond, b);
        fflush(dbgfile);
    }
    else
#endif
    {
        OutputDebugStringA(b);
    }
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

static DWORD svcsyserror(const char *fn, int line, DWORD ern, const wchar_t *err, const wchar_t *eds)
{
    wchar_t        hdr[MBUFSIZ];
    wchar_t        erb[MBUFSIZ];
    const wchar_t *errarg[10];
    int            c, i = 0;

    errarg[i++] = wnamestamp;
    if (servicename)
        errarg[i++] = servicename;
    errarg[i++] = CRLFW;
    errarg[i++] = L"reported the following error:\r\n";

    xsnwprintf(hdr, MBUFSIZ,
               L"svcbatch.c(%.4d, %S) %s", line, fn, err);
    if (eds) {
        xwcslcat(hdr, MBUFSIZ, L": ");
        xwcslcat(hdr, MBUFSIZ, eds);
    }
    errarg[i++] = hdr;

#if defined(_DEBUG)
# if (_DEBUG > 1)
    if (dbgfile) {
        fputc('\n', dbgfile);
    }
    else
# endif
    {
        OutputDebugStringA("\n");
    }
#endif
    if (ern) {
        c = xsnwprintf(erb, TBUFSIZ, L"error(%lu) ", ern);
        xwinapierror(erb + c, MBUFSIZ - c, ern);
        errarg[i++] = CRLFW;
        errarg[i++] = erb;
        DBG_PRINTF("%S, %S\n", hdr, erb);
    }
    else {
        ern = ERROR_INVALID_PARAMETER;
        DBG_PRINTF("%S\n", hdr);
    }
#if defined(_DEBUG) && (_DEBUG > 1)
    return ern;
#else
    errarg[i++] = CRLFW;
    while (i < 10) {
        errarg[i++] = L"";
    }
    if (setupeventlog()) {
        HANDLE es = RegisterEventSourceW(NULL, CPP_WIDEN(SVCBATCH_NAME));
        if (IS_VALID_HANDLE(es)) {
            /**
             * Generic message: '%1 %2 %3 %4 %5 %6 %7 %8 %9'
             * The event code in netmsg.dll is 3299
             */
            ReportEventW(es, EVENTLOG_ERROR_TYPE,
                         0, 3299, NULL, 9, 0, errarg, NULL);
            DeregisterEventSource(es);
        }
    }
    return ern;
#endif
}

static DWORD killproctree(HANDLE ph, DWORD pid, int rc)
{
    DWORD  r = 0;
    HANDLE h;
    HANDLE p;
    PROCESSENTRY32W e;

    h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (IS_INVALID_HANDLE(h)) {
        r = GetLastError();
        goto finished;
    }
    e.dwSize = DSIZEOF(PROCESSENTRY32W);
    if (Process32FirstW(h, &e) == 0) {
        r = GetLastError();
        if (r == ERROR_NO_MORE_FILES)
            r = 0;
        CloseHandle(h);
        goto finished;
    }
    do {
        if (e.th32ParentProcessID == pid) {
            p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, 0, e.th32ProcessID);

            if (IS_VALID_HANDLE(p)) {
                if (rc) {
                    DBG_PRINTF("killing child [%.4lu] of proc [%.4lu]", e.th32ProcessID, pid);
                    killproctree(p, e.th32ProcessID, rc - 1);
                }
                else {
                    DBG_PRINTF("terminating child [%.4lu] of proc [%.4lu]", e.th32ProcessID, pid);
                    TerminateProcess(p, 1);
                }
                CloseHandle(p);
            }
        }

    } while (Process32NextW(h, &e));
    CloseHandle(h);
finished:
    if (IS_VALID_HANDLE(ph)) {
        DBG_PRINTF("terminating proc [%.4lu]", pid);
        TerminateProcess(ph, 1);
    }
    return r;
}

static DWORD xcreatedir(const wchar_t *path)
{
    if (!CreateDirectoryW(path, NULL)) {
        DWORD rc = GetLastError();
        if (rc != ERROR_ALREADY_EXISTS)
            return rc;
    }
    return 0;
}

static DWORD xmdparent(wchar_t *path)
{
    DWORD rc;
    wchar_t *s;

    s = wcsrchr(path, L'\\');
    if (s == NULL)
        return ERROR_BAD_PATHNAME;
    *s = WNUL;
    rc = xcreatedir(path);
    if (rc == ERROR_PATH_NOT_FOUND) {
        /**
         * One or more intermediate directories do not exist
         * Call xmdparent again
         */
        rc = xmdparent(path);
    }
    *s = L'\\';
    return rc;
}

static DWORD xcreatepath(const wchar_t *path)
{
    DWORD rc;

    rc = xcreatedir(path);
    if (rc == ERROR_PATH_NOT_FOUND) {
        wchar_t *pp = xwcsdup(path);

        rc = xmdparent(pp);
        xfree(pp);

        if (rc == 0)
            rc = xcreatedir(path);
    }
    return rc;
}

static DWORD WINAPI xrunthread(LPVOID param)
{
    DWORD r;
    LPSVCBATCH_THREAD tp = (LPSVCBATCH_THREAD)param;

    r = (*tp->threadfn)(tp->param);
    ExitThread(r);
    return r;
}

static HANDLE xcreatethread(int detached, int suspended,
                            LPTHREAD_START_ROUTINE threadfn,
                            LPVOID param)
{
    static volatile LONG tc = 0;
    DWORD  ix;

    ix = (DWORD)InterlockedIncrement(&tc);
    if (ix >= SVCBATCH_MAX_THREADS) {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    svcthread[ix].threadfn = threadfn;
    svcthread[ix].param    = param;

    svcthread[ix].h = CreateThread(NULL, 0, xrunthread,
                                   &svcthread[ix], CREATE_SUSPENDED,
                                   &svcthread[ix].id);
    if (svcthread[ix].h == NULL)
        return NULL;
    if (detached) {
        ResumeThread(svcthread[ix].h);
        CloseHandle( svcthread[ix].h);
        return NULL;
    }
    else {
        if (!suspended)
            ResumeThread(svcthread[ix].h);
        return svcthread[ix].h;
    }
}

static BOOL isrelativepath(const wchar_t *p)
{
    if ((p != NULL) && (p[0] < 128)) {
        if ((p[0] == L'\\') || (isalpha(p[0]) && (p[1] == L':')))
            return FALSE;
    }
    return TRUE;
}

static wchar_t *getfullpathname(wchar_t *buf, int siz, const wchar_t *src, int isdir)
{
    ASSERT_WSTR(src, NULL);

    xwcslcpy(buf, siz, src);
    xcleanwinpath(buf, isdir);

    if (isrelativepath(buf)) {
        DWORD   nn;
        wchar_t bb[FBUFSIZ];

        nn = GetFullPathNameW(buf, FBUFSIZ, bb, NULL);
        if ((nn == 0) || (nn >= FBUFSIZ))
            return NULL;
        xwcslcpy(buf, siz, bb);
    }
    return buf;
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

static wchar_t *getfinalpathname(const wchar_t *path, int isdir)
{
    wchar_t buf[FBUFSIZ];
    DWORD   len;
    HANDLE  fh;
    DWORD   atr  = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, atr, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, buf, HBUFSIZ, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if (len >= FBUFSIZ) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return NULL;
    }
    CloseHandle(fh);
    fixshortpath(buf, len);
    return xwcsdup(buf);
}

static wchar_t *getrealpathname(const wchar_t *src, int isdir)
{
    wchar_t *fpn;
    wchar_t  buf[FBUFSIZ];

    if (!servicemode)
        return xwcsdup(src);

    fpn = getfullpathname(buf, FBUFSIZ, src, isdir);
    ASSERT_WSTR(fpn, NULL);

    return getfinalpathname(fpn, isdir);
}

static BOOL resolvebatchname(const wchar_t *a)
{
    wchar_t *p;

    if (svcbatchfile)
        return TRUE;
    svcbatchfile = getrealpathname(a, 0);
    if (IS_EMPTY_WCS(svcbatchfile))
        return FALSE;

    p = wcsrchr(svcbatchfile, L'\\');
    if (p) {
        *p = WNUL;
        svcbatchname = p + 1;
        servicebase  = xwcsdup(svcbatchfile);
        *p = L'\\';
        return TRUE;
    }
    else {
        xfree(svcbatchfile);
        svcbatchfile = NULL;
        servicebase  = NULL;
        svcbatchname = NULL;
        return FALSE;
    }
}

static void setsvcstatusexit(DWORD e)
{
    EnterCriticalSection(&servicelock);
    ssvcstatus.dwServiceSpecificExitCode = e;
    LeaveCriticalSection(&servicelock);
}

static void reportsvcstatus(DWORD status, DWORD param)
{
    static DWORD cpcnt = 1;

    if (!servicemode)
        return;
    EnterCriticalSection(&servicelock);
    if (InterlockedExchange(&sscstate, SERVICE_STOPPED) == SERVICE_STOPPED)
        goto finished;
    ssvcstatus.dwControlsAccepted = 0;
    ssvcstatus.dwCheckPoint       = 0;
    ssvcstatus.dwWaitHint         = 0;

    if (status == SERVICE_RUNNING) {
        ssvcstatus.dwControlsAccepted =  SERVICE_ACCEPT_STOP |
                                         SERVICE_ACCEPT_SHUTDOWN |
                                         preshutdown;
        cpcnt = 1;
    }
    else if (status == SERVICE_STOPPED) {
        if (param != 0)
            ssvcstatus.dwServiceSpecificExitCode = param;
        if (ssvcstatus.dwServiceSpecificExitCode == 0 &&
            ssvcstatus.dwCurrentState != SERVICE_STOP_PENDING) {
            ssvcstatus.dwServiceSpecificExitCode = ERROR_PROCESS_ABORTED;
            xsyserror(0, L"service stopped without SERVICE_CONTROL_STOP signal", NULL);
        }
        if (ssvcstatus.dwServiceSpecificExitCode != 0)
            ssvcstatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }
    else {
        ssvcstatus.dwCheckPoint = cpcnt++;
        ssvcstatus.dwWaitHint   = param;
    }
    ssvcstatus.dwCurrentState = status;
    InterlockedExchange(&sscstate, status);
    if (!SetServiceStatus(hsvcstatus, &ssvcstatus)) {
        xsyserror(GetLastError(), L"SetServiceStatus", NULL);
        InterlockedExchange(&sscstate, SERVICE_STOPPED);
    }
finished:
    LeaveCriticalSection(&servicelock);
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

    DBG_PRINTF("0x%08x %S", mode, name);
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
    if (!haspipedlogs) {
        LARGE_INTEGER ee = {{ 0, 0 }};
        return SetFilePointerEx(h, ee, NULL, FILE_END);
    }
    return TRUE;
}

static DWORD logappend(HANDLE h, LPCVOID buf, DWORD len)
{
    DWORD wr;

    if (xseekfend(h)) {
        if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0)) {
            InterlockedAdd64(&logwritten, wr);
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
            InterlockedAdd64(&logwritten, wr);
            if (!haspipedlogs)
                FlushFileBuffers(h);
            return xseekfend(h);
        }
    }
    return FALSE;
}

static void logwrline(HANDLE h, const char *s)
{
    char    wb[TBUFSIZ];
    DWORD   wr;
    DWORD   nw;

    nw = xtimehdr(wb, TBUFSIZ);

    if (nw > 0) {
        if (WriteFile(h, wb, nw, &wr, NULL) && (wr != 0))
            InterlockedAdd64(&logwritten, wr);
        nw = (DWORD)strlen(s);
        if (WriteFile(h, s, nw, &wr, NULL) && (wr != 0))
            InterlockedAdd64(&logwritten, wr);
        if (WriteFile(h, CRLFA, 2, &wr, NULL) && (wr != 0))
            InterlockedAdd64(&logwritten, wr);
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

    n = (int)strlen(hdr);
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
    logwransi(h, "Service name     : ", servicename);
    logwransi(h, "Service uuid     : ", serviceuuid);
    logwransi(h, "Batch file       : ", svcbatchfile);
    if (svcstopwargc)
        logwransi(h, "Shutdown batch   : ", svcstopwargv[0]);
    logwransi(h, "Program directory: ", exelocation);
    logwransi(h, "Base directory   : ", servicebase);
    logwransi(h, "Home directory   : ", servicehome);
    logwransi(h, "Logs directory   : ", servicelogs);
    if (haspipedlogs)
        logwransi(h, "Log redirected to: ", logredirargv[0]);

    logfflush(h);
}

static DWORD createlogsdir(void)
{
    wchar_t dp[FBUFSIZ];

    if (getfullpathname(dp, FBUFSIZ, outdirparam, 1) == NULL) {
        xsyserror(0, L"getfullpathname", outdirparam);
        return ERROR_BAD_PATHNAME;
    }
    servicelogs = getfinalpathname(dp, 1);
    if (servicelogs == NULL) {
        DWORD rc = GetLastError();

        if (rc != ERROR_PATH_NOT_FOUND)
            return xsyserror(rc, L"getfinalpathname", dp);

        rc = xcreatepath(dp);
        if (rc != 0)
            return xsyserror(rc, L"xcreatepath", dp);
        servicelogs = getfinalpathname(dp, 1);
        if (servicelogs == NULL)
            return xsyserror(GetLastError(), L"getfinalpathname", dp);
    }
    if (_wcsicmp(servicelogs, servicehome) == 0) {
        xsyserror(0, L"Logs directory cannot be the same as home directory",
                  servicelogs);
        return ERROR_INVALID_PARAMETER;
    }
    return 0;
}

#if defined(_DEBUG)
static DWORD WINAPI rdpipedlog(void *ph)
{
    DWORD rc = 0;
    DWORD rs = 1;
    DWORD rn = 0;
    DWORD rm = SBUFSIZ - 32;
    char  rb[2];
    char  rl[SBUFSIZ];

    DBG_PRINTS("started");
    while (rc == 0) {
        DWORD rd = 0;

        if (ReadFile(ph, rb, rs, &rd, NULL) && (rd != 0)) {
            if (rb[0] == '\r') {
                /* Skip */
            }
            else if (rb[0] == '\n') {
                rl[rn] = '\0';
                DBG_PRINTF("[%.4lu] %s", pipedprocpid, rl);
                rn = 0;
            }
            else {
                rl[rn++] = rb[0];
                if (rn > rm) {
                    rl[rn] = '\0';
                    DBG_PRINTF("[%.4lu] %s", pipedprocpid, rl);
                    rn = 0;
                }
            }
        }
        else {
            rc = GetLastError();
        }
    }
    CloseHandle(ph);
    if (rn) {
        rl[rn] = '\0';
        DBG_PRINTF("[%.4lu] %s", pipedprocpid, rl);
    }
    if (rc) {
        if ((rc == ERROR_BROKEN_PIPE) || (rc == ERROR_NO_DATA))
            DBG_PRINTS("pipe closed");
        else
            DBG_PRINTF("err=%lu", rc);
    }
    DBG_PRINTS("done");
    return 0;
}
#endif

static DWORD openlogpipe(BOOL ssp)
{
    DWORD    rc;
    HANDLE   wr = NULL;
    LPHANDLE rp = NULL;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;
    wchar_t *cmdline = NULL;
#if defined(_DEBUG)
    HANDLE rd = NULL;

    rp = &rd;
#endif

    DBG_PRINTS("started");

    xmemzero(&cp, 1, sizeof(PROCESS_INFORMATION));
    rc = createiopipes(&si, &wr, rp, 0);
    if (rc) {
       DBG_PRINTF("createiopipes failed with %lu", rc);
       return rc;
    }
    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    cmdline = xappendarg(1, NULL, NULL, logredirargv[0]);
    if (logredirargc == 1) {
        cmdline = xappendarg(1, cmdline, NULL, svclogfname);
    }
    else {
        int i;
        for (i = 1; i < logredirargc; i++) {
            xwchreplace(logredirargv[i], L'@', L'%');
            cmdline = xappendarg(1, cmdline, NULL, logredirargv[i]);
        }
    }
    DBG_PRINTF("cmdline %S", cmdline);
    if (!CreateProcessW(logredirargv[0], cmdline, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
                        NULL,
                        servicelogs,
                       &si, &cp)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", logredirargv[0]);
        goto failed;
    }
    pipedprocess = cp.hProcess;
    pipedprocpid = cp.dwProcessId;
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);

    ResumeThread(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hThread);
#if defined(_DEBUG)
    xcreatethread(1, 0, rdpipedlog, rd);
#endif
    if (haslogstatus)
        logwrline(wr, cnamestamp);

    InterlockedExchangePointer(&logfhandle, wr);
    DBG_PRINTF("running pipe log process %lu", pipedprocpid);
    xfree(cmdline);

    return 0;
failed:
    SAFE_CLOSE_HANDLE(wr);
#if defined(_DEBUG)
    SAFE_CLOSE_HANDLE(rd);
#endif
    SAFE_CLOSE_HANDLE(pipedprocess);

    return rc;
}

static DWORD makelogfile(BOOL ssp)
{
    wchar_t ewb[BBUFSIZ];
    struct  tm *ctm;
    time_t  ctt;
    DWORD   rc;
    DWORD   cm;
    HANDLE  h;
    int  i, x;

    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    time(&ctt);
    if (uselocaltime)
        ctm = localtime(&ctt);
    else
        ctm = gmtime(&ctt);
    if (_wcsftime_l(ewb, BBUFSIZ, svclogfname, ctm, localeobject) == 0)
        return xsyserror(0, L"invalid format code", svclogfname);
    xfree(logfilename);

    for (i = 0, x = 0; ewb[i]; i++) {
        wchar_t c = ewb[i];

        if ((c > 127) || (xfnchartype[c] & 1))
            ewb[x++] = c;
    }
    ewb[x] = WNUL;
    logfilename = xwcsmkpath(servicelogs, ewb);
    if (servicemode || truncatelogs)
        cm = CREATE_ALWAYS;
    else
        cm = OPEN_ALWAYS;
    h = CreateFileW(logfilename, GENERIC_WRITE,
                    FILE_SHARE_READ, &sazero, cm,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(h)) {
        xsyserror(rc, L"CreateFile", logfilename);
        return rc;
    }
    if (rc == ERROR_ALREADY_EXISTS) {
        if (cm == CREATE_ALWAYS) {
            DBG_PRINTF("truncated %S", logfilename);
        }
        else {
            logfflush(h);
            DBG_PRINTF("reusing %S", logfilename);
        }
    }
    InterlockedExchange64(&logwritten, 0);
    if (haslogstatus) {
        logwrline(h, cnamestamp);
        if (rc == ERROR_ALREADY_EXISTS) {
            if (cm == CREATE_ALWAYS)
                logwrtime(h, "Log truncated");
            else if (ssp)
                logwrtime(h, "Log reused");
        }
        else if (ssp) {
            logwrtime(h, "Log opened");
        }
    }
    InterlockedExchangePointer(&logfhandle, h);
    return 0;
}

static DWORD openlogfile(BOOL ssp)
{
    wchar_t *logpb    = NULL;
    HANDLE h          = NULL;
    int    renameprev = svcmaxlogs;
    int    rotateprev;
    DWORD  rc;
    DWORD  cm;

    if (wcschr(svclogfname, L'%'))
        return makelogfile(ssp);
    if (truncatelogs)
        renameprev = 0;
    if (rotatebysize || rotatebytime || truncatelogs)
        rotateprev = 0;
    else
        rotateprev = svcmaxlogs;
    if (logfilename == NULL)
        logfilename = xwcsmkpath(servicelogs, svclogfname);
    if (logfilename == NULL)
        return xsyserror(ERROR_FILE_NOT_FOUND, L"logfilename", NULL);

    if (renameprev) {
        if (GetFileAttributesW(logfilename) != INVALID_FILE_ATTRIBUTES) {
            if (ssp)
                reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
            if (rotateprev) {
                logpb = xwcsconcat(logfilename, L".0");
            }
            else {
                wchar_t sb[TBUFSIZ];

                xmktimedext(sb, TBUFSIZ);
                logpb = xwcsconcat(logfilename, sb);
            }
            if (!MoveFileExW(logfilename, logpb, MOVEFILE_REPLACE_EXISTING)) {
                rc = GetLastError();
                xsyserror(rc, logfilename, logpb);
                xfree(logpb);
                return rc;
            }
        }
        else {
            rc = GetLastError();
            if (rc != ERROR_FILE_NOT_FOUND) {
                return xsyserror(rc, L"GetFileAttributes", logfilename);
            }
        }
    }
    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    if (rotateprev) {
        int i;
        /**
         * Rotate previous log files
         */
        for (i = svcmaxlogs; i > 0; i--) {
            wchar_t *logpn;
            wchar_t  sfx[4] = { L'.', WNUL, WNUL, WNUL };

            sfx[1] = L'0' + i - 1;
            logpn  = xwcsconcat(logfilename, sfx);

            if (GetFileAttributesW(logpn) != INVALID_FILE_ATTRIBUTES) {
                wchar_t *lognn;
                int      logmv = 1;

                if (i > 2) {
                    /**
                     * Check for gap
                     */
                    sfx[1] = L'0' + i - 2;
                    lognn = xwcsconcat(logfilename, sfx);
                    if (GetFileAttributesW(lognn) == INVALID_FILE_ATTRIBUTES)
                        logmv = 0;
                    xfree(lognn);
                }
                if (logmv) {
                    sfx[1] = L'0' + i;
                    lognn = xwcsconcat(logfilename, sfx);
                    if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING)) {
                        rc = GetLastError();
                        xsyserror(rc, logpn, lognn);
                        xfree(logpn);
                        xfree(lognn);
                        goto failed;
                    }
                    xfree(lognn);
                    if (ssp)
                        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
                }
            }
            xfree(logpn);
        }
    }
    if (truncatelogs)
        cm = CREATE_ALWAYS;
    else
        cm = OPEN_ALWAYS;

    h = CreateFileW(logfilename, GENERIC_WRITE,
                    FILE_SHARE_READ, &sazero, cm,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(h)) {
        xsyserror(rc, L"CreateFile", logfilename);
        goto failed;
    }
    if (rc == ERROR_ALREADY_EXISTS) {
        if (cm == CREATE_ALWAYS) {
            DBG_PRINTF("truncated %S", logfilename);
        }
        else {
            logfflush(h);
            DBG_PRINTF("reusing %S", logfilename);
        }
    }
    InterlockedExchange64(&logwritten, 0);
    if (haslogstatus) {
        logwrline(h, cnamestamp);
        if (rc == ERROR_ALREADY_EXISTS) {
            if (cm == CREATE_ALWAYS)
                logwrtime(h, "Log truncated");
            else if (ssp)
                logwrtime(h, "Log reused");
        }
        else if (ssp) {
            logwrtime(h, "Log opened");
        }
    }
    InterlockedExchangePointer(&logfhandle, h);
    xfree(logpb);
    return 0;

failed:
    SAFE_CLOSE_HANDLE(h);
    if (logpb) {
        MoveFileExW(logpb, logfilename, MOVEFILE_REPLACE_EXISTING);
        xfree(logpb);
    }
    return rc;
}

static DWORD rotatelogs(void)
{
    DWORD  rc = 0;
    HANDLE h  = NULL;

    EnterCriticalSection(&logfilelock);
    InterlockedExchange(&rotateruns, 1);
    h = InterlockedExchangePointer(&logfhandle, NULL);
    if (h == NULL) {
        InterlockedExchange64(&logwritten, 0);
        rc = ERROR_FILE_NOT_FOUND;
        goto finished;
    }
    QueryPerformanceCounter(&pcstarttime);
    FlushFileBuffers(h);
    if (truncatelogs) {
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (SetFilePointerEx(h, ee, NULL, FILE_BEGIN)) {
            if (SetEndOfFile(h)) {
                InterlockedExchange64(&logwritten, 0);
                if (haslogstatus) {
                    logwrline(h, cnamestamp);
                    logwrtime(h, "Log truncated");
                    logprintf(h, "Log generation   : %lu",
                              InterlockedIncrement(&rotatecount));
                    logconfig(h);
                }
                DBG_PRINTS("truncated");
                InterlockedExchangePointer(&logfhandle, h);
                goto finished;
            }
        }
        rc = GetLastError();
        xsyserror(rc, L"truncatelogs", NULL);
        CloseHandle(h);
    }
    else {
        CloseHandle(h);
        rc = openlogfile(FALSE);
    }
    if (rc == 0) {
        if (haslogstatus) {
            logwrtime(logfhandle, "Log rotated");
            logprintf(logfhandle, "Log generation   : %lu",
                      InterlockedIncrement(&rotatecount));
            logconfig(logfhandle);
        }
        DBG_PRINTS("rotated");
    }
    else {
        setsvcstatusexit(rc);
        xsyserror(0, L"rotatelogs failed", NULL);
    }

finished:
    InterlockedExchange(&rotateruns, 0);
    LeaveCriticalSection(&logfilelock);
    return rc;
}

static void closelogfile(void)
{
    HANDLE h;
    DWORD  r;

    DBG_PRINTS("started");
    EnterCriticalSection(&logfilelock);
    h = InterlockedExchangePointer(&logfhandle, NULL);
    if (h) {
        BOOL f = TRUE;

        DBG_PRINTS("closing");
        if (IS_VALID_HANDLE(pipedprocess)) {
            if (!GetExitCodeProcess(pipedprocess, &r) || (r != STILL_ACTIVE))
                f = FALSE;
        }
        if (f) {
            DBG_PRINTS("flushing");
            if (haslogstatus) {
                logfflush(h);
                logwrtime(h, "Log closed");
            }
            FlushFileBuffers(h);
        }
        CloseHandle(h);
    }
    LeaveCriticalSection(&logfilelock);
    if (IS_VALID_HANDLE(pipedprocess)) {
        DBG_PRINTF("wait for log process %lu to finish", pipedprocpid);
        if (WaitForSingleObject(pipedprocess, SVCBATCH_STOP_STEP) == WAIT_TIMEOUT) {
            DBG_PRINTF("terminating log process %lu", pipedprocpid);
            killproctree(pipedprocess, pipedprocpid, 0);
        }
#if defined(_DEBUG)
        if (GetExitCodeProcess(pipedprocess, &r)) {
            DBG_PRINTF("log process returned %lu", r);
        }
#endif
        SAFE_CLOSE_HANDLE(pipedprocess);
    }
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
        if (siz < KILOBYTES(1)) {
            DBG_PRINTF("rotate size %S is less then 1K", rp);
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
                rotateint = mm * ONE_MINUTE * CPP_INT64_C(-1);
                DBG_PRINTF("rotate each %ld minutes", mm);
                rotatetmo.QuadPart = rotateint;
                rotatebytime = TRUE;
            }
        }
    }
    return TRUE;
}

static DWORD runshutdown(DWORD rt)
{
    wchar_t  rp[TBUFSIZ] = { L':', L':', L' ', L'-' };
    wchar_t *cmdline = NULL;
    HANDLE   wh[2];
    DWORD    rc = 0;
    int   i, ip = 4;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;

    DBG_PRINTS("started");

    xmemzero(&cp, 1, sizeof(PROCESS_INFORMATION));

    rc = createiopipes(&si, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }
    cmdline = xappendarg(1, NULL,    NULL, svcbatchexe);
    if (havelogging && svcendlogfn) {
        if (uselocaltime)
            rp[ip++] = L'l';
        if (truncatelogs || truncateslog)
            rp[ip++] = L't';
        if (haslogstatus)
            rp[ip++] = L'v';
    }
    else {
        rp[ip++] = L'q';
    }
    rp[ip++] = WNUL;
    if (ip < 6)
        rp[2] = WNUL;
    cmdline = xappendarg(0, cmdline, NULL,  rp);
    cmdline = xappendarg(0, cmdline, L"-c", localeparam);
    if (havelogging && svcendlogfn) {
        cmdline = xappendarg(1, cmdline, L"-o", servicelogs);
        cmdline = xappendarg(1, cmdline, L"-n", svcendlogfn);
    }
    for (i = 0; i < svcstopwargc; i++)
        cmdline = xappendarg(1, cmdline, NULL, svcstopwargv[i]);
    DBG_PRINTF("cmdline %S", cmdline);
    if (!CreateProcessW(svcbatchexe, cmdline, NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_CONSOLE,
                        NULL, NULL, &si, &cp)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", NULL);
        goto finished;
    }

    wh[0] = cp.hProcess;
    wh[1] = processended;
    ResumeThread(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);

    DBG_PRINTF("waiting for shutdown process %lu to finish", cp.dwProcessId);
    rc = WaitForMultipleObjects(2, wh, FALSE, rt + SVCBATCH_STOP_STEP);
#if defined(_DEBUG)
    switch (rc) {
        case WAIT_OBJECT_0:
            DBG_PRINTF("done with shutdown process %lu",
                      cp.dwProcessId);
        break;
        case WAIT_OBJECT_1:
            DBG_PRINTF("processended for %lu",
                      cp.dwProcessId);
        break;
        default:
        break;
    }
#endif
    if (rc != WAIT_OBJECT_0) {
        DBG_PRINTF("sending signal event to %lu",
                  cp.dwProcessId);
        SetEvent(ssignalevent);
        if (WaitForSingleObject(cp.hProcess, rt) != WAIT_OBJECT_0) {
            DBG_PRINTF("calling TerminateProcess for %lu",
                       cp.dwProcessId);
            killproctree(cp.hProcess, cp.dwProcessId, 1);
        }
    }
#if defined(_DEBUG)
    if (GetExitCodeProcess(cp.hProcess, &rc))
        DBG_PRINTF("shutdown process exited with %lu", rc);
    else
        DBG_PRINTF("shutdown GetExitCodeProcess failed with %lu", GetLastError());

#endif

finished:
    xfree(cmdline);
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hProcess);

    DBG_PRINTS("done");
    return rc;
}

static DWORD WINAPI stopthread(void *unused)
{

#if defined(_DEBUG)
    if (servicemode)
        DBG_PRINTS("service stop");
    else
        DBG_PRINTS("shutdown stop");
#endif

    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
    if (svcstopwargc) {
        DWORD rc;

        DBG_PRINTS("creating shutdown process");
        rc = runshutdown(SVCBATCH_STOP_CHECK);
        DBG_PRINTF("runshutdown returned %lu", rc);
        if (WaitForSingleObject(processended, 0) == WAIT_OBJECT_0) {
            DBG_PRINTS("processended by shutdown");
            goto finished;
        }
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
        if (rc == 0) {
            if (WaitForSingleObject(ssignalevent, 0) == WAIT_OBJECT_0) {
                DBG_PRINTS("shutdown signal event set");
            }
            else {
                DBG_PRINTS("wait for processended");
                if (WaitForSingleObject(processended, SVCBATCH_STOP_STEP) == WAIT_OBJECT_0) {
                    DBG_PRINTS("processended");
                    goto finished;
                }
            }
        }
    }
    if (SetConsoleCtrlHandler(NULL, TRUE)) {
        DWORD ws;

#if defined(_DEBUG)
        DBG_PRINTS("generating CTRL_C_EVENT");
#endif
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        ws = WaitForSingleObject(processended, SVCBATCH_STOP_STEP);
        SetConsoleCtrlHandler(NULL, FALSE);
        if (ws == WAIT_OBJECT_0) {
            DBG_PRINTS("processended by CTRL_C_EVENT");
            goto finished;
        }
    }
#if defined(_DEBUG)
    else {
        DBG_PRINTF("SetConsoleCtrlHandler failed err=%lu", GetLastError());
    }
    DBG_PRINTS("process still running");
#endif
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    DBG_PRINTS("child is still active ... terminating");

    killproctree(childprocess, childprocpid, 0);
    SAFE_CLOSE_HANDLE(childprocess);

finished:
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    SetEvent(svcstopended);
    DBG_PRINTS("done");
    return 0;
}

static void createstopthread(DWORD rv)
{
    if (InterlockedIncrement(&sstarted) == 1) {
        if (rv)
            setsvcstatusexit(rv);

        ResetEvent(svcstopended);
        xcreatethread(1, 0, stopthread, NULL);
    }
}

static DWORD logwrdata(BYTE *buf, DWORD len)
{
    DWORD  rc;
    HANDLE h;

    EnterCriticalSection(&logfilelock);
    h = InterlockedExchangePointer(&logfhandle, NULL);

    if (h)
        rc = logappend(h, buf, len);
    else
        rc = ERROR_NO_MORE_FILES;

    InterlockedExchangePointer(&logfhandle, h);
    if (rc == 0 && rotatebysize) {
        if (InterlockedCompareExchange64(&logwritten, 0, 0) >= rotatesiz.QuadPart) {
            InterlockedExchange64(&logwritten, 0);
            if (InterlockedCompareExchange(&rotateruns, 1, 0) == 0) {
                DBG_PRINTS("rotating by size");
                SetEvent(logrotatesig);
            }
        }
    }
    LeaveCriticalSection(&logfilelock);
    if (rc) {
        if (rc != ERROR_NO_MORE_FILES) {
            xsyserror(rc, L"logappend", NULL);
            createstopthread(rc);
        }
    }
    return rc;
}

static DWORD WINAPI wrpipethread(void *wh)
{
    DWORD wr;
    DWORD rc = 0;

    DBG_PRINTS("started");
    if (WriteFile(wh, YYES, 1, &wr, NULL) && (wr != 0)) {
        if (!FlushFileBuffers(wh))
            rc = GetLastError();
    }
    else {
        rc = GetLastError();
    }
#if defined(_DEBUG)
    if (rc) {
        if (rc == ERROR_BROKEN_PIPE)
            DBG_PRINTS("pipe closed");
        else
            DBG_PRINTF("err=%lu", rc);
    }
    DBG_PRINTS("done");
#endif
    return rc;
}

static void monitorshutdown(void)
{
    HANDLE wh[4];
    HANDLE h;
    DWORD  ws;

    DBG_PRINTS("started");

    wh[0] = processended;
    wh[1] = monitorevent;
    wh[2] = ssignalevent;

    ws = WaitForMultipleObjects(3, wh, FALSE, INFINITE);
    switch (ws) {
        case WAIT_OBJECT_0:
            DBG_PRINTS("processended signaled");
        break;
        case WAIT_OBJECT_1:
            DBG_PRINTS("monitorevent signaled");
        break;
        case WAIT_OBJECT_2:
            DBG_PRINTS("shutdown stop signaled");
            if (haslogstatus) {
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h) {
                    logfflush(h);
                    logwrline(h, "Received shutdown stop signal");
                }
                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
            }
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
            createstopthread(0);
        break;
        default:
        break;
    }
    DBG_PRINTS("done");
}

static void monitorservice(void)
{
    HANDLE wh[2];
    BOOL   rc = TRUE;

    DBG_PRINTS("started");

    wh[0] = processended;
    wh[1] = monitorevent;

    do {
        DWORD ws, cc;

        ws = WaitForMultipleObjects(2, wh, FALSE, INFINITE);
        switch (ws) {
            case WAIT_OBJECT_0:
                DBG_PRINTS("processended signaled");
                rc = FALSE;
            break;
            case WAIT_OBJECT_1:
                cc = (DWORD)InterlockedExchange(&monitorsig, 0);
                if (cc == 0) {
                    DBG_PRINTS("quit signaled");
                    rc = FALSE;
                }
                else if (cc == SVCBATCH_CTRL_BREAK) {
                    DBG_PRINTS("service SVCBATCH_CTRL_BREAK signaled");
                    if (haslogstatus) {
                        HANDLE h;

                        EnterCriticalSection(&logfilelock);
                        h = InterlockedExchangePointer(&logfhandle, NULL);

                        if (h) {
                            logfflush(h);
                            logwrline(h, "Signaled SVCBATCH_CTRL_BREAK");
                        }
                        InterlockedExchangePointer(&logfhandle, h);
                        LeaveCriticalSection(&logfilelock);
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
                }
                else {
                    DBG_PRINTF("unknown control %lu", cc);
                }
                ResetEvent(monitorevent);
            break;
            default:
                rc = FALSE;
            break;
        }
    } while (rc);

    DBG_PRINTS("done");
}

static DWORD WINAPI monitorthread(void *unused)
{
    if (servicemode)
        monitorservice();
    else
        monitorshutdown();

    return 0;
}


static DWORD WINAPI rotatethread(void *unused)
{
    HANDLE wh[4];
    HANDLE wt = NULL;
    DWORD  rc = 0;
    DWORD  nw = 2;

    DBG_PRINTF("started");

    wh[0] = processended;
    wh[1] = logrotatesig;
    wh[2] = NULL;

    if (rotatetmo.QuadPart) {
        wt = CreateWaitableTimer(NULL, TRUE, NULL);
        if (IS_INVALID_HANDLE(wt)) {
            goto finished;
        }
        if (!SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"SetWaitableTimer", NULL);
            goto finished;
        }
        wh[nw++] = wt;
    }

    while (rc == 0) {
        DWORD wc = WaitForMultipleObjects(nw, wh, FALSE, INFINITE);

        if ((InterlockedCompareExchange(&sstarted, 0, 0) > 0) && (wc != WAIT_OBJECT_0)) {
            DBG_PRINTS("service stop is running");
            wc = ERROR_NO_MORE_FILES;
        }
        switch (wc) {
            case WAIT_OBJECT_0:
                rc = ERROR_PROCESS_ABORTED;
                DBG_PRINTS("processended signaled");
            break;
            case WAIT_OBJECT_1:
                DBG_PRINTS("rotate by signal");
                rc = rotatelogs();
                if (rc == 0) {
                    if (IS_VALID_HANDLE(wt) && (rotateint < 0)) {
                        CancelWaitableTimer(wt);
                        SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE);
                    }
                }
                else {
                    xsyserror(rc, L"rotatelogs", NULL);
                    createstopthread(rc);
                }
                ResetEvent(logrotatesig);
            break;
            case WAIT_OBJECT_2:
                if (InterlockedCompareExchange(&rotateruns, 1, 0) == 0) {
                    DBG_PRINTS("rotate by time");
                    rc = rotatelogs();
                    if (rc) {
                        xsyserror(rc, L"rotatelogs", NULL);
                        createstopthread(rc);
                    }
                }
                CancelWaitableTimer(wt);
                if (rotateint > 0)
                    rotatetmo.QuadPart += rotateint;
                SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE);
                ResetEvent(logrotatesig);
            break;
            default:
                rc = wc;
            break;
        }
    }

finished:
    DBG_PRINTS("done");
    SAFE_CLOSE_HANDLE(wt);
    return 0;
}

static DWORD WINAPI workerthread(void *unused)
{
    int i;
    wchar_t *cmdline = NULL;
    HANDLE   wh[2];
    HANDLE   wrthread;
    HANDLE   wr = NULL;
    HANDLE   rd = NULL;
    DWORD    rc = 0;
    DWORD    nw = 2;
    PROCESS_INFORMATION cp;
    STARTUPINFOW  si;
    SVCBATCH_PIPE op;

    DBG_PRINTS("started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    xmemzero(&cp, 1, sizeof(PROCESS_INFORMATION));
    xmemzero(&op, 1, sizeof(SVCBATCH_PIPE));

    rc = createiopipes(&si, &wr, &rd, FILE_FLAG_OVERLAPPED);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        goto finished;
    }
    cmdline = xappendarg(1, NULL,    NULL,     comspec);
    cmdline = xappendarg(1, cmdline, L"/D /C", svcbatchfile);
    for (i = 0; i < svcbatchargc; i++) {
        xwchreplace(svcbatchargv[i], L'@', L'%');
        cmdline = xappendarg(1, cmdline, NULL, svcbatchargv[i]);
    }

    op.pipe     = rd;
    op.o.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (IS_INVALID_HANDLE(op.o.hEvent))
        goto finished;

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    DBG_PRINTF("cmdline %S", cmdline);
    if (!CreateProcessW(comspec, cmdline, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        NULL,
                        servicehome,
                       &si, &cp)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        xsyserror(rc, L"CreateProcess", NULL);
        goto finished;
    }
    childprocess = cp.hProcess;
    childprocpid = cp.dwProcessId;
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);

    wrthread = xcreatethread(0, 1, wrpipethread, wr);
    if (IS_INVALID_HANDLE(wrthread)) {
        xsyserror(GetLastError(), L"xcreatethread", NULL);
        goto finished;
    }
    wh[0] = childprocess;
    wh[1] = op.o.hEvent;

    ResumeThread(cp.hThread);
    ResumeThread(wrthread);

    SAFE_CLOSE_HANDLE(cp.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    DBG_PRINTF("running child with pid %lu", childprocpid);
    if (haslogrotate)
        xcreatethread(1, 0, rotatethread, NULL);

    do {
        DWORD ws;
        DWORD rr;

        ws = WaitForMultipleObjects(nw, wh, FALSE, INFINITE);
        switch (ws) {
            case WAIT_OBJECT_0:
                nw = 0;
                DBG_PRINTF("childprocess %lu done", childprocpid);
            break;
            case WAIT_OBJECT_1:
                if (op.stat == ERROR_IO_PENDING) {
                    if (!GetOverlappedResult(op.pipe,
                                             (LPOVERLAPPED)&op,
                                             &rr,
                                             FALSE)) {
                        op.stat = GetLastError();
                    }
                    else {
                        op.stat = 0;
                        if (havelogging)
                            rc = logwrdata(op.buff, rr);
                    }
                    break;
                }
                if (ReadFile(op.pipe, op.buff, HBUFSIZ,
                             &rr, (LPOVERLAPPED)&op) && rr) {
                    op.stat = 0;
                    if (havelogging)
                        rc = logwrdata(op.buff, rr);
                    SetEvent(op.o.hEvent);
                }
                else {
                    op.stat = GetLastError();
                    if (op.stat != ERROR_IO_PENDING) {
                        SAFE_CLOSE_HANDLE(op.pipe);
                        ResetEvent(op.o.hEvent);
                        rc = op.stat;
                        nw = 1;
                    }
                }
#if defined(_DEBUG)
                if (rc) {
                    if ((rc == ERROR_BROKEN_PIPE) || (rc == ERROR_NO_DATA))
                        DBG_PRINTS("pipe closed");
                    else if (rc == ERROR_NO_MORE_FILES)
                        DBG_PRINTS("logfile closed");
                    else
                        DBG_PRINTF("err=%lu", rc);
                }
#endif
            break;
            default:
                DBG_PRINTF("wait failed %lu with %lu", ws, GetLastError());
                nw = 0;
            break;

        }
    } while (nw);
    WaitForSingleObject(wrthread, INFINITE);
    CloseHandle(wrthread);

    DBG_PRINTF("finished %S with pid %lu",
              svcbatchname, childprocpid);
    if (IS_INVALID_HANDLE(childprocess)) {
        DBG_PRINTF("%S child process is closed", svcbatchname);
        goto finished;
    }
    if (GetExitCodeProcess(childprocess, &rc)) {
        DBG_PRINTF("%S exited with %lu", svcbatchname, rc);
    }
    else {
        rc = GetLastError();
        DBG_PRINTF("GetExitCodeProcess failed with %lu", rc);
    }
    if (rc) {
        if (rc != 255) {
            /**
              * 255 is exit code when CTRL_C is send to cmd.exe
              */
            setsvcstatusexit(ERROR_PROCESS_ABORTED);
        }
    }

finished:
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    SAFE_CLOSE_HANDLE(wr);
    SAFE_CLOSE_HANDLE(op.pipe);
    SAFE_CLOSE_HANDLE(op.o.hEvent);
    SetEvent(processended);

    DBG_PRINTS("done");
    return 0;
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
        case SERVICE_CONTROL_STOP:
            DBG_PRINTF("service stop control code: 0x%08X", ctrl);
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
            if (InterlockedIncrement(&sstarted) == 1) {
                ResetEvent(svcstopended);
                if (haslogstatus) {
                    HANDLE h;
                    EnterCriticalSection(&logfilelock);
                    h = InterlockedExchangePointer(&logfhandle, NULL);
                    if (h) {
                        logfflush(h);
                        logwrline(h, "Service signaled : SERVICE_CONTROL_STOP");
                    }
                    InterlockedExchangePointer(&logfhandle, h);
                    LeaveCriticalSection(&logfilelock);
                }
                xcreatethread(1, 0, stopthread, NULL);
            }
            else {
                DBG_PRINTS("service stop is already running");
            }
        break;
        case SVCBATCH_CTRL_BREAK:
            if (hasctrlbreak) {
                DBG_PRINTS("raising SVCBATCH_CTRL_BREAK");
                InterlockedExchange(&monitorsig, ctrl);
                SetEvent(monitorevent);
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
                if (InterlockedCompareExchange(&rotateruns, 1, 0) == 0) {
                    DBG_PRINTS("signaling SVCBATCH_CTRL_ROTATE");
                    SetEvent(logrotatesig);
                }
                else {
                    DBG_PRINTS("rotatelogs already running");
                    return ERROR_ALREADY_EXISTS;
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

static void WINAPI servicemain(DWORD argc, wchar_t **argv)
{
    DWORD  rv = 0;
    HANDLE wh[4] = { NULL, NULL, NULL, NULL };
    DWORD  ws;

    ssvcstatus.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    ssvcstatus.dwCurrentState = SERVICE_START_PENDING;

    if (IS_EMPTY_WCS(servicename) && (argc > 0))
        servicename = xwcsdup(argv[0]);
    if (IS_EMPTY_WCS(servicename)) {
        xsyserror(ERROR_INVALID_PARAMETER, L"Missing servicename", NULL);
        exit(ERROR_INVALID_PARAMETER);
        return;
    }
    if (servicemode) {
        hsvcstatus = RegisterServiceCtrlHandlerExW(servicename, servicehandler, NULL);
        if (IS_INVALID_HANDLE(hsvcstatus)) {
            return;
        }
        DBG_PRINTF("started %S", servicename);
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

        if (havelogging) {
            rv = createlogsdir();
            if (rv) {
                reportsvcstatus(SERVICE_STOPPED, rv);
                return;
            }
            SetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS", servicelogs);
        }
        else {
            wchar_t *tmp = xgetenv(L"TEMP");
            if (tmp == NULL) {
                xsyserror(GetLastError(), L"Missing TEMP environment variable", NULL);
                reportsvcstatus(SERVICE_STOPPED, ERROR_BAD_ENVIRONMENT);
                return;

            }
            SetEnvironmentVariableW(L"SVCBATCH_SERVICE_LOGS", tmp);
            xfree(tmp);
        }
    }
    else {
        DBG_PRINTF("shutting down %S", servicename);
        servicelogs = xwcsdup(outdirparam);
    }
    if (servicemode) {
        /**
         * Add additional environment variables
         * They are unique to this service instance
         */
        SetEnvironmentVariableW(L"SVCBATCH_SERVICE_BASE", servicebase);
        SetEnvironmentVariableW(L"SVCBATCH_SERVICE_HOME", servicehome);
        SetEnvironmentVariableW(L"SVCBATCH_SERVICE_NAME", servicename);
        SetEnvironmentVariableW(L"SVCBATCH_SERVICE_UUID", serviceuuid);

    }
    if (havelogging) {
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
        if (haspipedlogs)
            rv = openlogpipe(TRUE);
        else
            rv = openlogfile(TRUE);

        if (rv != 0) {
            xsyserror(0, L"openlog failed", NULL);
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        if (haslogstatus)
            logconfig(logfhandle);
    }
    else {
        DBG_PRINTS("logging is disabled");
    }
    SetConsoleCtrlHandler(consolehandler, TRUE);
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    wh[1] = xcreatethread(0, 0, monitorthread, NULL);
    if (IS_INVALID_HANDLE(wh[1])) {
        xsyserror(GetLastError(), L"xcreatethread", NULL);
        goto finished;
    }
    wh[0] = xcreatethread(0, 0, workerthread, NULL);
    if (IS_INVALID_HANDLE(wh[0])) {
        xsyserror(GetLastError(), L"xcreatethread", NULL);
        SetEvent(processended);
        goto finished;
    }
    DBG_PRINTS("running");
    WaitForMultipleObjects(2, wh, TRUE, INFINITE);
    SAFE_CLOSE_HANDLE(wh[0]);
    SAFE_CLOSE_HANDLE(wh[1]);

    if (WaitForSingleObject(svcstopended, 0) == WAIT_OBJECT_0) {
        DBG_PRINTS("stopped");
    }
    else {
        DBG_PRINTS("waiting for stopthread to finish");
        ws = WaitForSingleObject(svcstopended, SVCBATCH_STOP_HINT);
        if (ws == WAIT_TIMEOUT) {
            if (svcstopwargc) {
                DBG_PRINTS("sending shutdown stop signal");
                SetEvent(ssignalevent);
                ws = WaitForSingleObject(svcstopended, SVCBATCH_STOP_CHECK);
            }
        }
        DBG_PRINTF("stopthread status=%lu", ws);
    }

finished:

    SAFE_CLOSE_HANDLE(childprocess);
    closelogfile();

    reportsvcstatus(SERVICE_STOPPED, rv);
    DBG_PRINTS("done");
}

static void __cdecl cconsolecleanup(void)
{
    SetConsoleCtrlHandler(consolehandler, FALSE);
    FreeConsole();
}

static void __cdecl objectscleanup(void)
{
    SAFE_CLOSE_HANDLE(processended);
    SAFE_CLOSE_HANDLE(svcstopended);
    SAFE_CLOSE_HANDLE(ssignalevent);
    SAFE_CLOSE_HANDLE(monitorevent);
    SAFE_CLOSE_HANDLE(logrotatesig);

    DeleteCriticalSection(&logfilelock);
    DeleteCriticalSection(&servicelock);
}

static int xwmaininit(void)
{
    wchar_t *bb;
    DWORD    sz = FBUFSIZ;
    DWORD    nn;

    xmemzero(svcthread, SVCBATCH_MAX_THREADS, sizeof(SVCBATCH_THREAD));
    svcthread[0].h  = GetCurrentProcess();
    svcthread[0].id = GetCurrentProcessId();

    bb = xwmalloc(sz);
    nn = GetModuleFileNameW(NULL, bb, sz);
    if ((nn == 0) || (nn >= sz))
        return ERROR_BAD_PATHNAME;
    nn = fixshortpath(bb, nn);
    while (--nn > 2) {
        if (bb[nn] == L'\\') {
            bb[nn] = WNUL;
            exelocation = xwcsdup(bb);
            bb[nn] = L'\\';
            break;
        }
    }
    if (exelocation == NULL)
        return ERROR_BAD_PATHNAME;
    svcbatchexe = bb;
    if (!QueryPerformanceFrequency(&pcfrequency))
        return GetLastError();
    if (!QueryPerformanceCounter(&pcstarttime))
        return GetLastError();

    return 0;
}

int wmain(int argc, const wchar_t **wargv)
{
    int         i;
    int         opt;
    int         ncnt  = 0;
    int         rcnt  = 0;
    int         rv;
    wchar_t     bb[BBUFSIZ] = { L'-', WNUL, WNUL, WNUL };

    HANDLE      h;
    SERVICE_TABLE_ENTRYW se[2];
    const wchar_t *maxlogsparam = NULL;
    const wchar_t *batchparam   = NULL;
    const wchar_t *svchomeparam = NULL;
    const wchar_t *rparam[2];
    wchar_t       *nparam[2] = { NULL, NULL };

#if defined(_DEBUG)
# if defined(_MSC_VER) && (_MSC_VER < 1900)
    /* Not supported */
# else
    _set_invalid_parameter_handler(xiphandler);
# endif
   _CrtSetReportMode(_CRT_ASSERT, 0);
# if (_DEBUG > 1)
    {
        wchar_t dbgnb[BBUFSIZ] = L"";

        GetTempPathW(BBUFSIZ - TBUFSIZ, dbgnb);
        xwcslcat(dbgnb, BBUFSIZ, L"svcbatch_debug.log");
        dbgfile = _wfopen(dbgnb, L"atc");
        if (dbgfile) {
            long sz;

            sz = _filelength(_fileno(dbgfile));
            if (sz > 0) {
                fputs("\n\n", dbgfile);
                fflush(dbgfile);
            }
        }
    }
# endif
#endif
    /**
     * Make sure children (cmd.exe) are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);

    rv = xwmaininit();
    ASSERT_TRUE(rv, rv);

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
    }
    /**
     * Check if running as service or as a child process.
     */
    if (argc > 1) {
        const wchar_t *p = wargv[1];
        if ((p[0] == L':') && (p[1] == L':') && (p[2] == WNUL)) {
            servicemode = FALSE;
            servicename = xgetenv(L"SVCBATCH_SERVICE_NAME");
            if (servicename == NULL)
                return ERROR_BAD_ENVIRONMENT;
            cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT ;
            cwsappname  = CPP_WIDEN(SHUTDOWN_APPNAME);
            wargv[1] = wargv[0];
            argc    -= 1;
            wargv   += 1;
        }
    }
    ASSERT_SIZE(argc, 2, ERROR_INVALID_PARAMETER);
    xmbstowcs(0, wnamestamp, RBUFSIZ, cnamestamp);
    DBG_PRINTS(cnamestamp);

    while ((opt = xwgetopt(argc, wargv, L"bc:e:lm:n:o:pqr:s:tvw:")) != EOF) {
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
            case L'q':
                havelogging  = FALSE;
            break;
            case L't':
                truncateslog = TRUE;
                if (truncatelogs)
                    truncatelogs = FALSE;
                else
                    truncatelogs = TRUE;

            break;
            case L'v':
                haslogstatus = TRUE;
            break;
            /**
             * Options with arguments
             */
            case L'c':
                localeparam  = xwoptarg;
            break;
            case L'm':
                maxlogsparam = xwoptarg;
            break;
            case L'o':
                outdirparam  = xwoptarg;
            break;
            case L'w':
                svchomeparam = xwoptarg;
            break;
            /**
             * Options that can be defined
             * multiple times
             */
            case L'e':
                if (logredirargc < SVCBATCH_MAX_ARGS)
                    logredirargv[logredirargc++] = xwcsdup(xwoptarg);
                else
                    return xsyserror(0, L"Too many -e arguments", xwoptarg);
            break;
            case L's':
                if (svcstopwargc < SVCBATCH_MAX_ARGS)
                    svcstopwargv[svcstopwargc++] = xwcsdup(xwoptarg);
                else
                    return xsyserror(0, L"Too many -s arguments", xwoptarg);
            break;
            case L'n':
                if (ncnt > 1)
                    return xsyserror(0, L"Too many -n options", xwoptarg);

                nparam[ncnt] = xwcsdup(xwoptarg);
                if (servicemode) {
                    if (wcspbrk(xwoptarg, L"/\\:;<>?*|\""))
                        return xsyserror(0, L"Found invalid filename characters", xwoptarg);
                    /**
                     * If name is strftime formatted
                     * replace @ with % so it can be used by strftime
                     */
                    xwchreplace(nparam[ncnt], L'@', L'%');
                }
                ncnt++;
            break;
            case L'r':
                if (rcnt > 1)
                    return xsyserror(0, L"Too many -r options", xwoptarg);
                else
                    rparam[rcnt++] = xwoptarg;
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

    localeobject = _get_current_locale();
    if (localeparam) {
        if (*localeparam == L'.') {
            if (xwstartswith(localeparam + 1, L"UTF")) {
                if (wcschr(localeparam + 4, L'8'))
                    svccodepage = 65001;
                else
                    return xsyserror(0, L"Invalid -c command option value", localeparam);
            }
            else {
                svccodepage = xwcstoi(localeparam + 1, NULL);
                if (svccodepage < 0)
                    return xsyserror(0, L"Invalid -c command option value", localeparam);
            }
            DBG_PRINTF("using codepage %d", svccodepage);
        }
#if defined(_MSC_VER)
        else {
            localeobject = _wcreate_locale(LC_ALL, localeparam);
            if (localeobject == NULL)
                return xsyserror(0, L"Invalid -c command option value", localeparam);
        }
#endif
    }

    argc  -= xwoptind;
    wargv += xwoptind;
    if (argc > 0) {
        batchparam = wargv[0];
        for (i = 1; i < argc; i++) {
            /**
             * Add arguments for batch file
             */
            if (svcbatchargc < SVCBATCH_MAX_ARGS)
                svcbatchargv[svcbatchargc++] = xwcsdup(wargv[i]);
            else
                return xsyserror(0, L"Too many batch arguments", wargv[i]);
        }
    }
    if (IS_EMPTY_WCS(batchparam))
        return xsyserror(0, L"Missing batch file", NULL);
    if (servicemode)
        serviceuuid = xuuidstring(NULL);
    else
        serviceuuid = xgetenv(L"SVCBATCH_SERVICE_UUID");
    if (IS_EMPTY_WCS(serviceuuid))
        return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_UUID", NULL);
    comspec = xgetenv(L"COMSPEC");
    if (IS_EMPTY_WCS(comspec))
        return xsyserror(GetLastError(), L"COMSPEC", NULL);

    if (havelogging) {
        if (servicemode) {
            if (logredirargc == 0) {
                svcmaxlogs = SVCBATCH_MAX_LOGS;
                if (maxlogsparam) {
                    svcmaxlogs = xwcstoi(maxlogsparam, NULL);
                    if ((svcmaxlogs < 0) || (svcmaxlogs > SVCBATCH_MAX_LOGS))
                        return xsyserror(0, L"Invalid -m command option value", maxlogsparam);
                }
                if (svcmaxlogs)
                    haslogrotate = TRUE;
            }
            if (ncnt == 0)
                nparam[ncnt++] = xwcsdup(SVCBATCH_LOGNAME);
            if (svcstopwargc && (ncnt < 2))
                nparam[ncnt++] = xwcsdup(SHUTDOWN_LOGNAME);
        }
        if (outdirparam == NULL)
            outdirparam = SVCBATCH_LOGSDIR;
    }
    else {
        /**
         * Ensure that log related command options
         * are not defined when -q is defined
         */
        bb[0] = WNUL;
        if (logredirargc)
            xwcslcat(bb, TBUFSIZ, L"-e ");
        if (maxlogsparam)
            xwcslcat(bb, TBUFSIZ, L"-m ");
        if (ncnt)
            xwcslcat(bb, TBUFSIZ, L"-n ");
        if (outdirparam)
            xwcslcat(bb, TBUFSIZ, L"-o ");
        if (rcnt)
            xwcslcat(bb, TBUFSIZ, L"-r ");
        if (truncatelogs)
            xwcslcat(bb, TBUFSIZ, L"-t ");
        if (haslogstatus)
            xwcslcat(bb, TBUFSIZ, L"-v ");
        if (bb[0]) {
#if defined(_DEBUG)
            DBG_PRINTF("extra option(s) %S", bb);
#else
            return xsyserror(0, L"Option -q is mutually exclusive with option(s)", bb);
#endif
        }
    }
    if (servicemode) {
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
         *    and is resloved as valid path set it as home directory.
         *    If -w was defined and cannot be resolved fail.
         *
         * 3. Use running svcbatch.exe directory and set it as
         *    SetCurrentDirectory.
         *    If -w parameter was defined, resolve it and set as home
         *    directory or fail.
         *    In case -w was not defined reslove batch file and set its
         *    directory as home directory or fail if it cannot be reolved.
         *
         */

         if (svchomeparam) {
             if (isrelativepath(svchomeparam)) {

             }
             else {
                servicehome = getrealpathname(svchomeparam, 1);
                if (IS_EMPTY_WCS(servicehome))
                    return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
             }
         }
         if (servicehome == NULL) {
            if (isrelativepath(batchparam)) {
                /**
                 * No -w param and batch file is relative
                 * so we will use svcbatch.exe directory
                 */
            }
            else {
                if (!resolvebatchname(batchparam))
                    return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);

                if (svchomeparam == NULL) {
                    servicehome = servicebase;
                }
                else {
                    SetCurrentDirectoryW(servicebase);
                    servicehome = getrealpathname(svchomeparam, 1);
                    if (IS_EMPTY_WCS(servicehome))
                        return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
                }
            }
         }
         if (servicehome == NULL) {
            if (svchomeparam == NULL) {
                servicehome = exelocation;
            }
            else {
                SetCurrentDirectoryW(exelocation);
                servicehome = getrealpathname(svchomeparam, 1);
                if (IS_EMPTY_WCS(servicehome))
                    return xsyserror(ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
            }
         }
         SetCurrentDirectoryW(servicehome);
         if (svcbatchfile == NULL) {
            if (!resolvebatchname(batchparam))
                return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);
         }
    }
    else {
        servicehome = xgetenv(L"SVCBATCH_SERVICE_HOME");
        if (servicehome == NULL)
            return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_HOME", NULL);
    }
    if (!resolvebatchname(batchparam))
        return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);

    if (servicemode) {
        if (svcstopwargc) {
            wchar_t *p = svcstopwargv[0];

            svcstopwargv[0] = getrealpathname(p, 0);
            if (svcstopwargv[0] == NULL)
                return xsyserror(ERROR_FILE_NOT_FOUND, p, NULL);
            xfree(p);
        }
        if (ncnt) {
            svclogfname = nparam[0];
            if (ncnt > 1) {
                if (_wcsicmp(nparam[0], nparam[1]) == 0) {
                    return xsyserror(0, L"Log and shutdown file cannot have the same name", svclogfname);
                }
                else {
                    if (_wcsicmp(nparam[1], L"NUL"))
                        svcendlogfn = nparam[1];
                }
            }
        }
        if (logredirargc) {
            wchar_t *p = logredirargv[0];

            logredirargv[0] = getrealpathname(p, 0);
            if (logredirargv[0] == NULL)
                return xsyserror(ERROR_FILE_NOT_FOUND, p, NULL);
            haspipedlogs = TRUE;
            xfree(p);
        }
    }
    else {
        svclogfname = nparam[0];
    }

    xmemzero(&ssvcstatus, 1, sizeof(SERVICE_STATUS));
    xmemzero(&sazero,     1, sizeof(SECURITY_ATTRIBUTES));
    sazero.nLength = DSIZEOF(SECURITY_ATTRIBUTES);
    /**
     * Create logic state events
     */
    svcstopended = CreateEvent(NULL, TRUE, TRUE,  NULL);
    if (IS_INVALID_HANDLE(svcstopended))
        return xsyserror(GetLastError(), L"CreateEvent", NULL);
    processended = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(processended))
        return xsyserror(GetLastError(), L"CreateEvent", NULL);
    if (servicemode) {
        if (svcstopwargc) {
            xwcslcpy(bb, RBUFSIZ, SHUTDOWN_IPCNAME);
            xwcslcat(bb, RBUFSIZ, serviceuuid);
            ssignalevent = CreateEventW(&sazero, TRUE, FALSE, bb);
            if (IS_INVALID_HANDLE(ssignalevent))
                return xsyserror(GetLastError(), L"CreateEvent", bb);
        }
        if (haslogrotate) {
            for (i = 0; i < rcnt; i++) {
                if (!resolverotate(rparam[i]))
                    return xsyserror(0, L"Invalid rotate parameter", rparam[i]);
            }
            logrotatesig = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (IS_INVALID_HANDLE(logrotatesig))
                return xsyserror(GetLastError(), L"CreateEvent", NULL);
        }
    }
    else {
        xwcslcpy(bb, RBUFSIZ, SHUTDOWN_IPCNAME);
        xwcslcat(bb, RBUFSIZ, serviceuuid);
        ssignalevent = OpenEventW(SYNCHRONIZE, FALSE, bb);
        if (IS_INVALID_HANDLE(ssignalevent))
            return xsyserror(GetLastError(), L"OpenEvent", bb);
    }

    monitorevent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(monitorevent))
        return xsyserror(GetLastError(), L"CreateEvent", NULL);
    InitializeCriticalSection(&servicelock);
    InitializeCriticalSection(&logfilelock);
    atexit(objectscleanup);

    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = NULL;
    se[1].lpServiceProc = NULL;
    if (servicemode) {
        DBG_PRINTS("starting service");
        if (!StartServiceCtrlDispatcherW(se))
            rv = xsyserror(GetLastError(),
                           L"StartServiceCtrlDispatcher", NULL);
    }
    else {
        DBG_PRINTS("starting shutdown");
        servicemain(0, NULL);
        rv = ssvcstatus.dwServiceSpecificExitCode;
    }
    DBG_PRINTF("done (%lu)\n", rv);
#if defined(_DEBUG) && (_DEBUG > 1)
    if (dbgfile) {
        fflush(dbgfile);
        fclose(dbgfile);
    }
#endif
    return rv;
}
