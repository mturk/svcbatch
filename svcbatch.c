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
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <process.h>
#include <errno.h>
#include <share.h>
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


static volatile LONG         monitorsig  = 0;
static volatile LONG         rotatesig   = 0;
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
static LONGLONG              rotateint   = 0;
static LARGE_INTEGER         rotatetmo   = {{ 0, 0 }};
static LARGE_INTEGER         rotatesiz   = {{ 0, 0 }};
static LARGE_INTEGER         pcfrequency;
static LARGE_INTEGER         pcstarttime;

static wchar_t  *comspec          = NULL;
static wchar_t **dupwenvp         = NULL;
static int       dupwenvc         = 0;
static wchar_t  *wenvblock        = NULL;

static BOOL      haslogstatus     = FALSE;
static BOOL      hasctrlbreak     = FALSE;
static BOOL      haslogrotate     = FALSE;
static BOOL      haspipedlogs     = FALSE;
static BOOL      rotatebysize     = FALSE;
static BOOL      rotatebytime     = FALSE;
static BOOL      uselocaltime     = FALSE;
static BOOL      truncatelogs     = FALSE;
static BOOL      havelogging      = TRUE;
static BOOL      servicemode      = TRUE;

static DWORD     preshutdown      = 0;
static DWORD     childprocpid     = 0;
static DWORD     pipedprocpid     = 0;

static int       svcmaxlogs       = SVCBATCH_MAX_LOGS;
static int       xwoptind         = 1;
static int       xwoption         = 0;
static int       logredirargc     = 0;

#if defined(_DEBUG)
static BOOL      consolemode      = FALSE;
static FILE     *dbgoutstream     = NULL;
#endif

static wchar_t  *svcbatchexe      = NULL;
static wchar_t  *svcbatchfile     = NULL;
static wchar_t  *svcbatchname     = NULL;
static wchar_t  *shutdownfile     = NULL;
static wchar_t  *exelocation      = NULL;
static wchar_t  *servicebase      = NULL;
static wchar_t  *servicename      = NULL;
static wchar_t  *servicehome      = NULL;
static wchar_t  *serviceuuid      = NULL;
static wchar_t  *svcbatchargs     = NULL;
static wchar_t  *svcendargs       = NULL;

static wchar_t  *servicelogs      = NULL;
static wchar_t  *logfilename      = NULL;
static wchar_t  *logredirect      = NULL;
static wchar_t **logredirargv     = NULL;
static wchar_t  *wnamestamp       = NULL;
static wchar_t  *svclogfname      = NULL;

static HANDLE    childprocjob     = NULL;
static HANDLE    childprocess     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    ssignalevent     = NULL;
static HANDLE    processended     = NULL;
static HANDLE    monitorevent     = NULL;
static HANDLE    logrotatesig     = NULL;
static HANDLE    outputpiperd     = NULL;
static HANDLE    inputpipewrs     = NULL;
static HANDLE    pipedprocjob     = NULL;
static HANDLE    pipedprocess     = NULL;
static HANDLE    pipedprocout     = NULL;

static wchar_t      zerostring[4] = { WNUL,  WNUL,  WNUL, WNUL };
static wchar_t      CRLFW[4]      = { L'\r', L'\n', WNUL, WNUL };
static char         CRLFA[4]      = { '\r', '\n', '\0', '\0' };
static char         YYES[4]       = { 'Y',  '\r', '\n', '\0' };

static const char    *cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;
static const wchar_t *cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);
static const wchar_t *outdirparam = SVCBATCH_LOGSDIR;
static const wchar_t *svcendlogfn = SHUTDOWN_LOGNAME;
static const wchar_t *xwoptarg    = NULL;

static const wchar_t *xwcsiid(int i, DWORD c)
{
    const wchar_t *r;

    switch (i) {
        case II_CONSOLE:
            {
                switch (c) {
                    case CTRL_CLOSE_EVENT:
                        r = L"CTRL_CLOSE_EVENT";
                    break;
                    case CTRL_SHUTDOWN_EVENT:
                        r = L"CTRL_SHUTDOWN_EVENT";
                    break;
                    case CTRL_C_EVENT:
                        r = L"CTRL_C_EVENT";
                    break;
                    case CTRL_BREAK_EVENT:
                        r = L"CTRL_BREAK_EVENT";
                    break;
                    case CTRL_LOGOFF_EVENT:
                        r = L"CTRL_LOGOFF_EVENT";
                    break;
                    default:
                        r = L"CTRL_UNKNOWN";
                    break;
                }
            }
        break;
        case II_SERVICE:
            {
                switch (c) {
                    case SERVICE_CONTROL_PRESHUTDOWN:
                        r = L"SERVICE_CONTROL_PRESHUTDOWN";
                    break;
                    case SERVICE_CONTROL_SHUTDOWN:
                        r = L"SERVICE_CONTROL_SHUTDOWN";
                    break;
                    case SERVICE_CONTROL_STOP:
                        r = L"SERVICE_CONTROL_STOP";
                    break;
                    case SERVICE_CONTROL_INTERROGATE:
                        r = L"SERVICE_CONTROL_INTERROGATE";
                    break;
                    case SVCBATCH_CTRL_BREAK:
                        r = L"SVCBATCH_CTRL_BREAK";
                    break;
                    case SVCBATCH_CTRL_ROTATE:
                        r = L"SVCBATCH_CTRL_ROTATE";
                    break;
                    default:
                        r = L"SERVICE_CONTROL_UNKNOWN";
                    break;
                }
            }
        break;
        default:
            r = L"UNKNOWN";
        break;
    }

    return r;
}

static wchar_t *xwmalloc(size_t size)
{
    wchar_t *p = (wchar_t *)malloc((size + 2) * sizeof(wchar_t));
    if (p == NULL) {
        _wperror(L"xwmalloc");
        _exit(1);
    }
    p[size++] = WNUL;
    p[size]   = WNUL;
    return p;
}

static wchar_t *xwcalloc(size_t size)
{
    wchar_t *p = (wchar_t *)calloc(size + 2, sizeof(wchar_t));
    if (p == NULL) {
        _wperror(L"xwcalloc");
        _exit(1);
    }
    return p;
}

static void *xcalloc(size_t number, size_t size)
{
    void *p = calloc(number + 2, size);
    if (p == NULL) {
        _wperror(L"xcalloc");
        _exit(1);
    }
    return p;
}

static void xfree(void *m)
{
    if (m != NULL)
        free(m);
}

