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
#include <wincrypt.h>
#include <tlhelp32.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <process.h>
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
#else
# define DBG_PRINTF(Fmt, ...)   (void)0
# define DBG_PRINTS(Msg)        (void)0
#endif

#define xsyserror(_n, _e, _d)   svcsyserror(__FUNCTION__, __LINE__, (_n), (_e), (_d))

typedef union _UI_BYTES {
    unsigned char  b[4];
    struct {
        WORD       lo;
        WORD       hi;
    } DUMMYSTRUCTNAME;
    struct {
        WORD       lo;
        WORD       hi;
    } w;
    unsigned int   u;
} UI_BYTES;

typedef struct _SVCBATCH_THREAD {
    LPTHREAD_START_ROUTINE threadfn;
    LPVOID                 param;
} SVCBATCH_THREAD, *LPSVCBATCH_THREAD;

static volatile LONG         monitorsig  = 0;
static volatile LONG         rotatesig   = 0;
static volatile LONG         sstarted    = 0;
static volatile LONG         sscstate    = SERVICE_START_PENDING;
static volatile LONG         rotatecount = 0;
static volatile LONG         numserial   = 0;
static volatile LONG64       logwritten  = 0;
static volatile HANDLE       logfhandle  = NULL;
static SERVICE_STATUS_HANDLE hsvcstatus  = NULL;
static SERVICE_STATUS        ssvcstatus;
static CRITICAL_SECTION      servicelock;
static CRITICAL_SECTION      logfilelock;
static SECURITY_ATTRIBUTES   sazero;
static LONGLONG              rotateint   = 0;
static LARGE_INTEGER         rotatetmo   = {{ 0, 0 }};
static LARGE_INTEGER         rotatesiz   = {{ 0, 0 }};
static LARGE_INTEGER         pcfrequency;
static LARGE_INTEGER         pcstarttime;

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

static wchar_t  *svcbatchexe      = NULL;
static wchar_t  *svcbatchfile     = NULL;
static wchar_t  *svcbatchname     = NULL;
static wchar_t  *exelocation      = NULL;
static wchar_t  *servicebase      = NULL;
static wchar_t  *servicename      = NULL;
static wchar_t  *servicehome      = NULL;
static wchar_t  *serviceuuid      = NULL;
static wchar_t  *svcbatchargs     = NULL;

static wchar_t  *servicelogs      = NULL;
static wchar_t  *logfilename      = NULL;
static wchar_t  *wnamestamp       = NULL;
static wchar_t  *svclogfname      = NULL;
static wchar_t  *svcendlogfn      = NULL;

static wchar_t **logredirargv     = NULL;
static wchar_t **svcstopwargv     = NULL;

static HANDLE    childprocess     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    ssignalevent     = NULL;
static HANDLE    processended     = NULL;
static HANDLE    monitorevent     = NULL;
static HANDLE    logrotatesig     = NULL;
static HANDLE    outputpiperd     = NULL;
static HANDLE    inputpipewrs     = NULL;
static HANDLE    pipedprocess     = NULL;
static HANDLE    pipedprocout     = NULL;

static wchar_t      zerostring[4] = { WNUL,  WNUL,  WNUL, WNUL };
static wchar_t      CRLFW[4]      = { L'\r', L'\n', WNUL, WNUL };
static char         CRLFA[4]      = { '\r', '\n', '\0', '\0' };
static char         YYES[4]       = { 'Y',  '\r', '\n', '\0' };

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


static const char *xwcsiid(int i, DWORD c)
{
    const char *r = "UNKNOWN";

    switch (i) {
        case II_CONSOLE:
            {
                switch (c) {
                    case CTRL_CLOSE_EVENT:
                        r = "CTRL_CLOSE_EVENT";
                    break;
                    case CTRL_SHUTDOWN_EVENT:
                        r = "CTRL_SHUTDOWN_EVENT";
                    break;
                    case CTRL_C_EVENT:
                        r = "CTRL_C_EVENT";
                    break;
                    case CTRL_BREAK_EVENT:
                        r = "CTRL_BREAK_EVENT";
                    break;
                    case CTRL_LOGOFF_EVENT:
                        r = "CTRL_LOGOFF_EVENT";
                    break;
                    default:
                        r = "CTRL_UNKNOWN";
                    break;
                }
            }
        break;
        case II_SERVICE:
            {
                switch (c) {
                    case SERVICE_CONTROL_PRESHUTDOWN:
                        r = "SERVICE_CONTROL_PRESHUTDOWN";
                    break;
                    case SERVICE_CONTROL_SHUTDOWN:
                        r = "SERVICE_CONTROL_SHUTDOWN";
                    break;
                    case SERVICE_CONTROL_STOP:
                        r = "SERVICE_CONTROL_STOP";
                    break;
                    case SERVICE_CONTROL_INTERROGATE:
                        r = "SERVICE_CONTROL_INTERROGATE";
                    break;
                    case SVCBATCH_CTRL_BREAK:
                        r = "SVCBATCH_CTRL_BREAK";
                    break;
                    case SVCBATCH_CTRL_ROTATE:
                        r = "SVCBATCH_CTRL_ROTATE";
                    break;
                    default:
                        r = "SERVICE_CONTROL_UNKNOWN";
                    break;
                }
            }
        break;
    }

    return r;
}