static wchar_t **xwaalloc(size_t size)
{
    wchar_t **p = (wchar_t **)calloc(size + 2, sizeof(wchar_t *));
    if (p == NULL) {
        _wperror(L"xwaalloc");
        _exit(1);
    }
    return p;
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
    for (i = 0; i < n; i++) {
        p[i] = (wchar_t)(s[i]);
    }
    return p;
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
static size_t xwcslcat(wchar_t *dst, size_t siz, const wchar_t *src)
{
    const wchar_t *s = src;
    wchar_t *p;
    wchar_t *d = dst;
    size_t   n = siz;
    size_t   c;
    size_t   r;

    if (IS_EMPTY_WCS(s))
        return 0;
    while ((n-- != 0) && (*d != WNUL))
        d++;
    c = d - dst;
    n = siz - c;

    if (n == 0)
        return (c + wcslen(s));
    p = dst + c;
    while (*s != WNUL) {
        if (n != 1) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    r = c + (s - src);
    if (r >= siz)
        *p = WNUL;
    else
        *d = WNUL;

    return r;
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
        if (wcspbrk(s3, L" \n\r\t\"")) {
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
        oli = wcschr(opts, towlower((wchar_t)xwoption));
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

static wchar_t *xenvblock(int cnt, const wchar_t **arr)
{
    int      i;
    int      blen = 0;
    int     *s;
    wchar_t *e;
    wchar_t *b;

    s = (int *)xcalloc(cnt, sizeof(int));
    for (i = 0; i < cnt; i++) {
        int n = xwcslen(arr[i]);
        s[i]  = n++;
        blen += n;
    }

    b = xwcalloc(blen);
    e = b;
    for (i = 0; i < cnt; i++) {
        int n = s[i];
        wmemcpy(e, arr[i], n++);
        e += n;
    }
    xfree(s);
    return b;
}

static int xenvsort(const void *arg1, const void *arg2)
{
    return _wcsicoll(*((wchar_t **)arg1), *((wchar_t **)arg2));
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

static wchar_t *xuuidstring(void)
{
    int i, x;
    wchar_t      *b;
    HCRYPTPROV    h;
    unsigned char d[16];
    const wchar_t xb16[] = L"0123456789abcdef";

    if (!CryptAcquireContext(&h, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return NULL;
    if (!CryptGenRandom(h, 16, d))
        return NULL;
    CryptReleaseContext(h, 0);
    b = xwmalloc(38);
    for (i = 0, x = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            b[x++] = '-';
        b[x++] = xb16[d[i] >> 4];
        b[x++] = xb16[d[i] & 0x0F];
    }
    b[x] = WNUL;
    return b;
}

static void xmktimedstr(wchar_t *buf, int siz, const wchar_t *pfx)
{
    SYSTEMTIME st;
    int bsz = siz - 1;

    if (uselocaltime)
        GetLocalTime(&st);
    else
        GetSystemTime(&st);
    _snwprintf(buf, bsz, L"%s%.4d%.2d%.2d%.2d%.2d%.2d",
               pfx, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    buf[bsz] = WNUL;
}

static int xisbatchfile(const wchar_t *s)
{
    int n = xwcslen(s);
    /* a.bat */
    if (n > 4) {
        const wchar_t *e = s + n - 4;

        if (*(e++) == L'.') {
            if (_wcsicmp(e, L"bat") == 0)
                return 1;
            if (_wcsicmp(e, L"cmd") == 0)
                return 1;
        }
    }
    return 0;
}

static int xtimehdr(char *wb, int sz)
{
    LARGE_INTEGER ct;
    LARGE_INTEGER et;
    DWORD   ss, us, mm, hh;
    int     nc;

    sz--;
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

    nc = _snprintf(wb, sz, "[%.2lu:%.2lu:%.2lu.%.6lu] ",
                   hh, mm, ss, us);
    wb[sz] = '\0';
    if (nc < 0)
        nc = sz;
    return nc;
}

#if defined(_DEBUG)
/**
 * Runtime debugging functions
 */
static void dbgprints(const char *funcname, const char *string)
{
    int  c = MBUFSIZ - 1;
    char b[MBUFSIZ];

    _snprintf(b, c, "[%.4lu] %d %-16s %s",
              GetCurrentThreadId(),
              servicemode, funcname, string);
     b[c] = '\0';
     if (dbgoutstream) {
         char tb[TBUFSIZ];

         xtimehdr(tb, TBUFSIZ);
         fprintf(dbgoutstream, "%s[%.4lu] %s\n", tb, GetCurrentProcessId(), b);
         fflush(dbgoutstream);
     }
     else {
        OutputDebugStringA(b);
     }
}

static void dbgprintf(const char *funcname, const char *format, ...)
{
    int     n;
    int     c = MBUFSIZ - 1;
    char    b[MBUFSIZ];
    va_list ap;

    n = _snprintf(b, c, "[%.4lu] %d %-16s ",
                  GetCurrentThreadId(),
                  servicemode, funcname);
    b[c] = '\0';
    va_start(ap, format);
    _vsnprintf(b + n, c - n, format, ap);
    va_end(ap);
    b[c] = '\0';

    if (dbgoutstream) {
        char tb[TBUFSIZ];

        xtimehdr(tb, TBUFSIZ);
        fprintf(dbgoutstream, "%s[%.4lu] %s\n", tb, GetCurrentProcessId(), b);
        fflush(dbgoutstream);

    }
    else {
        OutputDebugStringA(b);
    }
}

# if (_DEBUG > 1)
static FILE *xmkdbgtemp(void)
{
    FILE *ds = NULL;
    DWORD rc;
    wchar_t bb[BBUFSIZ];
    wchar_t rb[TBUFSIZ];

    rc = GetEnvironmentVariableW(L"SVCBATCH_SERVICE_DDBG", bb, _MAX_FNAME);
    if (rc != 0) {
        xwcslcat(bb, BBUFSIZ, SHUTDOWN_LOGFEXT);
        goto doopen;
    }
    rc = GetEnvironmentVariableW(L"TEMP", bb, _MAX_FNAME);
    if ((rc == 0) || (rc >= _MAX_FNAME)) {
        rc = GetEnvironmentVariableW(L"TMP", bb, _MAX_FNAME);
        if ((rc == 0) || (rc >= _MAX_FNAME)) {
            OutputDebugStringW(L">>> SvcBatch");
            OutputDebugStringW(L"    Missing TEMP and TMP environment variables");
            OutputDebugStringW(L"<<< SvcBatch");
            return NULL;
        }
    }
    xmktimedstr(rb, TBUFSIZ, L"\\" SVCBATCH_DBGNAME);
    xwcslcat(bb, BBUFSIZ, rb);
    SetEnvironmentVariableW(L"SVCBATCH_SERVICE_DDBG", bb);
    xwcslcat(bb, BBUFSIZ, SVCBATCH_LOGFEXT);

doopen:
    ds = _wfsopen(bb, L"wtc", _SH_DENYWR);
    if (ds == NULL) {
        OutputDebugStringW(L">>> SvcBatch cannot create debug file");
        OutputDebugStringW(bb);
        OutputDebugStringW(L"<<< SvcBatch");
    }
    return ds;
}
# endif
#endif

static void xwinapierror(wchar_t *buf, int bufsize, DWORD statcode)
{
    int c = bufsize - 1;
    int n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL,
                           statcode,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           buf,
                           c,
                           NULL);
    if (n) {
        do {
            buf[n--] = WNUL;
        } while ((n > 0) && ((buf[n] == L'.') || (buf[n] <= L' ')));
        while (n-- > 0) {
            if (iswspace(buf[n]))
                buf[n] = L' ';
        }
    }
    else {
        _snwprintf(buf, c,
                   L"Unknown Windows error code (%lu)", statcode);
    }
    buf[c] = WNUL;
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
    int            n = MBUFSIZ - 1;
    int            c, i = 0;

    errarg[i++] = wnamestamp;
    if (servicename)
        errarg[i++] = servicename;
    errarg[i++] = CRLFW;
    errarg[i++] = L"reported the following error:\r\n";

    _snwprintf(hdr, n, L"svcbatch.c(%.4d, %S) %s", line, fn, err);
    hdr[n] = WNUL;
    if (eds) {
        xwcslcat(hdr, MBUFSIZ, L": ");
        xwcslcat(hdr, MBUFSIZ, eds);
    }
    errarg[i++] = hdr;

    if (ern) {
        c = _snwprintf(erb, 32, L"error(%lu) ", ern);
        if (c < 0)
            c = 0;
        xwinapierror(erb + c, n - c, ern);
        erb[n] = WNUL;
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
#if defined(_DEBUG)
    if (consolemode) {
        return ern;
    }
#endif
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

static DWORD xcreatedir(const wchar_t *path)
{
    DWORD rc = 0;

    if (!CreateDirectoryW(path, NULL)) {
        rc = GetLastError();
        if (rc == ERROR_ALREADY_EXISTS)
            rc = 0;
    }
    return rc;
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

static HANDLE xcreatethread(int detach, unsigned initflag,
                            unsigned int (__stdcall *threadfn)(void *),
                            void *param)
{
    unsigned u;
    HANDLE   h;

    h = (HANDLE)_beginthreadex(NULL, 0, threadfn, param, initflag, &u);

    if (IS_INVALID_HANDLE(h))
        return NULL;
    if (detach) {
        CloseHandle(h);
        h = NULL;
    }
    return h;
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

    if (IS_EMPTY_WCS(src))
        return NULL;
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
            svcsyserror(__FUNCTION__, __LINE__, 0,
                        L"service stopped without SERVICE_CONTROL_STOP signal", NULL);
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
#if defined(_DEBUG)
    if (consolemode)
        goto finished;
#endif
    if (!SetServiceStatus(hsvcstatus, &ssvcstatus)) {
        svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"SetServiceStatus", NULL);
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
    if (!CreatePipe(&(si->hStdInput), &sh, &sa, 0))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreatePipe", NULL);
    if (!DuplicateHandle(cp, sh, cp,
                         iwrs, FALSE, 0,
                         DUPLICATE_SAME_ACCESS))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"DuplicateHandle", NULL);
    SAFE_CLOSE_HANDLE(sh);

    /**
     * Create stdout/stderr pipe, with read side
     * of the pipe as non inheritable
     */
    if (!CreatePipe(&sh, &(si->hStdError), &sa, 0))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreatePipe", NULL);
    if (!DuplicateHandle(cp, sh, cp,
                         ords, FALSE, 0,
                         DUPLICATE_SAME_ACCESS))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"DuplicateHandle", NULL);
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

    if ((s == NULL) || (*s == '\0'))
        return 0;
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
    c = _vsnprintf(buf, MBUFSIZ - 1, format, ap);
    va_end(ap);
    if (c > 0) {
        buf[c] = '\0';
        return logwrline(h, buf);
    }
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
    if (shutdownfile) {
        logprintf(h, "Shutdown batch   : %S", shutdownfile);
        if (svcendargs)
            logprintf(h, "      arguments  : %S", svcendargs);
    }
    logprintf(h, "Program directory: %S", exelocation);
    logprintf(h, "Base directory   : %S", servicebase);
    logprintf(h, "Home directory   : %S", servicehome);
    logprintf(h, "Logs directory   : %S", servicelogs);
    if (haspipedlogs)
        logprintf(h, "Log redirected to: %S", logredirect);

    logfflush(h);
}

static DWORD createlogsdir(void)
{
    DWORD   rc;
    wchar_t *dp;

    dp = getfullpathname(outdirparam, 1);
    if (dp == NULL) {
        svcsyserror(__FUNCTION__, __LINE__, 0,
                    L"getfullpathname", outdirparam);
        return ERROR_BAD_PATHNAME;
    }
    servicelogs = getfinalpathname(dp, 1);
    if (servicelogs == NULL) {
        rc = xcreatepath(dp);
        if (rc != 0)
            return svcsyserror(__FUNCTION__, __LINE__, rc, L"xcreatepath", dp);
        servicelogs = getfinalpathname(dp, 1);
        if (servicelogs == NULL)
            return svcsyserror(__FUNCTION__, __LINE__, ERROR_PATH_NOT_FOUND,
                               L"getfinalpathname", dp);
    }
    if (_wcsicmp(servicelogs, servicehome) == 0) {
        svcsyserror(__FUNCTION__, __LINE__, 0,
                    L"Logs directory cannot be the same as home directory",
                    servicelogs);
        return ERROR_BAD_PATHNAME;
    }
    xfree(dp);
    return 0;
}

static unsigned int __stdcall rdpipedlog(void *unused)
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
    XENDTHREAD(0);
}

static DWORD openlogpipe(BOOL ssp)
{
    DWORD  rc;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;
    HANDLE wr = NULL;
    wchar_t *cmdline = NULL;

    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    rc = createiopipes(&si, &wr, &pipedprocout);
    if (rc)
        return rc;

    pipedprocjob = CreateJobObject(&sazero, NULL);
    if (IS_INVALID_HANDLE(pipedprocjob)) {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateJobObject", NULL);
        goto failed;
    }

    ji.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(pipedprocjob,
                                 JobObjectExtendedLimitInformation,
                                &ji,
                                 DSIZEOF(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))) {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"SetInformationJobObject", NULL);
        goto failed;
    }
    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    cmdline = xappendarg(1, NULL,    NULL, logredirect);
    if (logredirargc == 1) {
        cmdline = xappendarg(1, cmdline, NULL, svclogfname);
    }
    else {
        int i;
        for (i = 1; i < logredirargc; i++) {
            if (wcscmp(logredirargv[i], L"@@logfile@@") == 0)
                cmdline = xappendarg(1, cmdline, NULL, svclogfname);
            else
                cmdline = xappendarg(1, cmdline, NULL, logredirargv[i]);
        }
    }
    LocalFree(logredirargv);
    DBG_PRINTF("cmdline %S", cmdline);
    if (!CreateProcessW(logredirect, cmdline, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
                        wenvblock,
                        servicelogs,
                       &si, &cp)) {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateProcess", logredirect);
        goto failed;
    }
    pipedprocess = cp.hProcess;
    pipedprocpid = cp.dwProcessId;
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    if (!AssignProcessToJobObject(pipedprocjob, pipedprocess)) {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"AssignProcessToJobObject", NULL);
        TerminateProcess(pipedprocess, rc);
        goto failed;
    }
    ResumeThread(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hThread);
    xcreatethread(1, 0, &rdpipedlog, NULL);

    if (haslogstatus) {
        logwrline(wr, cnamestamp);
    }
    InterlockedExchangePointer(&logfhandle, wr);
    DBG_PRINTF("running pipe log process %lu", pipedprocpid);
    xfree(cmdline);

    return 0;
failed:
    xfree(cmdline);
    SAFE_CLOSE_HANDLE(wr);
    SAFE_CLOSE_HANDLE(pipedprocout);
    SAFE_CLOSE_HANDLE(pipedprocess);
    SAFE_CLOSE_HANDLE(pipedprocjob);

    return rc;
}

static DWORD makelogfile(BOOL ssp)
{
    wchar_t ewb[BBUFSIZ];
    struct  tm *ctm;
    time_t  ctt;
    DWORD   rc;
    DWORD   cm = servicemode ? CREATE_ALWAYS : OPEN_ALWAYS;

    HANDLE  h;

    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    time(&ctt);
    if (uselocaltime)
        ctm = localtime(&ctt);
    else
        ctm = gmtime(&ctt);
    if (wcsftime(ewb, BBUFSIZ, svclogfname, ctm) == 0)
        return svcsyserror(__FUNCTION__, __LINE__, 0, L"invalid format code", svclogfname);
    xfree(logfilename);
    logfilename = xwcsmkpath(servicelogs, ewb);

    h = CreateFileW(logfilename, GENERIC_WRITE,
                    FILE_SHARE_READ, &sazero, cm,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(h)) {
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateFile", logfilename);
        return rc;
    }
    if (rc == ERROR_ALREADY_EXISTS) {
        if (servicemode) {
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
            if (servicemode)
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
    wchar_t *logpb = NULL;
    HANDLE h       = NULL;
    int    rotateprev;
    DWORD  rc;
    DWORD  cm = truncatelogs ? CREATE_ALWAYS : OPEN_ALWAYS;

    if (wcschr(svclogfname, L'%'))
        return makelogfile(ssp);
    if (rotatebysize || rotatebytime)
        rotateprev = 0;
    else
        rotateprev = svcmaxlogs;
    if (logfilename == NULL)
        logfilename = xwcsmkpath(servicelogs, svclogfname);
    if (logfilename == NULL)
        return svcsyserror(__FUNCTION__, __LINE__,
                           ERROR_FILE_NOT_FOUND, L"logfilename", NULL);

    if (svcmaxlogs > 0) {
        if (GetFileAttributesW(logfilename) != INVALID_FILE_ATTRIBUTES) {
            if (ssp)
                reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
            if (rotateprev) {
                logpb = xwcsconcat(logfilename, L".0");
            }
            else {
                wchar_t sb[TBUFSIZ];

                xmktimedstr(sb, TBUFSIZ, L".");
                logpb = xwcsconcat(logfilename, sb);
            }
            if (!MoveFileExW(logfilename, logpb, MOVEFILE_REPLACE_EXISTING)) {
                rc = GetLastError();
                svcsyserror(__FUNCTION__, __LINE__, rc, logfilename, logpb);
                xfree(logpb);
                return rc;
            }
        }
        else {
            rc = GetLastError();
            if (rc != ERROR_FILE_NOT_FOUND) {
                return svcsyserror(__FUNCTION__, __LINE__, rc, L"GetFileAttributes", logfilename);
            }
        }
    }
    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    if (rotateprev) {
        int i;
        /**
         * Rename previous log files
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
                        svcsyserror(__FUNCTION__, __LINE__, rc, logpn, lognn);
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

    h = CreateFileW(logfilename, GENERIC_WRITE,
                    FILE_SHARE_READ, &sazero, cm,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(h)) {
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateFile", logfilename);
        goto failed;
    }
    if (rc == ERROR_ALREADY_EXISTS) {
        if (truncatelogs) {
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
            if (truncatelogs)
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
        svcsyserror(__FUNCTION__, __LINE__, rc, L"truncatelogs", NULL);
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
        svcsyserror(__FUNCTION__, __LINE__, 0, L"rotatelogs failed", NULL);
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
            TerminateProcess(pipedprocess, WAIT_TIMEOUT);
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
        SAFE_CLOSE_HANDLE(pipedprocjob);
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

static int resolverotate(const wchar_t *str)
{
    wchar_t *sp;

    if (IS_EMPTY_WCS(str)) {
        return 0;
    }

    sp = xwcsdup(str);
    if (wcspbrk(sp, L"BKMG")) {
        LONGLONG len;
        LONGLONG siz;
        LONGLONG mux = 0;
        wchar_t *rp  = sp;
        wchar_t *ep  = zerostring;

        siz = _wcstoi64(rp, &ep, 10);
        if ((siz < 0) || (ep == rp) || (errno == ERANGE))
            return __LINE__;
        switch (ep[0]) {
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
                return __LINE__;
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
        wchar_t *rp  = sp;
        wchar_t *p;

        rotateint    = 0;
        rotatebytime = FALSE;
        rotatetmo.QuadPart = 0;

        p = wcschr(rp, L':');
        if (p == NULL) {
            wchar_t *ep = zerostring;
            long     mm = wcstol(rp, &ep, 10);

            if ((mm < 0) || (errno == ERANGE) || (*ep != WNUL)) {
                DBG_PRINTF("invalid rotate timeout %S", rp);
                return __LINE__;
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
                if (mm > 1) {
                    rotateint = mm * ONE_MINUTE * CPP_INT64_C(-1);
                    DBG_PRINTF("rotate each %ld minutes", mm);
                    rotatetmo.QuadPart = rotateint;
                    rotatebytime = TRUE;
                }
                else {
                    DBG_PRINTS("rotate by time disabled");
                }
            }
        }
        else {
            int hh, mm, ss;

            *(p++) = WNUL;
            hh = _wtoi(rp);
            if ((hh < 0) || (hh > 23) || (errno == ERANGE))
                return __LINE__;
            rp = p;
            p  = wcschr(rp, L':');
            if (p == NULL)
                return __LINE__;
            *(p++) = WNUL;
            mm = _wtoi(rp);
            if ((mm < 0) || (mm > 59) || (errno == ERANGE))
                return __LINE__;
            rp = p;
            ss = _wtoi(rp);
            if ((ss < 0) || (ss > 59) || (errno == ERANGE))
                return __LINE__;
            DBG_PRINTF("rotate each day at %.2d:%.2d:%.2d",
                      hh, mm, ss);
            resolvetimeout(hh, mm, ss, 1);
        }
    }
    xfree(sp);
    return 0;
}

static DWORD runshutdown(DWORD rt)
{
    wchar_t  rp[TBUFSIZ];
    wchar_t *cmdline;
    HANDLE   wh[2];
    HANDLE   job = NULL;
    DWORD    rc = 0;
    DWORD    cf = CREATE_NEW_CONSOLE;
    int      ip = 0;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;

    DBG_PRINTS("started");

    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));
    si.cb = DSIZEOF(STARTUPINFOW);

    cmdline = xappendarg(1, NULL,    NULL,  svcbatchexe);
#if defined(_DEBUG)
    if (consolemode) {
        cf = CREATE_NEW_PROCESS_GROUP;
        cmdline = xappendarg(1, cmdline, L"++", servicename);
    }
    else
#endif
    {
        cmdline = xappendarg(1, cmdline, L"::", servicename);
    }
    rp[ip++] = L'-';
    if (havelogging && svcendlogfn) {
        if (uselocaltime)
            rp[ip++] = L'l';
        if (truncatelogs)
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
    cmdline = xappendarg(0, cmdline, L"-u", serviceuuid);
    cmdline = xappendarg(1, cmdline, L"-w", servicehome);
    if (havelogging && svcendlogfn) {
        cmdline = xappendarg(1, cmdline, L"-o", servicelogs);
        cmdline = xappendarg(1, cmdline, L"-n", svcendlogfn);
    }
    cmdline = xappendarg(1, cmdline, NULL, shutdownfile);
    cmdline = xappendarg(0, cmdline, NULL, svcendargs);

    DBG_PRINTF("cmdline %S", cmdline);

    ji.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    job = CreateJobObject(&sazero, NULL);
    if (IS_INVALID_HANDLE(job)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateJobObject", NULL);
        goto finished;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &ji,
                                 DSIZEOF(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"SetInformationJobObject", NULL);
        goto finished;
    }

    if (!CreateProcessW(svcbatchexe, cmdline, NULL, NULL, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | cf,
                        NULL, NULL, &si, &cp)) {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateProcess", NULL);
        goto finished;
    }

    if (!AssignProcessToJobObject(job, cp.hProcess)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"AssignProcessToJobObject", NULL);
        TerminateProcess(cp.hProcess, rc);
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
            TerminateProcess(cp.hProcess, ERROR_BROKEN_PIPE);
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
    SAFE_CLOSE_HANDLE(job);

    DBG_PRINTS("done");
    return rc;
}

static unsigned int __stdcall stopthread(void *param)
{
    DWORD ce = CTRL_C_EVENT;
    DWORD pg = 0;

#if defined(_DEBUG)
    if (consolemode && !servicemode) {
        ce = CTRL_BREAK_EVENT;
        pg = GetCurrentProcessId();
    }
    if (servicemode)
        DBG_PRINTS("service stop");
    else
        DBG_PRINTS("shutdown stop");
#endif

    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
    if (shutdownfile && param) {
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
        if (pg)
            DBG_PRINTF("generating %S for process group %lu",
                        xwcsiid(II_CONSOLE, ce), pg);
        else
            DBG_PRINTF("generating %S", xwcsiid(II_CONSOLE, ce));
#endif
        GenerateConsoleCtrlEvent(ce, pg);
        ws = WaitForSingleObject(processended, SVCBATCH_STOP_STEP);
        SetConsoleCtrlHandler(NULL, FALSE);
        if (ws == WAIT_OBJECT_0) {
            DBG_PRINTF("processended by %S", xwcsiid(II_CONSOLE, ce));
            goto finished;
        }
    }
    else {
        DBG_PRINTF("SetConsoleCtrlHandler failed err=%lu", GetLastError());
    }
    DBG_PRINTS("process still running");
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    DBG_PRINTS("child is still active ... terminating");
    SAFE_CLOSE_HANDLE(childprocess);
    SAFE_CLOSE_HANDLE(childprocjob);

finished:
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    SetEvent(svcstopended);
    DBG_PRINTS("done");
    XENDTHREAD(0);
}

static void createstopthread(DWORD rv)
{
    void *sp = INVALID_HANDLE_VALUE;

    if (rv) {
        setsvcstatusexit(rv);
        sp = NULL;
    }
    if (InterlockedIncrement(&sstarted) == 1) {
        ResetEvent(svcstopended);
        xcreatethread(1, 0, &stopthread, sp);
    }
    else {
        InterlockedDecrement(&sstarted);
        DBG_PRINTS("already started");
    }
}

static unsigned int __stdcall rdpipethread(void *unused)
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
                    svcsyserror(__FUNCTION__, __LINE__, rc, L"logappend", NULL);
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
    XENDTHREAD(0);
}

static unsigned int __stdcall wrpipethread(void *unused)
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
    XENDTHREAD(0);
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
                    DBG_PRINTF("service %S signaled", xwcsiid(II_SERVICE, cc));
                    if (haslogstatus) {
                        HANDLE h;

                        EnterCriticalSection(&logfilelock);
                        h = InterlockedExchangePointer(&logfhandle, NULL);

                        if (h) {
                            logfflush(h);
                            logprintf(h, "Signaled %S", xwcsiid(II_SERVICE, cc));
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

static unsigned int __stdcall monitorthread(void *unused)
{
    if (servicemode)
        monitorservice();
    else
        monitorshutdown();

    XENDTHREAD(0);
}


static unsigned int __stdcall rotatethread(void *unused)
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
            rc = GetLastError();
            setsvcstatusexit(rc);
            svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateWaitableTimer", NULL);
            goto finished;
        }
        if (!SetWaitableTimer(wt, &rotatetmo, 0, NULL, NULL, FALSE)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            svcsyserror(__FUNCTION__, __LINE__, rc, L"SetWaitableTimer", NULL);
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
                    svcsyserror(__FUNCTION__, __LINE__, rc, L"rotatelogs", NULL);
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
                    svcsyserror(__FUNCTION__, __LINE__, rc, L"rotatelogs", NULL);
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
    XENDTHREAD(0);
}

static unsigned int __stdcall workerthread(void *unused)
{
    wchar_t *cmdline;
    HANDLE   wh[4];
    DWORD    rc;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;

    DBG_PRINTS("started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    cmdline = xappendarg(1, NULL,    NULL,     comspec);
    cmdline = xappendarg(1, cmdline, L"/D /C", svcbatchfile);
    cmdline = xappendarg(0, cmdline, NULL,     svcbatchargs);

    DBG_PRINTF("cmdline %S", cmdline);
    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    rc = createiopipes(&si, &inputpipewrs, &outputpiperd);
    if (rc != 0) {
        setsvcstatusexit(rc);
        goto finished;
    }
    childprocjob = CreateJobObject(&sazero, NULL);
    if (IS_INVALID_HANDLE(childprocjob)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateJobObject", NULL);
        goto finished;
    }

    ji.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(childprocjob,
                                 JobObjectExtendedLimitInformation,
                                &ji,
                                 DSIZEOF(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"SetInformationJobObject", NULL);
        goto finished;
    }

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    if (!CreateProcessW(comspec, cmdline, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        wenvblock,
                        servicehome,
                       &si, &cp)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateProcess", NULL);
        goto finished;
    }
    childprocess = cp.hProcess;
    childprocpid = cp.dwProcessId;
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    if (!AssignProcessToJobObject(childprocjob, childprocess)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"AssignProcessToJobObject", NULL);
        TerminateProcess(childprocess, rc);
        goto finished;
    }
    wh[0] = childprocess;
    wh[1] = xcreatethread(0, CREATE_SUSPENDED, &rdpipethread, NULL);
    if (IS_INVALID_HANDLE(wh[1])) {
        rc = ERROR_TOO_MANY_TCBS;
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"rdpipethread", NULL);
        TerminateProcess(childprocess, ERROR_OUTOFMEMORY);
        goto finished;
    }
    wh[2] = xcreatethread(0, CREATE_SUSPENDED, &wrpipethread, NULL);
    if (IS_INVALID_HANDLE(wh[2])) {
        rc = ERROR_TOO_MANY_TCBS;
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"wrpipethread", NULL);
        TerminateProcess(childprocess, ERROR_OUTOFMEMORY);
        goto finished;
    }

    ResumeThread(cp.hThread);
    ResumeThread(wh[1]);
    ResumeThread(wh[2]);
    SAFE_CLOSE_HANDLE(cp.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    DBG_PRINTF("running child with pid %lu", childprocpid);
    if (haslogrotate) {
        xcreatethread(1, 0, &rotatethread, NULL);
    }
    WaitForMultipleObjects(3, wh, TRUE, INFINITE);
    CloseHandle(wh[1]);
    CloseHandle(wh[2]);

    DBG_PRINTF("finished %S with pid %lu",
              svcbatchname, childprocpid);
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
    XENDTHREAD(0);
}

#if defined(_MSC_VER) && (_MSC_VER > 1800)
static void xiphandler(const wchar_t *e,
    const wchar_t *w, const wchar_t *f,
    unsigned int n, uintptr_t r)
{
    e = NULL;
    w = NULL;
    f = NULL;
    n = 0;
    r = 0;
    DBG_PRINTS("invalid parameter handler called");
}
#endif

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
            DBG_PRINTF("%S", xwcsiid(II_SERVICE, ctrl));
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
            if (haslogstatus) {
                HANDLE h;
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h) {
                    logfflush(h);
                    logprintf(h, "Service signaled : %S",  xwcsiid(II_SERVICE, ctrl));
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

#if defined(_DEBUG)
/**
 * Debug helper code
 */

static DWORD scmsendcontol(const wchar_t *msg)
{
    ULARGE_INTEGER mid;
    DWORD          rc  = 0;
    DWORD          sz  = DSIZEOF(ULARGE_INTEGER);
    HANDLE         cse = NULL;
    HANDLE         csp = NULL;
    wchar_t       *psn;

    if (_wcsicmp(msg, L"stop") == 0)
        mid.LowPart = SERVICE_CONTROL_STOP;
    else
        mid.LowPart = _wtoi(msg);

    if ((mid.LowPart < 1) || (mid.LowPart > 255)) {
        svcsyserror(__FUNCTION__, __LINE__, 0, L"Invalid control message", msg);
        return ERROR_INVALID_PARAMETER;
    }
    mid.HighPart = GetCurrentProcessId();

    psn = xwcsconcat(SVCBATCH_SCSNAME, servicename);
    cse = OpenEventW(EVENT_MODIFY_STATE, FALSE, psn);

    if (IS_INVALID_HANDLE(cse))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"OpenEvent", psn);
    xfree(psn);

    if (!SetEvent(cse)) {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"SetEvent", servicename);
        goto finished;
    }
    psn = xwcsconcat(SVCBATCH_SCMNAME, servicename);
    do {
        rc  = ERROR_PIPE_CONNECTED;
        csp = CreateFileW(psn, GENERIC_READ | GENERIC_WRITE,
                          0, NULL, OPEN_EXISTING, 0, NULL);
        if (IS_INVALID_HANDLE(csp)) {
            rc = GetLastError();

            if (rc == ERROR_PIPE_BUSY) {
                rc = 0;
                if (!WaitNamedPipe(psn, 10000)) {
                    DBG_PRINTS("could not open pipe within 10 seconds");
                    rc = ERROR_PIPE_BUSY;
                }
            }
        }
    } while (rc == 0);

    xfree(psn);
    if (rc == ERROR_PIPE_CONNECTED) {
        DWORD wr;
        DWORD pm = PIPE_READMODE_MESSAGE;

        DBG_PRINTF("connected to service %S", servicename);
        rc = 0;
        if (!SetNamedPipeHandleState(csp, &pm, NULL, NULL)) {
            rc = GetLastError();
            svcsyserror(__FUNCTION__, __LINE__, rc, L"SetNamedPipeHandleState", servicename);
            goto finished;
        }
        if (WriteFile(csp, (LPCVOID)&mid, sz, &wr, NULL) && (wr != 0)) {
            if (wr != sz) {
                rc = ERROR_INVALID_DATA;
                DBG_PRINTF("wrote %lu instead %lu bytes", wr, sz);
            }
            else {
                DBG_PRINTF("send %S to %S", xwcsiid(II_SERVICE, mid.LowPart), servicename);
            }
        }
        else {
            rc = GetLastError();
        }
    }
finished:
    SAFE_CLOSE_HANDLE(cse);
    SAFE_CLOSE_HANDLE(csp);

    return rc;
}

static DWORD scmhandlectrl(HANDLE csp)
{
    DWORD rc = 0;
    DWORD rd;
    DWORD sz = DSIZEOF(ULARGE_INTEGER);
    ULARGE_INTEGER mid;

    if (ReadFile(csp, (LPVOID)&mid, sz, &rd, NULL) && (rd != 0)) {
        if (rd != sz) {
             DBG_PRINTF("read %lu insted %lu bytes", rd, sz);
             rc = ERROR_INVALID_DATA;
        }
        else {
            DBG_PRINTF("sending %S to servicehandler from %lu",
                      xwcsiid(II_SERVICE, mid.LowPart), mid.HighPart);
            rc = servicehandler(mid.LowPart, 0, NULL, NULL);
            DBG_PRINTF("servicehandler returned %lu", rc);
        }
    }
    else {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"ReadFile", servicename);
    }
    DisconnectNamedPipe(csp);
    return rc;
}

static unsigned int __stdcall scmctrlthread(void *unused)
{
    HANDLE   wh[2];
    HANDLE   cse = NULL;
    HANDLE   csp = NULL;
    wchar_t *psn = NULL;
    DWORD    rc  = 0;

    DBG_PRINTS("started");

    psn = xwcsconcat(SVCBATCH_SCSNAME, servicename);
    cse = CreateEventW(&sazero, TRUE, FALSE, psn);
    if (IS_INVALID_HANDLE(cse)) {
        svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", psn);
        goto finished;
    }
    DBG_PRINTF("created scm event %S", psn);
    xfree(psn);
    psn = xwcsconcat(SVCBATCH_SCMNAME, servicename);

    wh[0] = processended;
    wh[1] = cse;

    do {
        DWORD ws;
        DWORD pc;

        DBG_PRINTS("waiting for scm signal");
        csp = CreateNamedPipeW(psn,
                              PIPE_ACCESS_DUPLEX,
                              PIPE_TYPE_MESSAGE |
                              PIPE_READMODE_MESSAGE |
                              PIPE_WAIT,
                              1,
                              BBUFSIZ,
                              BBUFSIZ,
                              0,
                              NULL);
        if (IS_INVALID_HANDLE(csp)) {
            rc = GetLastError();
            svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateNamedPipe", psn);
            goto finished;
        }
        DBG_PRINTF("created pipe %S", psn);;
        ws = WaitForMultipleObjects(2, wh, FALSE, INFINITE);
        switch (ws) {
            case WAIT_OBJECT_0:
                DBG_PRINTS("processended signaled");
                rc = 1;
            break;
            case WAIT_OBJECT_1:
                DBG_PRINTS("scm signaled");

                pc = ERROR_PIPE_CONNECTED;
                if (!ConnectNamedPipe(csp, NULL))
                    pc = GetLastError();
                if (pc == ERROR_PIPE_CONNECTED)
                    scmhandlectrl(csp);
                else
                    DBG_PRINTF("client could not connect %lu", pc);
                SAFE_CLOSE_HANDLE(csp);
                ResetEvent(cse);
            break;
        }
    } while (rc == 0);

finished:
    xfree(psn);
    SAFE_CLOSE_HANDLE(cse);
    SAFE_CLOSE_HANDLE(csp);

    DBG_PRINTS("done");
    XENDTHREAD(0);
}

#endif

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
        svcsyserror(__FUNCTION__, __LINE__, ERROR_INVALID_PARAMETER, L"Missing servicename", NULL);
        exit(ERROR_INVALID_PARAMETER);
        return;
    }
    if (servicemode) {
#if defined(_DEBUG)
        if (consolemode) {
            xcreatethread(1, 0, &scmctrlthread, NULL);
        }
        else
#endif
        {
            hsvcstatus = RegisterServiceCtrlHandlerExW(servicename, servicehandler, NULL);
            if (IS_INVALID_HANDLE(hsvcstatus)) {
                svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"RegisterServiceCtrlHandlerEx", NULL);
                exit(ERROR_INVALID_HANDLE);
                return;
            }
        }
        DBG_PRINTF("started %S", servicename);
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

        if (havelogging) {
            rv = createlogsdir();
            if (rv) {
                reportsvcstatus(SERVICE_STOPPED, rv);
                return;
            }
        }
        if (truncatelogs)
            svcmaxlogs = 0;
    }
    else {
        DBG_PRINTF("shutting down %S", servicename);
        servicelogs = xwcsdup(outdirparam);
    }
    /**
     * Add additional environment variables
     * They are unique to this service instance
     */
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_BASE=", servicebase);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_HOME=", servicehome);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_NAME=", servicename);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_UUID=", serviceuuid);

    qsort((void *)dupwenvp, dupwenvc, sizeof(wchar_t *), xenvsort);
    /**
     * Convert environment array to environment block
     */
    wenvblock = xenvblock(dupwenvc, (const wchar_t **)dupwenvp);
    if (wenvblock == NULL) {
        svcsyserror(__FUNCTION__, __LINE__, 0, L"bad environment", NULL);
        reportsvcstatus(SERVICE_STOPPED, ERROR_OUTOFMEMORY);
        return;
    }
    xwaafree(dupwenvp);

    if (havelogging) {
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
        if (haspipedlogs)
            rv = openlogpipe(TRUE);
        else
            rv = openlogfile(TRUE);

        if (rv != 0) {
            svcsyserror(__FUNCTION__, __LINE__, 0, L"openlog failed", NULL);
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

    wh[1] = xcreatethread(0, 0, &monitorthread, NULL);
    if (IS_INVALID_HANDLE(wh[1])) {
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__FUNCTION__, __LINE__, rv, L"monitorthread", NULL);
        goto finished;
    }
    wh[0] = xcreatethread(0, 0, &workerthread, NULL);
    if (IS_INVALID_HANDLE(wh[0])) {
        SetEvent(monitorevent);
        CloseHandle(wh[1]);
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__FUNCTION__, __LINE__, rv, L"workerthread", NULL);
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
            if (shutdownfile) {
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
    SAFE_CLOSE_HANDLE(childprocjob);

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
    SAFE_CLOSE_HANDLE(childprocjob);
    SAFE_CLOSE_HANDLE(pipedprocjob);

    DeleteCriticalSection(&logfilelock);
    DeleteCriticalSection(&servicelock);
#if defined(_DEBUG)
    if ((dbgoutstream != NULL) && (dbgoutstream != stdout)) {
        fclose(dbgoutstream);
    }
#endif
}

static int xwmaininit(const wchar_t **wenv)
{
    wchar_t *bb;
    DWORD    sm = FBUFSIZ;
    DWORD    sz;
    DWORD    nn;
    int      ec = 0;

    sz = sm - 2;
    bb = xwmalloc(sz);
    nn = GetModuleFileNameW(NULL, bb, sz);
    if (nn == 0)
        return 0;
    while (nn >= sz) {
        sm = sm * 2;
        sz = sm - 2;
        xfree(bb);
        bb = xwmalloc(sz);
        nn = GetModuleFileNameW(NULL, bb, sz);
        if (nn == 0)
            return 0;
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
        return 0;
    svcbatchexe = bb;
    QueryPerformanceFrequency(&pcfrequency);
    QueryPerformanceCounter(&pcstarttime);

    if (wenv) {
        while (wenv[ec])
            ++ec;
    }

    return ec;
}

int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
{
    int         i;
    int         opt;
    int         envc;
    int         rcnt  = 0;
    int         rv    = 0;
    wchar_t     bb[4] = { L'-', WNUL, WNUL, WNUL };
    HANDLE      h;
    SERVICE_TABLE_ENTRYW se[2];
    const wchar_t *batchparam   = NULL;
    const wchar_t *svchomeparam = NULL;
    const wchar_t *svcendparam  = NULL;
    const wchar_t *logpipeparam = NULL;
    const wchar_t *lognameparam = SVCBATCH_LOGNAME;
    const wchar_t *rparam[2];

#if defined(_DEBUG)
# if defined(_MSC_VER) && (_MSC_VER > 1800)
    _set_invalid_parameter_handler(xiphandler);
# endif
   _CrtSetReportMode(_CRT_ASSERT, 0);
#endif
    envc = xwmaininit(wenv);
    if (envc == 0)
        return ERROR_BAD_ENVIRONMENT;
    /**
     * Make sure children (cmd.exe) are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
#if defined(_DEBUG)
    if (argc > 2) {
        const wchar_t *p = wargv[1];
        if ((((p[0] == L'-') && (p[1] == L'-')) || ((p[0] == L'+') && (p[1] == L'+'))) && (p[2] == WNUL)) {
            consolemode  = TRUE;
            servicename  = xwcsdup(wargv[2]);
            dbgoutstream = stdout;
            if (wcschr(servicename, L'\\')) {
                DBG_PRINTF("Service name '%S' cannot have backslash character", servicename);
                return ERROR_INVALID_PARAMETER;
            }
            if (p[0] == L'+') {
                servicemode = FALSE;
                cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT ;
                cwsappname  = CPP_WIDEN(SHUTDOWN_APPNAME);
            }
            else {
                /**
                 * Services current directory is always
                 * set to SystemDirectory
                 *
                 * hard coded for now
                 */
                SetCurrentDirectoryW(L"C:\\Windows\\System32");
            }
            wargv[2] = wargv[0];
            argc    -= 2;
            wargv   += 2;
        }
    }
    if (consolemode) {
        DBG_PRINTF("Running %S in console mode\n", servicename);
    }
# if (_DEBUG > 1)
    else {
        dbgoutstream = xmkdbgtemp();
        if (dbgoutstream == NULL)
            return ERROR_ACCESS_DENIED;
    }
# endif
#endif
    /**
     * Check if running as service or as a child process.
     */
    if (argc > 2) {
        const wchar_t *p = wargv[1];
        if ((p[0] == L':') && (p[1] == L':') && (p[2] == WNUL)) {
            servicemode = FALSE;
            servicename = xwcsdup(wargv[2]);
            cnamestamp  = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT ;
            cwsappname  = CPP_WIDEN(SHUTDOWN_APPNAME);

            wargv[2]    = wargv[0];
            argc       -= 2;
            wargv      += 2;
        }
    }

    if (argc == 1) {
        fprintf(stdout, "%s\n\n", cnamestamp);
        fprintf(stdout, "Visit " SVCBATCH_PROJECT_URL " for more details\n");

        return 0;
    }
    wnamestamp = xcwiden(cnamestamp);
    DBG_PRINTS(cnamestamp);

    while ((opt = xwgetopt(argc, wargv, L"a:bc:e:lm:n:o:pqr:s:tu:vw:")) != EOF) {
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
                truncatelogs = TRUE;
            break;
            case L'v':
                haslogstatus = TRUE;
            break;
            /**
             * Options with arguments
             */
            case L'a':
                svcendargs   = xappendarg(1, svcendargs,  NULL, xwoptarg);
            break;
#if defined(_DEBUG)
            case L'c':
                if (consolemode)
                    return scmsendcontol(xwoptarg);
                else
                    return svcsyserror(__FUNCTION__, __LINE__, 0,
                                       L"Cannot use -c command option when running as service", NULL);
            break;
#endif
            case L'e':
                logpipeparam = xwoptarg;
            break;
            case L'm':
                svcmaxlogs   = _wtoi(xwoptarg);
                if ((svcmaxlogs < 0) || (svcmaxlogs > SVCBATCH_MAX_LOGS))
                    return svcsyserror(__FUNCTION__, __LINE__, 0,
                                       L"Invalid -m command option value", xwoptarg);
            break;
            case L'n':
                lognameparam = xwoptarg;
            break;
            case L'o':
                outdirparam  = xwoptarg;
            break;
            case L'r':
                if (rcnt > 1)
                    return svcsyserror(__FUNCTION__, __LINE__, 0,
                                       L"Too many -r options", xwoptarg);
                else
                    rparam[rcnt++] = xwoptarg;
            break;
            case L's':
                svcendparam  = xwoptarg;
            break;
            case L'w':
                svchomeparam = xwoptarg;
            break;
            /**
             * Private options
             */
            case L'u':
                serviceuuid  = xwcsdup(xwoptarg);
            break;
            /**
             * Invalid option
             */
            case ENOENT:
                bb[1] = xwoption;
                return svcsyserror(__FUNCTION__, __LINE__, 0,
                                   L"Missing argument for command line option", bb);
            break;
            default:
                bb[1] = xwoption;
                return svcsyserror(__FUNCTION__, __LINE__, 0,
                                   L"Invalid command line option", bb);
            break;
        }
    }

    if (!havelogging) {
        /**
         * The -q option was defined
         *
         * Disable all log related options
         */
        outdirparam  = NULL;
        logpipeparam = NULL;
        lognameparam = NULL;
        svcendlogfn  = NULL;
        svcmaxlogs   = 0;
        truncatelogs = FALSE;
        haslogstatus = FALSE;
        haslogrotate = FALSE;
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
    if (!xisbatchfile(batchparam))
        return svcsyserror(__FUNCTION__, __LINE__, 0, L"Invalid batch file", batchparam);
    if (IS_EMPTY_WCS(serviceuuid)) {
        if (servicemode)
            serviceuuid = xuuidstring();
        else
            return svcsyserror(__FUNCTION__, __LINE__, 0,
                               L"Missing -u <SVCBATCH_SERVICE_UUID> parameter", NULL);
    }
    if (IS_EMPTY_WCS(serviceuuid))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"xuuidstring", NULL);
    comspec = xgetenv(L"COMSPEC");
    if (IS_EMPTY_WCS(comspec))
        return svcsyserror(__FUNCTION__, __LINE__, ERROR_ENVVAR_NOT_FOUND, L"COMSPEC", NULL);
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
                    return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
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
                    return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, batchparam, NULL);

                if (svchomeparam == NULL) {
                    servicehome = servicebase;
                }
                else {
                    SetCurrentDirectoryW(servicebase);
                    servicehome = getrealpathname(svchomeparam, 1);
                    if (IS_EMPTY_WCS(servicehome))
                        return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
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
                    return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, svchomeparam, NULL);
            }
         }
         SetCurrentDirectoryW(servicehome);
         if (svcbatchfile == NULL) {
            if (resolvebatchname(batchparam))
                return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, batchparam, NULL);
         }
    }
    else {
        servicehome  = xwcsdup(svchomeparam);
        svcmaxlogs   = 0;
        haslogrotate = FALSE;
    }
    if (resolvebatchname(batchparam))
        return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, batchparam, NULL);

    svclogfname = xwcsdup(lognameparam);
    if (servicemode) {
        if (svcmaxlogs > 0)
            haslogrotate = TRUE;
        if (svcendparam) {
            if (!xisbatchfile(svcendparam))
                return svcsyserror(__FUNCTION__, __LINE__, 0, L"Invalid batch file", svcendparam);

            shutdownfile = getrealpathname(svcendparam, 0);
            if (IS_EMPTY_WCS(shutdownfile))
                return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, svcendparam, NULL);
        }
        else if (svcendargs) {
            /**
             * Use the service batch file as shutdownfile
             */
            shutdownfile = svcbatchfile;
        }
        if (svclogfname) {
            wchar_t *s;

            if (wcspbrk(svclogfname, L"/\\:<>?*|\""))
                return svcsyserror(__FUNCTION__, __LINE__, 0,
                                   L"Invalid -n command option value", svclogfname);

            s = wcschr(svclogfname, L'@');
            if (s) {
                /**
                 * Name is strftime formated
                 * replace @ with % so it can be used by strftime
                 */
                while (*s != WNUL) {
                    if (*s == L'@')
                        *s = L'%';
                    s++;
                }
            }
            s = wcschr(svclogfname, L';');
            if (s) {
                *(s++) = WNUL;
                if ((*s == WNUL) || (_wcsicmp(s, L"NUL") == 0))
                    svcendlogfn = NULL;
                else
                    svcendlogfn = s;
            }
            if (svcendlogfn) {
                if (_wcsicmp(svclogfname, svcendlogfn) == 0)
                    return svcsyserror(__FUNCTION__, __LINE__, 0,
                                       L"Log and shutdown file names are the same", svclogfname);
            }
        }
        if (logpipeparam) {
            logredirargv = CommandLineToArgvW(logpipeparam, &logredirargc);
            if (logredirargv == NULL)
                return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), logpipeparam, NULL);

            logredirect = getrealpathname(logredirargv[0], 0);
            if (logredirect == NULL)
                return svcsyserror(__FUNCTION__, __LINE__, ERROR_PATH_NOT_FOUND, logredirargv[0], NULL);
            haspipedlogs = TRUE;
            haslogrotate = FALSE;
        }
    }

    dupwenvp = xwaalloc(envc + 6);
    for (i = 0; i < envc; i++) {
        if (!xwstartswith(wenv[i], L"SVCBATCH_SERVICE_"))
            dupwenvp[dupwenvc++] = xwcsdup(wenv[i]);
    }

    memset(&ssvcstatus, 0, sizeof(SERVICE_STATUS));
    memset(&sazero,     0, sizeof(SECURITY_ATTRIBUTES));
    sazero.nLength = DSIZEOF(SECURITY_ATTRIBUTES);
    /**
     * Create logic state events
     */
    svcstopended = CreateEvent(&sazero, TRUE, TRUE,  NULL);
    if (IS_INVALID_HANDLE(svcstopended))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", L"svcstopended");
    processended = CreateEvent(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(processended))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", L"processended");
    if (servicemode) {
        if (shutdownfile) {
            wchar_t *psn = xwcsconcat(SHUTDOWN_IPCNAME, serviceuuid);
            ssignalevent = CreateEventW(&sazero, TRUE, FALSE, psn);
            if (IS_INVALID_HANDLE(ssignalevent))
                return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", psn);
            xfree(psn);
        }
        if (haslogrotate) {
            for (i = 0; i < rcnt; i++) {
                rv = resolverotate(rparam[i]);
                if (rv != 0)
                    return svcsyserror(__FUNCTION__, rv, 0, L"Cannot resolve", rparam[i]);
            }
            logrotatesig = CreateEvent(&sazero, TRUE, FALSE, NULL);
            if (IS_INVALID_HANDLE(logrotatesig))
                return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", L"logrotatesig");
        }
    }
    else {
        wchar_t *psn = xwcsconcat(SHUTDOWN_IPCNAME, serviceuuid);
        ssignalevent = OpenEventW(SYNCHRONIZE, FALSE, psn);
        if (IS_INVALID_HANDLE(ssignalevent))
            return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"OpenEvent", psn);
        xfree(psn);
    }

    monitorevent = CreateEvent(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(monitorevent))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", L"monitorevent");
    InitializeCriticalSection(&servicelock);
    InitializeCriticalSection(&logfilelock);
    atexit(objectscleanup);

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
                return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"GetStdHandle", NULL);
            DBG_PRINTS("allocated new console");
        }
        else {
            return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"AllocConsole", NULL);
        }
    }
    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = NULL;
    se[1].lpServiceProc = NULL;
    if (servicemode) {
        DBG_PRINTS("starting service");
#if defined(_DEBUG)
        if (consolemode) {
            servicemain(0, NULL);
            rv = ssvcstatus.dwWin32ExitCode;
        }
        else
#endif
        {
            if (!StartServiceCtrlDispatcherW(se))
                rv = svcsyserror(__FUNCTION__, __LINE__, GetLastError(),
                                 L"StartServiceCtrlDispatcher", NULL);
        }
    }
    else {
        DBG_PRINTS("starting shutdown");
        servicemain(0, NULL);
        rv = ssvcstatus.dwServiceSpecificExitCode;
    }
    DBG_PRINTF("done (%lu)", rv);
    return rv;
}