static int xfatalerr(const char *func, int err)
{
    OutputDebugStringA(">>> " SVCBATCH_NAME " " SVCBATCH_VERSION_STR);
    OutputDebugStringA(func);
    OutputDebugStringA("<<<\n\n");
    _exit(err);
    TerminateProcess(GetCurrentProcess(), err);
    return err;
}

static void *xmmalloc(size_t number, size_t size)
{
    void *p;

    p = malloc(number * size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    return p;
}

static void *xmcalloc(size_t number, size_t size)
{
    void *p;

    p = calloc(number,  size);
    if (p == NULL) {
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    }
    return p;
}

static wchar_t *xwmalloc(size_t size)
{
    wchar_t *p = (wchar_t *)xmmalloc(size + 1, sizeof(wchar_t));

    p[0]    = WNUL;
    p[size] = WNUL;
    return p;
}

static wchar_t *xwcalloc(size_t size)
{
    return (wchar_t *)xmcalloc(size + 1, sizeof(wchar_t));
}

static wchar_t **xwaalloc(size_t size)
{
    return (wchar_t **)xmcalloc(size + 2, sizeof(wchar_t *));
}

static void xfree(void *m)
{
    if (m != NULL)
        free(m);
}

static void xwaafree(wchar_t **a)
{
    if (a != NULL) {
        wchar_t **p = a;

        while (*p != NULL)
            xfree(*(p++));
        free(a);
    }
}

static void xmemzero(void *mem, size_t number, size_t size)
{
    if (mem && number && size)
        memset(mem, 0, number * size);
}

static wchar_t *xwcsdup(const wchar_t *s)
{
    wchar_t *p;
    size_t   n;

    if (IS_EMPTY_WCS(s))
        return NULL;
    n = wcslen(s);
    p = xwmalloc(n);
    wmemcpy(p, s, n);
    return p;
}

static wchar_t *xcwiden(const char *s)
{
    wchar_t *p;
    size_t   i, n;

    if (s == NULL)
        return NULL;
    n = strlen(s);
    p = xwmalloc(n);
    for (i = 0; i < n; i++)
        p[i] = s[i] < 128 ? (wchar_t)s[i] : L'?';

    return p;
}

static void xwchreplace(wchar_t *s, wchar_t c, wchar_t r)
{
    while (*s != WNUL) {
        if (*s == c)
            *s = r;
        s++;
    }
}

static wchar_t *xgetenv(const wchar_t *s)
{
    DWORD    n;
    wchar_t  e[BBUFSIZ];
    wchar_t *d = NULL;

    n = GetEnvironmentVariableW(s, e, BBUFSIZ);
    if (n != 0) {
        d = xwmalloc(n);
        if (n > BBUFSIZ) {
            GetEnvironmentVariableW(s, d, n);
        }
        else {
            wmemcpy(d, e, n);
        }
    }
    if (d == NULL)
        SetLastError( ERROR_ENVVAR_NOT_FOUND);
    return d;
}

static int xwcslen(const wchar_t *s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    else
        return (int)wcslen(s);
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
int xwcslcat(wchar_t *dst, int siz, const wchar_t *src)
{
    const wchar_t *s = src;
    wchar_t *p;
    wchar_t *d = dst;
    size_t   z = siz;
    size_t   n = siz;
    size_t   c;
    size_t   r;

    ASSERT_ZERO(siz, 0);
    ASSERT_WSTR(src, 0);
    while ((n-- != 0) && (*d != WNUL))
        d++;
    c = d - dst;
    n = z - c;

    if (n-- == 0)
        return ((int)c + xwcslen(s));
    p = dst + c;
    while (*s != WNUL) {
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

int xwcslcpy(wchar_t *dst, int siz, const wchar_t *src)
{
    const wchar_t *s = src;
    wchar_t *d = dst;
    int      n = siz;

    ASSERT_WSTR(s, 0);
    while (*s != WNUL) {
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

    cp = xwmalloc(l1 + l2);
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
    cp = xwmalloc(nd + nf + 1);

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

    int l1, l2, l3;

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
    l1 = xwcslen(s1);
    l2 = xwcslen(s2);
    e  = xwmalloc(l1 + l2 + l3 + 4);
    d  = e;

    if(l1) {
        wmemcpy(d, s1, l1);
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
                wmemset(d, L'\\', b * 2);
                d += b * 2;
                break;
            }
            else if (*c == L'"') {
                wmemset(d, L'\\', b * 2 + 1);
                d += b * 2 + 1;
                *(d++) = *c;
            }
            else {
                wmemset(d, L'\\', b);
                d += b;
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
    xfree(s1);

    return e;
}

static int xwstartswith(const wchar_t *str, const wchar_t *src)
{
    while (*str != WNUL) {
        if (towupper(*str) != *src)
            break;
        str++;
        src++;
        if (*src == WNUL)
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
        if (*place != WNUL) {
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

    for (i = 0; s[i] != WNUL; i++) {
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
    int i, x = 0;
    UI_BYTES      u;
    unsigned char d[16];
    const wchar_t xb16[] = L"0123456789abcdef";

#if 1
    HCRYPTPROV    h;
    if (!CryptAcquireContext(&h, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return NULL;
    if (!CryptGenRandom(h, 16, d))
        return NULL;
    CryptReleaseContext(h, 0);
#else
    if (BCryptGenRandom(NULL, d, 16,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != STATUS_SUCCESS)
        return NULL;
#endif
    if (b == NULL)
        b = xwmalloc(TBUFSIZ - 1);
    u.hi = (WORD)GetCurrentProcessId();
    u.lo = (WORD)InterlockedIncrement(&numserial) << 8;
    for (i = 3; i > 0; i--) {
        b[x++] = xb16[u.b[i] >> 4];
        b[x++] = xb16[u.b[i] & 0x0F];
    }
    b[x++] = '-';
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
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
    LARGE_INTEGER ct;
    LARGE_INTEGER et;
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

    xsnprintf(b, MBUFSIZ, "[%.4lu] %d %-16s %s",
              GetCurrentThreadId(),
              servicemode, funcname, string);
    OutputDebugStringA(b);
}

static void dbgprintf(const char *funcname, const char *format, ...)
{
    int     n;
    char    b[MBUFSIZ];
    va_list ap;

    n = xsnprintf(b, MBUFSIZ, "[%.4lu] %d %-16s ",
                  GetCurrentThreadId(),
                  servicemode, funcname);
    va_start(ap, format);
    xvsnprintf(b + n, MBUFSIZ - n, format, ap);
    va_end(ap);

    OutputDebugStringA(b);
}

#endif

static void xiphandler(const wchar_t *e,
                       const wchar_t *w, const wchar_t *f,
                       unsigned int n, uintptr_t r)
{
    DBG_PRINTS("invalid parameter handler called");
}

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

    if (ern) {
        c = xsnwprintf(erb, TBUFSIZ, L"error(%lu) ", ern);
        xwinapierror(erb + c, MBUFSIZ - c, ern);
        errarg[i++] = CRLFW;
        errarg[i++] = erb;
        DBG_PRINTF("%S, %S", hdr, erb);
    }
    else {
        ern = ERROR_INVALID_PARAMETER;
        DBG_PRINTF("%S", hdr);
    }
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
    xfree(tp);
    ExitThread(r);
    return r;
}

static HANDLE xcreatethread(int detached, int suspended,
                            LPTHREAD_START_ROUTINE threadfn)
{
    DWORD  id;
    HANDLE th;
    LPSVCBATCH_THREAD tp;

    tp = (LPSVCBATCH_THREAD)xmmalloc(1, sizeof(SVCBATCH_THREAD));
    tp->threadfn = threadfn;
    tp->param    = suspended ? NULL : INVALID_HANDLE_VALUE;

    th = CreateThread(NULL, 0, xrunthread, tp, CREATE_SUSPENDED, &id);
    if (th == NULL) {
        DBG_PRINTS("CreateThread failed");
        xfree(tp);
        return NULL;
    }
    if (detached) {
        ResumeThread(th);
        CloseHandle(th);
        return NULL;
    }
    else {
        if (!suspended)
            ResumeThread(th);
        return th;
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

static wchar_t *getfullpathname(const wchar_t *src, int isdir)
{
    wchar_t *cp;

    ASSERT_WSTR(src, NULL);

    cp = xwcsdup(src);
    xcleanwinpath(cp, isdir);

    if (isrelativepath(cp)) {
        DWORD   nn;
        DWORD   sz = HBUFSIZ - 1;
        wchar_t bb[HBUFSIZ];

        nn = GetFullPathNameW(cp, sz, bb, NULL);
        if ((nn == 0) || (nn >= sz)) {
            xfree(cp);
            return NULL;
        }
        xfree(cp);
        cp = xwcsdup(bb);
    }

    return cp;
}

static wchar_t *getfinalpathname(const wchar_t *path, int isdir)
{
    wchar_t    *buf  = NULL;
    DWORD       siz  = _MAX_FNAME;
    DWORD       len  = 0;
    HANDLE      fh;
    DWORD       atr  = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, atr, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    while (buf == NULL) {
        buf = xwmalloc(siz);
        len = GetFinalPathNameByHandleW(fh, buf, siz, VOLUME_NAME_DOS);
        if (len == 0) {
            CloseHandle(fh);
            xfree(buf);
            return NULL;
        }
        if (len > siz) {
            xfree(buf);
            buf = NULL;
            siz = len;
        }
    }
    CloseHandle(fh);
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
        }
    }
    return buf;
}

static wchar_t *getrealpathname(const wchar_t *src, int isdir)
{
    wchar_t    *fpn;
    wchar_t    *buf;

    if (!servicemode)
        return xwcsdup(src);

    fpn = getfullpathname(src, isdir);
    if (IS_EMPTY_WCS(fpn))
        return NULL;
    buf = getfinalpathname(fpn, isdir);

    xfree(fpn);
    return buf;
}

static int resolvebatchname(const wchar_t *a)
{
    int i;

    if (svcbatchfile)
        return 0;
    svcbatchfile = getrealpathname(a, 0);
    if (IS_EMPTY_WCS(svcbatchfile))
        return 1;

    i = xwcslen(svcbatchfile);
    while (--i > 0) {
        if (svcbatchfile[i] == L'\\') {
            svcbatchfile[i] = WNUL;
            svcbatchname = svcbatchfile + i + 1;
            servicebase  = xwcsdup(svcbatchfile);
            svcbatchfile[i] = L'\\';
            return 0;
        }
    }
    xfree(svcbatchfile);
    svcbatchfile = NULL;
    servicebase  = NULL;
    svcbatchname = NULL;
    return 1;
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

static DWORD createiopipes(LPSTARTUPINFOW si, LPHANDLE iwrs, LPHANDLE ords)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE sh = NULL;
    HANDLE cp = GetCurrentProcess();

    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;
    /**
     * Create stdin pipe, with write side
     * of the pipe as non inheritable.
     */
    if (!CreatePipe(&(si->hStdInput), &sh, &sa, HBUFSIZ))
        return GetLastError();

    if (!DuplicateHandle(cp, sh, cp,
                         iwrs, FALSE, 0,
                         DUPLICATE_SAME_ACCESS))
        return GetLastError();
    SAFE_CLOSE_HANDLE(sh);

    /**
     * Create stdout/stderr pipe, with read side
     * of the pipe as non inheritable
     */
    if (!CreatePipe(&sh, &(si->hStdError), &sa, HBUFSIZ))
        return GetLastError();
    if (!DuplicateHandle(cp, sh, cp,
                         ords, FALSE, 0,
                         DUPLICATE_SAME_ACCESS))
        return GetLastError();
    si->hStdOutput = si->hStdError;

    SAFE_CLOSE_HANDLE(sh);
    return 0;
}

static int xseekfend(HANDLE h)
{
    if (!haspipedlogs) {
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (!SetFilePointerEx(h, ee, NULL, FILE_END))
            return 1;
    }
    return 0;
}

static DWORD logappend(HANDLE h, LPCVOID buf, DWORD len)
{
    DWORD wr;


    if (xseekfend(h))
        return GetLastError();

    if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0)) {
        InterlockedAdd64(&logwritten, wr);
        return 0;
    }
    else {
        return GetLastError();
    }
}

static DWORD logfflush(HANDLE h)
{
    DWORD wr;

    if (xseekfend(h))
        return GetLastError();
    if (WriteFile(h, CRLFA, 2, &wr, NULL) && (wr != 0)) {
        InterlockedAdd64(&logwritten, wr);
        if (!haspipedlogs)
            FlushFileBuffers(h);
    }
    else {
        return GetLastError();
    }
    if (xseekfend(h))
        return GetLastError();
    else
        return 0;
}

static DWORD logwrline(HANDLE h, const char *s)
{
    char    wb[TBUFSIZ];
    DWORD   wr;
    DWORD   nw;

    ASSERT_CSTR(s, 0);
    nw = xtimehdr(wb, TBUFSIZ);

    if (nw > 0) {
        if (WriteFile(h, wb, nw, &wr, NULL) && (wr != 0))
            InterlockedAdd64(&logwritten, wr);
        else
            return GetLastError();
    }
    nw = (DWORD)strlen(s);
    if (WriteFile(h, s, nw, &wr, NULL) && (wr != 0))
        InterlockedAdd64(&logwritten, wr);
    else
        return GetLastError();
    if (WriteFile(h, CRLFA, 2, &wr, NULL) && (wr != 0))
        InterlockedAdd64(&logwritten, wr);
    else
        return GetLastError();

    return 0;
}

static DWORD logprintf(HANDLE h, const char *format, ...)
{
    int     c;
    char    buf[MBUFSIZ];
    va_list ap;

    va_start(ap, format);
    c = xvsnprintf(buf, MBUFSIZ, format, ap);
    va_end(ap);
    if (c > 0)
        return logwrline(h, buf);
    else
        return 0;
}

static DWORD logwrtime(HANDLE h, const char *hdr)
{
    SYSTEMTIME tt;

    if (uselocaltime)
        GetLocalTime(&tt);
    else
        GetSystemTime(&tt);
    return logprintf(h, "%-16s : %.4d-%.2d-%.2d %.2d:%.2d:%.2d",
                     hdr, tt.wYear, tt.wMonth, tt.wDay,
                     tt.wHour, tt.wMinute, tt.wSecond);
}

static void logconfig(HANDLE h)
{

    logprintf(h, "Service name     : %S", servicename);
    logprintf(h, "Service uuid     : %S", serviceuuid);
    logprintf(h, "Batch file       : %S", svcbatchfile);
    if (svcbatchargs)
        logprintf(h, "      arguments  : %S", svcbatchargs);
    if (svcstopwargc)
        logprintf(h, "Shutdown batch   : %S", svcstopwargv[0]);
    logprintf(h, "Program directory: %S", exelocation);
    logprintf(h, "Base directory   : %S", servicebase);
    logprintf(h, "Home directory   : %S", servicehome);
    logprintf(h, "Logs directory   : %S", servicelogs);
    if (haspipedlogs)
        logprintf(h, "Log redirected to: %S", logredirargv[0]);

    logfflush(h);
}

static DWORD createlogsdir(void)
{
    DWORD   rc;
    wchar_t *dp;

    dp = getfullpathname(outdirparam, 1);
    if (dp == NULL) {
        xsyserror(0, L"getfullpathname", outdirparam);
        return ERROR_BAD_PATHNAME;
    }
    servicelogs = getfinalpathname(dp, 1);
    if (servicelogs == NULL) {
        rc = xcreatepath(dp);
        if (rc != 0)
            return xsyserror(rc, L"xcreatepath", dp);
        servicelogs = getfinalpathname(dp, 1);
        if (servicelogs == NULL)
            return xsyserror(ERROR_PATH_NOT_FOUND,
                             L"getfinalpathname", dp);
    }
    if (_wcsicmp(servicelogs, servicehome) == 0) {
        xsyserror(0, L"Logs directory cannot be the same as home directory",
                  servicelogs);
        return ERROR_BAD_PATHNAME;
    }
    xfree(dp);
    return 0;
}

static DWORD WINAPI rdpipedlog(void *unused)
{
    DWORD rc = 0;
#if defined(_DEBUG)
    DWORD rs = 1;
    DWORD rn = 0;
    DWORD rm = SBUFSIZ - 32;
    char  rb[2];
    char  rl[SBUFSIZ];

    DBG_PRINTS("started");
#else
    DWORD rs = SBUFSIZ;
    char  rb[SBUFSIZ];
#endif

    while (rc == 0) {
        DWORD rd = 0;

        if (ReadFile(pipedprocout, rb, rs, &rd, NULL) && (rd != 0)) {
#if defined(_DEBUG)
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
#endif
        }
        else {
            rc = GetLastError();
        }
    }
#if defined(_DEBUG)
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
#endif
    return 0;
}

static DWORD openlogpipe(BOOL ssp)
{
    DWORD  rc;
    DWORD  cf = CREATE_NEW_CONSOLE;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;
    HANDLE wr = NULL;
    wchar_t *cmdline = NULL;

    xmemzero(&cp, 1, sizeof(PROCESS_INFORMATION));
    xmemzero(&si, 0, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    rc = createiopipes(&si, &wr, &pipedprocout);
    if (rc)
        return rc;

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
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | cf,
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
    xcreatethread(1, 0, rdpipedlog);

    if (haslogstatus) {
        logwrline(wr, cnamestamp);
    }
    InterlockedExchangePointer(&logfhandle, wr);
    DBG_PRINTF("running pipe log process %lu", pipedprocpid);
    xfree(cmdline);

    return 0;
failed:
    xfree(cmdline);
    xwaafree(logredirargv);
    SAFE_CLOSE_HANDLE(wr);
    SAFE_CLOSE_HANDLE(pipedprocout);
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
    if (wcsftime(ewb, BBUFSIZ, svclogfname, ctm) == 0)
        return xsyserror(0, L"invalid format code", svclogfname);
    xfree(logfilename);

    for (i = 0, x = 0; ewb[i] != WNUL; i++) {
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
        return svcsyserror(__FUNCTION__, __LINE__,
                           ERROR_FILE_NOT_FOUND, L"logfilename", NULL);

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
    LeaveCriticalSection(&logfilelock);
    return rc;
}

static void closelogfile(void)
{
    HANDLE h;

    EnterCriticalSection(&logfilelock);
    h = InterlockedExchangePointer(&logfhandle, NULL);
    if (h) {
        DBG_PRINTS("closing");
        if (haslogstatus) {
            logfflush(h);
            logwrtime(h, "Log closed");
        }
        FlushFileBuffers(h);
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
        {
            DWORD rv;

            if (GetExitCodeProcess(pipedprocess, &rv)) {
                DBG_PRINTF("log process returned %lu", rv);
            }
        }
#endif
        SAFE_CLOSE_HANDLE(pipedprocout);
        SAFE_CLOSE_HANDLE(pipedprocess);
    }
    DBG_PRINTS("closed");
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
        int      siz;
        LONGLONG len;
        LONGLONG mux = CPP_INT64_C(0);

        siz = xwcstoi(rp, &ep);
        if (siz < 1)
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
        len = siz * mux;
        if (len < KILOBYTES(1)) {
            DBG_PRINTF("rotate size %S is less then 1K", rp);
            rotatesiz.QuadPart = CPP_INT64_C(0);
            rotatebysize = FALSE;
        }
        else {
            DBG_PRINTF("rotate if larger then %S", rp);
            rotatesiz.QuadPart = len;
            rotatebysize = TRUE;
        }
    }
    else {
        rotateint    = 0;
        rotatebytime = FALSE;
        rotatetmo.QuadPart = 0;

        if (wcschr(rp, L':') == NULL) {
            int mm;

            mm = xwcstoi(rp, &ep);
            if (*ep) {
                DBG_PRINTF("invalid rotate timeout %S", rp);
                return FALSE;
            }
            else if (mm < 0) {
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
        else {
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
    }
    return TRUE;
}

static DWORD runshutdown(DWORD rt)
{
    wchar_t  rp[TBUFSIZ];
    wchar_t *cmdline = NULL;
    HANDLE   wh[2];
    DWORD    rc = 0;
    DWORD    cf = CREATE_NEW_CONSOLE;
    int   i, ip = 0;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;

    DBG_PRINTS("started");

    xmemzero(&cp, 1, sizeof(PROCESS_INFORMATION));
    xmemzero(&si, 1, sizeof(STARTUPINFOW));
    si.cb = DSIZEOF(STARTUPINFOW);

    cmdline = xappendarg(1, NULL,    NULL, svcbatchexe);
    cmdline = xappendarg(0, cmdline, NULL, L"::");
    rp[ip++] = L'-';
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
    if (ip > 2)
        cmdline = xappendarg(0, cmdline, NULL,  rp);
    cmdline = xappendarg(0, cmdline, L"-c", localeparam);
    if (havelogging && svcendlogfn) {
        cmdline = xappendarg(1, cmdline, L"-o", servicelogs);
        cmdline = xappendarg(1, cmdline, L"-n", svcendlogfn);
    }
    for (i = 0; i < svcstopwargc; i++)
        cmdline = xappendarg(1, cmdline, NULL, svcstopwargv[i]);

    DBG_PRINTF("cmdline %S", cmdline);

    if (!CreateProcessW(svcbatchexe, cmdline, NULL, NULL, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | cf,
                        NULL, NULL, &si, &cp)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", NULL);
        goto finished;
    }

    wh[0] = cp.hProcess;
    wh[1] = processended;
    ResumeThread(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hThread);

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
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hProcess);

    DBG_PRINTS("done");
    return rc;
}

static DWORD WINAPI stopthread(void *param)
{

#if defined(_DEBUG)
    if (servicemode)
        DBG_PRINTS("service stop");
    else
        DBG_PRINTS("shutdown stop");
#endif

    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
    if (svcstopwargc && param) {
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
    if (rv) {
        setsvcstatusexit(rv);
    }
    if (InterlockedIncrement(&sstarted) == 1) {
        ResetEvent(svcstopended);
        xcreatethread(1, rv, stopthread);
    }
    else {
        InterlockedDecrement(&sstarted);
        DBG_PRINTS("already started");
    }
}

static DWORD WINAPI rdpipethread(void *unused)
{
    DWORD rc = 0;
    BYTE  rb[HBUFSIZ];

    DBG_PRINTS("started");
    while (rc == 0) {
        DWORD rd = 0;

        if (ReadFile(outputpiperd, rb, HBUFSIZ, &rd, NULL) && (rd != 0)) {
            EnterCriticalSection(&logfilelock);
            if (havelogging) {
                HANDLE h = InterlockedExchangePointer(&logfhandle, NULL);

                if (h)
                    rc = logappend(h, rb, rd);
                else
                    rc = ERROR_NO_MORE_FILES;

                InterlockedExchangePointer(&logfhandle, h);
                if ((rc == 0) && rotatebysize) {
                    if (InterlockedCompareExchange64(&logwritten, 0, 0) >= rotatesiz.QuadPart) {
                        InterlockedExchange64(&logwritten, 0);
                        InterlockedExchange(&rotatesig, 1);
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
        }
        else {
            rc = GetLastError();
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
    DBG_PRINTS("done");
#endif
    return 0;
}

static DWORD WINAPI wrpipethread(void *unused)
{
    DWORD wr;

    DBG_PRINTS("started");
    if (WriteFile(inputpipewrs, YYES, 3, &wr, NULL) && (wr != 0)) {
        FlushFileBuffers(inputpipewrs);
    }
#if defined(_DEBUG)
    else {
        DWORD rc = GetLastError();
        if (rc == ERROR_BROKEN_PIPE)
            DBG_PRINTS("pipe closed");
        else
            DBG_PRINTF("err=%lu", rc);
    }
    DBG_PRINTS("done");
#endif
    return 0;
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
                    DBG_PRINTF("service %s signaled", xwcsiid(II_SERVICE, cc));
                    if (haslogstatus) {
                        HANDLE h;

                        EnterCriticalSection(&logfilelock);
                        h = InterlockedExchangePointer(&logfhandle, NULL);

                        if (h) {
                            logfflush(h);
                            logprintf(h, "Signaled %s", xwcsiid(II_SERVICE, cc));
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
                if (InterlockedExchange(&rotatesig, 0) == 1)
                    DBG_PRINTS("rotate by size");
                else
                    DBG_PRINTS("rotate by signal");
                rc = rotatelogs();
                if (rc == 0) {
                    if (IS_VALID_HANDLE(wt) && (rotateint < 0)) {
                        CancelWaitableTimer(wt);
                        SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE);
                    }
                    ResetEvent(logrotatesig);
                }
                else {
                    xsyserror(rc, L"rotatelogs", NULL);
                    createstopthread(rc);
                }
            break;
            case WAIT_OBJECT_2:
                DBG_PRINTS("rotate by time");
                rc = rotatelogs();
                if (rc == 0) {
                    CancelWaitableTimer(wt);
                    if (rotateint > 0)
                        rotatetmo.QuadPart += rotateint;
                    SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE);
                    ResetEvent(logrotatesig);
                }
                else {
                    xsyserror(rc, L"rotatelogs", NULL);
                    createstopthread(rc);
                }
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
    wchar_t *cmdline;
    HANDLE   wh[4];
    DWORD    rc = 0;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;

    DBG_PRINTS("started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    cmdline = xappendarg(1, NULL,    NULL,     comspec);
    cmdline = xappendarg(1, cmdline, L"/D /C", svcbatchfile);
    cmdline = xappendarg(0, cmdline, NULL,     svcbatchargs);

    DBG_PRINTF("cmdline %S", cmdline);
    xmemzero(&cp, 1, sizeof(PROCESS_INFORMATION));
    xmemzero(&si, 1, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    rc = createiopipes(&si, &inputpipewrs, &outputpiperd);
    if (rc != 0)
        goto finished;

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
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

    wh[0] = childprocess;
    wh[1] = xcreatethread(0, 1, rdpipethread);
    if (IS_INVALID_HANDLE(wh[1])) {
        goto finished;
    }
    wh[2] = xcreatethread(0, 1, wrpipethread);
    if (IS_INVALID_HANDLE(wh[2])) {
        goto finished;
    }

    ResumeThread(cp.hThread);
    ResumeThread(wh[1]);
    ResumeThread(wh[2]);
    SAFE_CLOSE_HANDLE(cp.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    DBG_PRINTF("running child with pid %lu", childprocpid);
    if (haslogrotate) {
        xcreatethread(1, 0, rotatethread);
    }
    WaitForMultipleObjects(3, wh, TRUE, INFINITE);
    CloseHandle(wh[1]);
    CloseHandle(wh[2]);

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
            InterlockedIncrement(&sstarted);
            DBG_PRINTS(xwcsiid(II_SERVICE, ctrl));
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
            if (haslogstatus) {
                HANDLE h;
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h) {
                    logfflush(h);
                    logprintf(h, "Service signaled : %s",  xwcsiid(II_SERVICE, ctrl));
                }
                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
            }
            InterlockedDecrement(&sstarted);
            createstopthread(0);
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
                DBG_PRINTS("signaling SVCBATCH_CTRL_ROTATE");
                InterlockedExchange(&rotatesig, 0);
                SetEvent(logrotatesig);
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
            DBG_PRINTF("unknown control %lu", ctrl);
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

    wh[1] = xcreatethread(0, 0, monitorthread);
    if (IS_INVALID_HANDLE(wh[1])) {
        goto finished;
    }
    wh[0] = xcreatethread(0, 0, workerthread);
    if (IS_INVALID_HANDLE(wh[0])) {
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

    SAFE_CLOSE_HANDLE(inputpipewrs);
    SAFE_CLOSE_HANDLE(outputpiperd);
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
    DWORD    sm = FBUFSIZ;
    DWORD    sz;
    DWORD    nn;

    sz = sm - 1;
    bb = xwmalloc(sz);
    nn = GetModuleFileNameW(NULL, bb, sz);
    if (nn == 0)
        return GetLastError();
    while (nn >= sz) {
        sm = sm * 2;
        sz = sm - 1;
        xfree(bb);
        bb = xwmalloc(sz);
        nn = GetModuleFileNameW(NULL, bb, sz);
        if (nn == 0)
            return GetLastError();
    }
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
    QueryPerformanceFrequency(&pcfrequency);
    QueryPerformanceCounter(&pcstarttime);

    return 0;
}

int wmain(int argc, const wchar_t **wargv)
{
    int         i;
    int         opt;
    int         ncnt  = 0;
    int         rcnt  = 0;
    int         rv    = 0;
    wchar_t     bb[BBUFSIZ] = { L'-', WNUL, WNUL, WNUL };

    HANDLE      h;
    SERVICE_TABLE_ENTRYW se[2];
    const wchar_t *maxlogsparam = NULL;
    const wchar_t *batchparam   = NULL;
    const wchar_t *svchomeparam = NULL;
    const wchar_t *rparam[2];
    wchar_t       *nparam[2] = { NULL, NULL };

#if defined(_MSC_VER) && (_MSC_VER < 1900)
    /* Not supported */
#else
    _set_invalid_parameter_handler(xiphandler);
#endif
#if defined(_DEBUG)
   _CrtSetReportMode(_CRT_ASSERT, 0);
#endif
    rv = xwmaininit();
    if (rv)
        return rv;
    /**
     * Make sure children (cmd.exe) are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);

    h = GetStdHandle(STD_INPUT_HANDLE);
    if (IS_INVALID_HANDLE(h)) {
       if (AllocConsole()) {
            /**
             * AllocConsole should create new set of
             * standard i/o handles
             */
            atexit(cconsolecleanup);
            h = GetStdHandle(STD_INPUT_HANDLE);
            if (IS_INVALID_HANDLE(h))
                return ERROR_DEV_NOT_EXIST;
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

    if (argc == 1) {
        fprintf(stdout, "%s\n\n", cnamestamp);
        fprintf(stdout, "Visit " SVCBATCH_PROJECT_URL " for more details\n");

        return 0;
    }

    wnamestamp = xcwiden(cnamestamp);
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
                if (logredirargv == NULL)
                    logredirargv = xwaalloc(SVCBATCH_MAX_ARGS);
                if (logredirargc < SVCBATCH_MAX_ARGS)
                    logredirargv[logredirargc++] = xwcsdup(xwoptarg);
                else
                    return xsyserror(0, L"Too many -e arguments", xwoptarg);
            break;
            case L's':
                if (svcstopwargv == NULL)
                    svcstopwargv = xwaalloc(SVCBATCH_MAX_ARGS);
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
    if (localeparam) {
        if (_wsetlocale(LC_ALL, localeparam) == NULL)
            return xsyserror(0, L"Invalid -c command option value", localeparam);
    }
    argc  -= xwoptind;
    wargv += xwoptind;
    if (argc > 0) {
        batchparam = wargv[0];
        for (i = 1; i < argc; i++) {
            /**
             * Add arguments for batch file
             */
            svcbatchargs = xappendarg(1, svcbatchargs,  NULL, wargv[i]);
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
        if (rcnt)
            xwcslcat(bb, TBUFSIZ, L"-r ");
        if (ncnt)
            xwcslcat(bb, TBUFSIZ, L"-n ");
        if (outdirparam)
            xwcslcat(bb, TBUFSIZ, L"-o ");
        if (logredirargc)
            xwcslcat(bb, TBUFSIZ, L"-e ");
        if (truncatelogs)
            xwcslcat(bb, TBUFSIZ, L"-t ");
        if (haslogstatus)
            xwcslcat(bb, TBUFSIZ, L"-v ");
        if (maxlogsparam)
            xwcslcat(bb, TBUFSIZ, L"-m ");
        if (bb[0])
            return xsyserror(0, L"Option -q is mutually exclusive with option(s)", bb);
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
                if (resolvebatchname(batchparam))
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
            if (resolvebatchname(batchparam))
                return xsyserror(ERROR_FILE_NOT_FOUND, batchparam, NULL);
         }
    }
    else {
        servicehome = xgetenv(L"SVCBATCH_SERVICE_HOME");
        if (servicehome == NULL)
            return xsyserror(GetLastError(), L"SVCBATCH_SERVICE_HOME", NULL);
    }
    if (resolvebatchname(batchparam))
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
            if (_wcsicmp(nparam[0], L"NUL") == 0)
                return xsyserror(0, L"Invalid log filename", nparam[0]);
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
    return rv;
}
