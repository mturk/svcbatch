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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <process.h>
#include <shellapi.h>
#include "svcbatch.h"

static volatile LONG         monitorsig  = 0;
static volatile LONG         svcworking  = 1;
static volatile LONG         sstarted    = 0;
static volatile LONG         rstarted    = 0;
static volatile LONG         sscstate    = SERVICE_START_PENDING;
static volatile LONG         rotatecount = 0;
static volatile LONG         logwritten  = 0;
static volatile HANDLE       logfhandle  = NULL;
static SERVICE_STATUS_HANDLE hsvcstatus  = NULL;
static SERVICE_STATUS        ssvcstatus;
static CRITICAL_SECTION      servicelock;
static CRITICAL_SECTION      logfilelock;
static SECURITY_ATTRIBUTES   sazero;
static LONGLONG              rotateint   = SVCBATCH_LOGROTATE_DEF;
static LARGE_INTEGER         rotatetmo   = {{ 0, 0 }};
static LARGE_INTEGER         rotatesiz   = {{ 0, 0 }};
static HANDLE                rotatedev   = NULL;

static wchar_t  *comspec          = NULL;
static wchar_t **dupwenvp         = NULL;
static int       dupwenvc         = 0;
static wchar_t  *wenvblock        = NULL;
static int       hasctrlbreak     = 0;
static int       usecleanpath     = 0;
static int       usesafeenv       = 0;
static int       autorotate       = 0;
static int       consolemode      = 0;
static int       runbatchmode     = 0;
static int       nonsvcmode       = 0;

static wchar_t  *svcbatchfile     = NULL;
static wchar_t  *batchdirname     = NULL;
static wchar_t  *svcbatchexe      = NULL;
static wchar_t  *servicebase      = NULL;
static wchar_t  *servicename      = NULL;
static wchar_t  *servicehome      = NULL;
static wchar_t  *serviceuuid      = NULL;
static wchar_t  *serviceexec      = NULL;

static wchar_t  *loglocation      = NULL;
static wchar_t  *logfilename      = NULL;
static ULONGLONG logtickcount     = CPP_UINT64_C(0);
#if defined(_DBGVIEW)
static CRITICAL_SECTION dbgviewlock;
#if defined(_DBGVIEW_SAVE)
static volatile LONG   dbgwritten = 0;
static volatile HANDLE dbgfhandle = NULL;
static ULONGLONG       dbginitick;
#endif
#endif
static HANDLE    childprocjob     = NULL;
static HANDLE    childprocess     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    svcexecended     = NULL;
static HANDLE    processended     = NULL;
static HANDLE    monitorevent     = NULL;
static HANDLE    workingevent     = NULL;
static HANDLE    outputpiperd     = NULL;
static HANDLE    inputpipewrs     = NULL;

static wchar_t      zerostring[4] = { L'\0', L'\0', L'\0', L'\0' };
static wchar_t      CRLFW[4]      = { L'\r', L'\n', L'\0', L'\0' };
static char         CRLFA[4]      = { '\r', '\n', '\r', '\n' };
static wchar_t     *cwargv[]      = { NULL, NULL };

static const char    *cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_STR " " SVCBATCH_BUILD_STAMP;
static const wchar_t *wnamestamp  = CPP_WIDEN(SVCBATCH_SVCNAME);
static const wchar_t *wnbatchapp  = CPP_WIDEN(SVCBATCH_NAME);

static const wchar_t *stdwinpaths = L";"    \
    L"%SystemRoot%\\System32;"              \
    L"%SystemRoot%;"                        \
    L"%SystemRoot%\\System32\\Wbem;"        \
    L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0";

static const wchar_t *removeenv[] = {
    L"_=",
    L"!::=",
    L"!;=",
    L"SVCBATCH_SERVICE_BASE=",
    L"SVCBATCH_SERVICE_HOME=",
    L"SVCBATCH_SERVICE_LOGDIR=",
    L"SVCBATCH_SERVICE_NAME=",
    L"SVCBATCH_SERVICE_SELF=",
    L"SVCBATCH_SERVICE_UUID=",
    L"PATH=",
    NULL
};

static const wchar_t *safewinenv[] = {
    L"ALLUSERSPROFILE=",
    L"APPDATA=",
    L"COMMONPROGRAMFILES(X86)=",
    L"COMMONPROGRAMFILES=",
    L"COMMONPROGRAMW6432=",
    L"COMPUTERNAME=",
    L"COMSPEC=",
    L"HOMEDRIVE=",
    L"HOMEPATH=",
    L"LOCALAPPDATA=",
    L"LOGONSERVER=",
    L"NUMBER_OF_PROCESSORS=",
    L"OS=",
    L"PATHEXT=",
    L"PROCESSOR_ARCHITECTURE=",
    L"PROCESSOR_ARCHITEW6432=",
    L"PROCESSOR_IDENTIFIER=",
    L"PROCESSOR_LEVEL=",
    L"PROCESSOR_REVISION=",
    L"PROGRAMDATA=",
    L"PROGRAMFILES(X86)=",
    L"PROGRAMFILES=",
    L"PROGRAMW6432=",
    L"PSMODULEPATH=",
    L"PUBLIC=",
    L"SESSIONNAME=",
    L"SYSTEMDRIVE=",
    L"SYSTEMROOT=",
    L"TEMP=",
    L"TMP=",
    L"USERDNSDOMAIN=",
    L"USERDOMAIN=",
    L"USERDOMAIN_ROAMINGPROFILE=",
    L"USERNAME=",
    L"USERPROFILE=",
    L"WINDIR=",
    NULL
};

static wchar_t *xwalloc(size_t size)
{
    wchar_t *p = (wchar_t *)calloc(size + 2, sizeof(wchar_t));
    if (p == NULL)
        _exit(ERROR_OUTOFMEMORY);

    return p;
}

static wchar_t **waalloc(size_t size)
{
    wchar_t **p = (wchar_t **)calloc(size + 2, sizeof(wchar_t *));
    if (p == NULL)
        _exit(ERROR_OUTOFMEMORY);

    return p;
}

static void xfree(void *m)
{
    if (m != NULL)
        free(m);
}

static wchar_t *xwcsdup(const wchar_t *s)
{
    wchar_t *p;
    size_t   n;

    if (IS_EMPTY_WCS(s))
        return NULL;
    n = wcslen(s);
    p = xwalloc(n);
    wmemcpy(p, s, n);
    return p;
}

static wchar_t *xgetenv(const wchar_t *s)
{
    DWORD    n;
    wchar_t  e[2];
    wchar_t *d = NULL;

    if ((n = GetEnvironmentVariableW(s, e, 1)) != 0) {
        d = xwalloc(n);
        GetEnvironmentVariableW(s, d, n);
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

static wchar_t *xwcsconcat(const wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp, *rv;
    size_t l1 = xwcslen(s1);
    size_t l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;

    cp = rv = xwalloc(l1 + l2);

    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    cp += l1;
    if(l2 > 0)
        wmemcpy(cp, s2, l2);
    return rv;
}

static wchar_t *xwcsappend(wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp, *rv;
    size_t l1 = xwcslen(s1);
    size_t l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;

    cp = rv = xwalloc(l1 + l2);

    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    cp += l1;
    if(l2 > 0)
        wmemcpy(cp, s2, l2);
    xfree(s1);
    return rv;
}

static wchar_t *xwcsvarcat(const wchar_t *p, ...)
{
    const wchar_t *ap;
    wchar_t *cp, *rp;
    int  sls[32];
    int  len = 0;
    int  cnt = 1;
    va_list vap;

    sls[0] = xwcslen(p);

    va_start(vap, p);
    while ((ap = va_arg(vap, const wchar_t *)) != NULL) {
        sls[cnt] = xwcslen(ap);
        len += sls[cnt];
        if (cnt++ > 30)
            break;
    }
    va_end(vap);
    len += sls[0];

    if (len == 0)
        return NULL;
    cp = rp = xwalloc(len);
    if (sls[0] != 0) {
        wmemcpy(cp, p, sls[0]);
        cp += sls[0];
    }

    cnt = 1;

    va_start(vap, p);
    while ((ap = va_arg(vap, const wchar_t *)) != NULL) {
        if ((len = sls[cnt]) != 0) {
            wmemcpy(cp, ap, len);
            cp += len;
        }
        if (cnt++ > 30)
            break;
    }
    va_end(vap);
    return rp;
}

static int strstartswith(const wchar_t *str, const wchar_t *src)
{
    while (*str != L'\0') {
        if (towlower(*str) != towlower(*src))
            break;
        str++;
        src++;
        if (*src == L'\0')
            return 1;
    }
    return 0;
}

static int envsort(const void *arg1, const void *arg2)
{
    return _wcsicoll(*((wchar_t **)arg1), *((wchar_t **)arg2));
}

static int wcshavespace(const wchar_t *s)
{
    while (*s != L'\0') {
        if (iswspace(*s))
            return 1;
        s++;
    }
    return 0;
}

static wchar_t *xappendarg(wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp, *rv;
    size_t l1 = xwcslen(s1);
    size_t l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;

    cp = rv = xwalloc(l1 + l2 + 4);

    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    cp += l1;
    if(l2 > 0) {
        if (wcshavespace(s2)) {
            *(cp++) = L'\\';
            *(cp++) = L'"';
            wmemcpy(cp, s2, l2);
            cp += l2;
            *(cp++) = L'\\';
            *(cp++) = L'"';
        }
        else
            wmemcpy(cp, s2, l2);
    }
    xfree(s1);
    return rv;
}

static wchar_t *xrmspaces(wchar_t *dest, const wchar_t *src)
{
    wchar_t *dp = NULL;

    if (IS_EMPTY_WCS(src))
        return dest;
    while (*src != L'\0') {
        if (!iswspace(*src)) {
            if (dp == NULL)
                dp = dest;
            *dest++ = *src;
        }
        ++src;
    }
    *dest = L'\0';
    return dp;
}

static void xcleanwinpath(wchar_t *s)
{
    int i;

    if (IS_EMPTY_WCS(s))
        return;
    for (i = 0; s[i] != L'\0'; i++) {
        if (s[i] == L'/')
            s[i] =  L'\\';
    }
    --i;
    while (i > 2) {
        if (s[i] == L';')
            s[i--] = L'\0';
        else
            break;
    }
    while (i > 1) {
        if (s[i] ==  L'\\')
            s[i--] = L'\0';
        else
            break;
    }

}

/**
 * Check if the path doesn't start
 * with \ or C:\
 */
static int isrelativepath(const wchar_t *p)
{
    if (p[0] < 128) {
        if ((p[0] == L'\\') || (isalpha(p[0]) && (p[1] == L':')))
            return 0;
    }
    return 1;
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
    b = xwalloc(38);
    for (i = 0, x = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            b[x++] = '-';
        b[x++] = xb16[d[i] >> 4];
        b[x++] = xb16[d[i] & 0x0F];
    }

    return b;
}

#if defined(_DBGVIEW)
static void dbgprints(const char *funcname, const char *string)
{
    char    buf[SBUFSIZ];
    char   *bp;
    size_t  blen = SBUFSIZ - 1;
    int     n = 0;
    int     z = 0;
#if defined(_DBGVIEW_SAVE)
    HANDLE  h;
    DWORD   ct, wr, ss, ms;
#endif

    bp = buf;
#if defined(_DBGVIEW_SAVE)
    ct = (DWORD)(GetTickCount64() - dbginitick);
    ms = (DWORD)(ct % MS_IN_SECOND);
    ss = (DWORD)(ct / MS_IN_SECOND);
    z  = _snprintf(bp, blen,
                   "[%.6lu.%.3lu] [%.4lu] ",
                   ss, ms, GetCurrentProcessId());
    bp = bp + z;
#endif
    n = _snprintf(bp, blen - z,
                  "[%.4lu] %-16s ",
                  GetCurrentThreadId(), funcname);
    bp = bp + n;
    strncat(bp, string, blen - n - z);
    buf[SBUFSIZ - 1] = '\0';
    EnterCriticalSection(&dbgviewlock);
    if (consolemode) {
        fputs(buf,  stdout);
        fputc('\n', stdout);
    }
    else {
        OutputDebugStringA(buf + z);
    }
#if defined(_DBGVIEW_SAVE)
    h = InterlockedExchangePointer(&dbgfhandle, NULL);
    if (h != NULL) {
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (SetFilePointerEx(h, ee, NULL, FILE_END)) {
            if (WriteFile(h, buf, (DWORD)strlen(buf), &wr, NULL)) {
                InterlockedAdd(&dbgwritten, wr);
                WriteFile(h, CRLFA, 2, &wr, NULL);
                if (InterlockedAdd(&dbgwritten, wr) >= SVCBATCH_LOGFLUSH_SIZE) {
                    FlushFileBuffers(h);
                    InterlockedExchange(&dbgwritten, 0);
                }
            }
        }
        InterlockedExchangePointer(&dbgfhandle, h);
    }
#endif
    LeaveCriticalSection(&dbgviewlock);
}

static void dbgprintf(const char *funcname, const char *format, ...)
{
    char    buf[SBUFSIZ];
    va_list ap;

    va_start(ap, format);
    _vsnprintf(buf, SBUFSIZ -1, format, ap);
    va_end(ap);
    buf[SBUFSIZ - 1] = '\0';
    dbgprints(funcname, buf);
}
#else
#define dbgprints(_a, _b) (void)0
#define dbgprintf(x, ...) (void)0
#endif

static void xwinapierror(wchar_t *buf, DWORD bufsize, DWORD statcode)
{
    DWORD len;
    len = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                         NULL,
                         statcode,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         buf,
                         bufsize,
                         NULL);
    if (len) {
        do {
            buf[len--] = L'\0';
        } while ((len > 0) && ((buf[len] == L'.') || (buf[len] <= L' ')));
        while (len-- > 0) {
            if (iswspace(buf[len]))
                buf[len] = L' ';
        }
    }
    else {
        _snwprintf(buf, bufsize, L"Unknown Win32 error code: %lu", statcode);
        buf[bufsize - 1] = L'\0';
    }
}

static int setupeventlog(void)
{
    static int ssrv = 0;
    static volatile LONG eset   = 0;
    static const wchar_t emsg[] = L"%SystemRoot%\\System32\\netmsg.dll\0";
    wchar_t *kname;
    DWORD c;
    HKEY  k;

    if (consolemode)
        return 0;
    if (InterlockedIncrement(&eset) > 1)
        return ssrv;
    kname = xwcsconcat( L"SYSTEM\\CurrentControlSet\\Services\\" \
                        L"EventLog\\Application\\", wnamestamp);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kname,
                        0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_READ | KEY_WRITE,
                        NULL, &k, &c) != ERROR_SUCCESS)
        return 0;
    xfree(kname);
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
    ssrv = 1;
finished:
    RegCloseKey(k);
    return ssrv;
}

static DWORD svcsyserror(int line, DWORD ern, const wchar_t *err)
{
    wchar_t        buf[BBUFSIZ];
    wchar_t        erb[MBUFSIZ];
    const wchar_t *errarg[10];

    memset(buf, 0, sizeof(buf));
    _snwprintf(buf, BBUFSIZ - 1, L"svcbatch.c(%d)", line);

    errarg[0] = wnamestamp;
    if (IS_EMPTY_WCS(servicename))
        errarg[1] = L"(undefined)";
    else
        errarg[1] = servicename;
    errarg[2] = L"reported the following error:\r\n";
    errarg[3] = buf;
    errarg[4] = err;
    errarg[5] = NULL;
    errarg[6] = NULL;
    errarg[7] = NULL;
    errarg[8] = NULL;
    errarg[9] = NULL;

    if (consolemode)
        fprintf(stderr, "%S %S\n", errarg[0], errarg[1]);
    else
        dbgprintf(__FUNCTION__, "%S %S", errarg[0], errarg[1]);
    if (ern) {
        memset(erb, 0, sizeof(erb));
        xwinapierror(erb, MBUFSIZ - 1, ern);
        errarg[5] = L":";
        errarg[6] = erb;
        errarg[7] = CRLFW;
        if (consolemode)
            fprintf(stderr, "%S %S : %S\n", buf, err, erb);
        else
            dbgprintf(__FUNCTION__, "%S %S : %S", buf, err, erb);
    }
    else {
        errarg[5] = CRLFW;
        ern = ERROR_INVALID_PARAMETER;
        if (consolemode)
            fprintf(stderr, "%S %S\n", errarg[0], errarg[1]);
        else
            dbgprintf(__FUNCTION__, "%S %S", buf, err);
    }
    if (setupeventlog()) {
        HANDLE es = RegisterEventSourceW(NULL, wnamestamp);
        if (es != NULL) {
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

static HANDLE xcreatethread(int detach, unsigned initflag,
                            unsigned int (__stdcall *threadfn)(void *))
{
    unsigned u;
    HANDLE   h;

    h = (HANDLE)_beginthreadex(NULL, 0, threadfn, NULL, initflag, &u);

    if (IS_INVALID_HANDLE(h))
        return NULL;
    if (detach) {
        CloseHandle(h);
        h = NULL;
    }
    return h;
}

static wchar_t *expandenvstrings(const wchar_t *str)
{
    wchar_t  *buf = NULL;
    DWORD     siz;
    DWORD     len = 0;

    for (siz = 0; str[siz] != L'\0'; siz++) {
        if (str[siz] == L'%')
            len++;
    }
    if (len == 0) {
        buf = xwalloc(siz);
        wmemcpy(buf, str, siz);
    }
    while (buf == NULL) {
        buf = xwalloc(++siz);
        len = ExpandEnvironmentStringsW(str, buf, siz);
        if (len == 0) {
            xfree(buf);
            return NULL;
        }
        if (len > siz) {
            xfree(buf);
            buf = NULL;
            siz = len;
        }
    }
    xcleanwinpath(buf);
    return buf;
}

static wchar_t *getrealpathname(const wchar_t *path, int isdir)
{
    wchar_t    *es;
    wchar_t    *buf  = NULL;
    DWORD       siz  = _MAX_FNAME;
    DWORD       len  = 0;
    HANDLE      fh;
    DWORD       fa   = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

    if (IS_EMPTY_WCS(path))
        return NULL;
    es = expandenvstrings(path);
    if (es == NULL)
        return NULL;
    fh = CreateFileW(es, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, fa, NULL);
    xfree(es);
    if (IS_INVALID_HANDLE(fh))
        return NULL;

    while (buf == NULL) {
        buf = xwalloc(siz);
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

static int resolvesvcbatchexe(void)
{
    LPWSTR     *caa = NULL;
    int         i;

    caa = CommandLineToArgvW(zerostring, &i);
    if (caa == NULL)
        return 0;
    svcbatchexe = getrealpathname(caa[0], 0);
    LocalFree(caa);
    if (svcbatchexe != NULL) {
        i = xwcslen(svcbatchexe);
        while (--i > 0) {
            if (svcbatchexe[i] == L'\\') {
                svcbatchexe[i] = L'\0';
                servicebase = xwcsdup(svcbatchexe);
                svcbatchexe[i] = L'\\';
                return 1;
            }
        }
    }
    return 0;
}

static int resolvebatchname(const wchar_t *batch)
{
    int i;
    int d = 0;

    svcbatchfile = getrealpathname(batch, 0);
    if (IS_EMPTY_WCS(svcbatchfile))
        return 0;

    i = xwcslen(svcbatchfile);
    while (--i > 5) {
        if ((d == 0) && (svcbatchfile[i] == L'.')) {
            d = i;
            svcbatchfile[i] = L'\0';
        }
        if (svcbatchfile[i] == L'\\') {
            svcbatchfile[i] = L'\0';
            batchdirname = xwcsdup(svcbatchfile);
            if (d > 0) {
                if (consolemode)
                    cwargv[0] = xwcsdup(svcbatchfile + i + 1);
                svcbatchfile[d] = L'.';
            }
            svcbatchfile[i] = L'\\';
            return 1;
        }
    }
    return 0;
}

#if defined(_CHECK_IF_SERVICE)
static int runningasservice(void)
{
    int     rv = 0;
    HWINSTA ws = GetProcessWindowStation();

    if (ws != NULL) {
        DWORD len;
        USEROBJECTFLAGS uf;
        wchar_t name[BBUFSIZ];

        if (GetUserObjectInformationW(ws, UOI_NAME, name,
                                      BBUFSIZ, &len)) {
            if (strstartswith(name, L"Service-")) {
                if (GetUserObjectInformationW(ws, UOI_FLAGS, &uf,
                                              DSIZEOF(uf), &len)) {
                    if (uf.dwFlags != WSF_VISIBLE)
                        rv = 1;
#if defined(_DBGVIEW)
                    else
                        OutputDebugStringW(name);
#endif
                }
            }
        }
    }
    return rv;
}
#endif

static void setsvcstatusexit(DWORD e)
{
    EnterCriticalSection(&servicelock);
    ssvcstatus.dwServiceSpecificExitCode = e;
    LeaveCriticalSection(&servicelock);
}

static void reportsvcstatus(DWORD status, DWORD param)
{
    static DWORD cpcnt = 1;

    EnterCriticalSection(&servicelock);
    if (InterlockedExchange(&sscstate, SERVICE_STOPPED) == SERVICE_STOPPED)
        goto finished;
    ssvcstatus.dwControlsAccepted = 0;
    ssvcstatus.dwCheckPoint       = 0;
    ssvcstatus.dwWaitHint         = 0;

    if ((status == SERVICE_RUNNING) || (status == SERVICE_PAUSED)) {
        ssvcstatus.dwControlsAccepted =  SERVICE_ACCEPT_STOP |
                                         SERVICE_ACCEPT_SHUTDOWN |
                                         SERVICE_ACCEPT_PAUSE_CONTINUE;
#if defined(SERVICE_ACCEPT_PRESHUTDOWN)
        ssvcstatus.dwControlsAccepted |= SERVICE_ACCEPT_PRESHUTDOWN;
#endif
        cpcnt = 1;
    }
    else if (status == SERVICE_STOPPED) {
        if (param != 0)
            ssvcstatus.dwServiceSpecificExitCode = param;
        if (ssvcstatus.dwServiceSpecificExitCode == 0 &&
            ssvcstatus.dwCurrentState != SERVICE_STOP_PENDING)
            ssvcstatus.dwServiceSpecificExitCode = ERROR_PROCESS_ABORTED;
        if (ssvcstatus.dwServiceSpecificExitCode != 0)
            ssvcstatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }
    else {
        ssvcstatus.dwCheckPoint = cpcnt++;
        ssvcstatus.dwWaitHint   = param;
    }
    ssvcstatus.dwCurrentState = status;
    if (nonsvcmode || SetServiceStatus(hsvcstatus, &ssvcstatus))
        InterlockedExchange(&sscstate, status);
    else
        svcsyserror(__LINE__, GetLastError(), L"SetServiceStatus");
finished:
    LeaveCriticalSection(&servicelock);
}

static DWORD createiopipes(LPSTARTUPINFOW si)
{
    SECURITY_ATTRIBUTES sa;
    DWORD  rc = 0;
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
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
    if (!DuplicateHandle(cp, sh, cp,
                         &inputpipewrs, FALSE, 0,
                         DUPLICATE_SAME_ACCESS)) {
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle");
        goto finished;
    }
    SAFE_CLOSE_HANDLE(sh);

    /**
     * Create stdout/stderr pipe, with read side
     * of the pipe as non inheritable
     */
    if (!CreatePipe(&sh, &(si->hStdError), &sa, 0))
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
    if (!DuplicateHandle(cp, sh, cp,
                         &outputpiperd, FALSE, 0,
                         DUPLICATE_SAME_ACCESS))
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle");
    si->hStdOutput = si->hStdError;

finished:
    SAFE_CLOSE_HANDLE(sh);
    return rc;
}

static DWORD logappend(HANDLE h, LPCVOID buf, DWORD len)
{
    DWORD wr, rc = 0;
    LARGE_INTEGER ee = {{ 0, 0 }};

    if (!SetFilePointerEx(h, ee, NULL, FILE_END)) {
        return GetLastError();
    }
    if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0)) {
        if (InterlockedAdd(&logwritten, wr) >= SVCBATCH_LOGFLUSH_SIZE) {
            FlushFileBuffers(h);
            InterlockedExchange(&logwritten, 0);
        }
    }
    else {
        rc = GetLastError();
    }
    if (rc != 0) {
        dbgprintf(__FUNCTION__, "wrote zero bytes (0x%08x)", rc);
    }
    return rc;
}

static void logfflush(HANDLE h)
{
    LARGE_INTEGER ee = {{ 0, 0 }};

    if (SetFilePointerEx(h, ee, NULL, FILE_END)) {
        DWORD wr;

        WriteFile(h, CRLFA, 2, &wr, NULL);
        FlushFileBuffers(h);
        InterlockedExchange(&logwritten, 0);
    }
}

static void logwrline(HANDLE h, const char *str)
{
    char    buf[BBUFSIZ];
    ULONGLONG ct;
    int     dd, hh, mm, ss, ms;
    DWORD   w;

    ct = GetTickCount64() - logtickcount;
    ms = (int)((ct % MS_IN_SECOND));
    ss = (int)((ct / MS_IN_SECOND) % 60);
    mm = (int)((ct / MS_IN_MINUTE) % 60);
    hh = (int)((ct / MS_IN_HOUR)   % 24);
    dd = (int)((ct / MS_IN_DAY));

    memset(buf, 0, sizeof(buf));
    sprintf(buf,
            "[%.2d:%.2d:%.2d:%.2d.%.3d] [%.4lu:%.4lu] ",
            dd, hh, mm, ss, ms,
            GetCurrentProcessId(),
            GetCurrentThreadId());
    buf[BBUFSIZ - 1] = '\0';
    WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL);
    InterlockedAdd(&logwritten, w);
    WriteFile(h, str, (DWORD)strlen(str), &w, NULL);
    InterlockedAdd(&logwritten, w);
    WriteFile(h, CRLFA, 2, &w, NULL);
    InterlockedAdd(&logwritten, w);
}

static void logprintf(HANDLE h, const char *format, ...)
{
    char    buf[MBUFSIZ];
    va_list ap;

    memset(buf, 0, sizeof(buf));
    va_start(ap, format);
    _vsnprintf(buf, MBUFSIZ - 1, format, ap);
    va_end(ap);
    logwrline(h, buf);
}

static void logwrtime(HANDLE h, const char *hdr)
{
    SYSTEMTIME tt;

    GetSystemTime(&tt);
    logprintf(h, "%-16s : %.4d-%.2d-%.2d %.2d:%.2d:%.2d",
              hdr,
              tt.wYear, tt.wMonth, tt.wDay,
              tt.wHour, tt.wMinute, tt.wSecond);
}

static void logconfig(HANDLE h)
{
    wchar_t *fs = NULL;

    logprintf(h, "Service name     : %S", servicename);
    logprintf(h, "Service uuid     : %S", serviceuuid);
    logprintf(h, "Batch file       : %S", svcbatchfile);
    logprintf(h, "Base directory   : %S", servicebase);
    logprintf(h, "Working directory: %S", servicehome);
    logprintf(h, "Log directory    : %S", loglocation);
    if (autorotate)
        fs = xwcsappend(fs, L"autorotate, ");
    if (consolemode)
        fs = xwcsappend(fs, L"console mode, ");
    if (runbatchmode)
        fs = xwcsappend(fs, L"runbatchmode mode, ");
    if (hasctrlbreak)
        fs = xwcsappend(fs, L"ctrl+break, ");
    if (usecleanpath)
        fs = xwcsappend(fs, L"clean path, ");
    if (usesafeenv)
        fs = xwcsappend(fs, L"safe environment, ");

    if (fs != NULL) {
        int i = xwcslen(fs);
        fs[i - 2] = L'\0';
        logprintf(h, "Features         : %S", fs);
        xfree(fs);
    }
    logfflush(h);
}

static DWORD openlogfile(BOOL firstopen)
{
    wchar_t  sfx[24];
    wchar_t *logpb = NULL;
    DWORD rc = 0;
    HANDLE h = NULL;
    WIN32_FILE_ATTRIBUTE_DATA ad;
    int i;

    logtickcount = GetTickCount64();

    if (logfilename == NULL) {
        if (!CreateDirectoryW(loglocation, 0)) {
            rc = GetLastError();
            if (rc != ERROR_ALREADY_EXISTS) {
                dbgprintf(__FUNCTION__,
                          "cannot create logdir: %S",
                          loglocation);
                return rc;
            }
        }
        if (isrelativepath(loglocation)) {
            wchar_t *n  = loglocation;
            loglocation = getrealpathname(n, 1);
            if (loglocation == NULL)
                return ERROR_PATH_NOT_FOUND;
            xfree(n);
        }
        if (_wcsicmp(loglocation, servicehome) == 0) {
            dbgprintf(__FUNCTION__,
                      "loglocation cannot be the same as servicehome");
            return ERROR_BAD_PATHNAME;
        }
        logfilename = xwcsvarcat(loglocation, L"\\", wnbatchapp, L".log", NULL);
    }
#if defined(_DBGVIEW_SAVE)
    EnterCriticalSection(&dbgviewlock);
    h = InterlockedExchangePointer(&dbgfhandle, NULL);
    if (h == NULL) {
        wchar_t *dn;

        dn = xwcsvarcat(loglocation, L"\\", wnbatchapp, L".dbg", NULL);

        h  = CreateFileW(dn, GENERIC_WRITE,
                         FILE_SHARE_READ, &sazero, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
        if (IS_INVALID_HANDLE(h)) {
            h = NULL;
            dbgprintf(__FUNCTION__, "failed to create %S", dn);
        }
        else {
            SYSTEMTIME tt;
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                DWORD wr;
                LARGE_INTEGER ee = {{ 0, 0 }};

                SetFilePointerEx(h, ee, NULL, FILE_END);
                WriteFile(h, CRLFA, 4, &wr, NULL);
            }

            GetSystemTime(&tt);
            InterlockedExchangePointer(&dbgfhandle, h);
            dbgprintf(__FUNCTION__, "%.4d-%.2d-%.2d %.2d:%.2d:%.2d",
                      tt.wYear, tt.wMonth, tt.wDay,
                      tt.wHour, tt.wMinute, tt.wSecond);
            dbgprintf(__FUNCTION__, "tracing %S to %S", servicename, dn);
        }
        xfree(dn);
    }
    else {
        /**
         * Already opened
         */
        InterlockedExchangePointer(&dbgfhandle, h);
    }
    LeaveCriticalSection(&dbgviewlock);
#endif
    memset(sfx, 0, sizeof(sfx));
    if (GetFileAttributesExW(logfilename, GetFileExInfoStandard, &ad)) {
        DWORD mm;

        if (firstopen)
            reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
        if (autorotate) {
            SYSTEMTIME st;

            FileTimeToSystemTime(&ad.ftLastWriteTime, &st);
            _snwprintf(sfx, 20, L".%.4d-%.2d-%.2d.%.2d%.2d%.2d",
                       st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond);
            mm = 0;
        }
        else {
            mm = MOVEFILE_REPLACE_EXISTING;
            sfx[0] = L'.';
            sfx[1] = L'0';
        }
        logpb = xwcsconcat(logfilename, sfx);
        if (!MoveFileExW(logfilename, logpb, mm)) {
            rc = GetLastError();
            xfree(logpb);
            return svcsyserror(__LINE__, rc, L"MoveFileExW");
        }
    }
    else {
        rc = GetLastError();
        if (rc != ERROR_FILE_NOT_FOUND) {
            dbgprintf(__FUNCTION__, "%lu %S", rc, logfilename);
            return svcsyserror(__LINE__, rc, L"GetFileAttributesExW");
        }
    }

    if (firstopen)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    if (autorotate == 0) {
        /**
         * Rotate previous log files
         */
        for (i = SVCBATCH_MAX_LOGS; i > 0; i--) {
            wchar_t *logpn;

            sfx[1] = L'0' + i - 1;
            logpn  = xwcsconcat(logfilename, sfx);

            if (GetFileAttributesW(logpn) != INVALID_FILE_ATTRIBUTES) {
                wchar_t *lognn;

                sfx[1] = L'0' + i;
                lognn = xwcsconcat(logfilename, sfx);
                if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING)) {
                    rc = GetLastError();
                    svcsyserror(__LINE__, rc, L"MoveFileExW");
                    xfree(logpn);
                    xfree(lognn);
                    goto failed;
                }
                xfree(lognn);
                if (firstopen)
                    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
            }
            xfree(logpn);
        }
    }
    h = CreateFileW(logfilename, GENERIC_WRITE,
                    FILE_SHARE_READ, &sazero, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);

    if (IS_INVALID_HANDLE(h)) {
        rc = GetLastError();
        goto failed;
    }
    xfree(logpb);
    InterlockedExchange(&logwritten, 0);
    logwrline(h, cnamestamp);

    if (firstopen)
        logwrtime(h, "Log opened");
    InterlockedExchangePointer(&logfhandle, h);
    return 0;

failed:
    SAFE_CLOSE_HANDLE(h);
    if (logpb != NULL) {
        MoveFileExW(logpb, logfilename, MOVEFILE_REPLACE_EXISTING);
        xfree(logpb);
    }
    return rc;
}

static DWORD rotatelogs(void)
{
    DWORD  rc;
    HANDLE h = NULL;

    EnterCriticalSection(&logfilelock);
    h =  InterlockedExchangePointer(&logfhandle, NULL);
    if (h == NULL) {
        LeaveCriticalSection(&logfilelock);
        return ERROR_FILE_NOT_FOUND;
    }
    logfflush(h);
    logwrtime(h, "Log rotatation initialized");
    FlushFileBuffers(h);
    CloseHandle(h);
    rc = openlogfile(FALSE);
    if (rc == 0) {
        logwrtime(logfhandle, "Log rotated");
        logprintf(logfhandle, "Log generation   : %lu",
                  InterlockedIncrement(&rotatecount));
        logconfig(logfhandle);
    }
    else {
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"rotatelogs");
    }
    LeaveCriticalSection(&logfilelock);
    return rc;
}

static void closelogfile(void)
{
    HANDLE h;

    EnterCriticalSection(&logfilelock);
    h = InterlockedExchangePointer(&logfhandle, NULL);
    if (h != NULL) {
        logfflush(h);
        logwrtime(h, "Log closed");
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    LeaveCriticalSection(&logfilelock);
}

static void rotatesynctime(void)
{
    if ((rotatedev != NULL) && (rotateint != ONE_DAY)) {
        CancelWaitableTimer(rotatedev);
        SetWaitableTimer(rotatedev, &rotatetmo, 0, NULL, NULL, 0);
    }
}

static int resolverotate(const wchar_t *str)
{
    wchar_t   *rp, *sp;

    rotatetmo.QuadPart = rotateint;
    if (IS_EMPTY_WCS(str)) {
        dbgprintf(__FUNCTION__, "rotate in 30 days");
        return 0;
    }
    rp = sp = xwcsdup(str);
    if (*rp == L'@') {
        int      hh, mm, ss;
        wchar_t *p;

        rp++;
        p = wcschr(rp, L':');
        if (p == NULL) {
            if ((p = wcschr(rp, L'~')) != NULL)
                *(p++) = L'\0';
            mm = _wtoi(rp);
            rp = p;
            if (mm < SVCBATCH_MIN_LOGRTIME)
                return __LINE__;
            rotateint = mm * ONE_MINUTE * CPP_INT64_C(-1);
            dbgprintf(__FUNCTION__, "rotate in %d minutes", mm);
            rotatetmo.QuadPart = rotateint;
        }
        else {
            SYSTEMTIME     st;
            FILETIME       ft;
            ULARGE_INTEGER ui;

            *(p++) = L'\0';
            hh = _wtoi(rp);
            rp = p;
            if ((p = wcschr(rp, L':')) == NULL)
                return __LINE__;
            *(p++) = L'\0';
            mm = _wtoi(rp);
            rp = p;
            if ((p = wcschr(rp, L'~')) != NULL)
                *(p++) = L'\0';
            ss = _wtoi(rp);
            rp = p;
            if ((hh > 23) || (mm > 59) || (ss > 59))
                return __LINE__;
            rotateint    = ONE_DAY;
            GetSystemTime(&st);
            SystemTimeToFileTime(&st, &ft);
            ui.HighPart = ft.dwHighDateTime;
            ui.LowPart  = ft.dwLowDateTime;
            ui.QuadPart += rotateint;
            ft.dwHighDateTime = ui.HighPart;
            ft.dwLowDateTime  = ui.LowPart;
            FileTimeToSystemTime(&ft, &st);
            st.wHour   = hh;
            st.wMinute = mm;
            st.wSecond = ss;
            SystemTimeToFileTime(&st, &ft);
            rotatetmo.HighPart = ft.dwHighDateTime;
            rotatetmo.LowPart  = ft.dwLowDateTime;
            dbgprintf(__FUNCTION__, "rotate at %.4d-%.2d-%.2d %.2d:%.2d:%.2d",
                      st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond);
        }
    }
    if (rp != NULL) {
        LONGLONG siz;
        LONGLONG mux = CPP_INT64_C(1);
        wchar_t *ep  = NULL;

        siz = _wcstoi64(rp, &ep, 10);
        if (siz < mux)
            return __LINE__;
        if (IS_EMPTY_WCS(ep)) {
            dbgprintf(__FUNCTION__, "rotate if > %lu bytes", (DWORD)siz);
        }
        else {
            wchar_t mm = towupper(ep[0]);
            if (ep[1] != '\0')
                return __LINE__;
            switch (mm) {
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
            dbgprintf(__FUNCTION__, "rotate if > %lu %Cb", (DWORD)siz, mm);
        }
        rotatesiz.QuadPart = siz * mux;
        if (rotatesiz.QuadPart < SVCBATCH_MIN_LOGSIZE)
            return __LINE__;
    }
    xfree(sp);
    return 0;
}

static unsigned int __stdcall stopthread(void *unused)
{
    const char yn[2] = { 'Y', '\n'};
    DWORD  wr, ws, wn = SVCBATCH_PENDING_INIT;
    HANDLE h = NULL;
    int   i;

    if (InterlockedIncrement(&sstarted) > 1) {
        dbgprints(__FUNCTION__, "already started");
        XENDTHREAD(0);
    }
    ResetEvent(svcstopended);

    dbgprintf(__FUNCTION__, "started");
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

    EnterCriticalSection(&logfilelock);
    h = InterlockedExchangePointer(&logfhandle, NULL);
    if (h != NULL) {
        logfflush(h);
        logwrline(h, "Service STOP signaled\r\n");
    }
    InterlockedExchangePointer(&logfhandle, h);
    LeaveCriticalSection(&logfilelock);

    dbgprintf(__FUNCTION__, "raising CTRL_C_EVENT");
    if (SetConsoleCtrlHandler(NULL, TRUE)) {
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        ws = WaitForSingleObject(processended, SVCBATCH_PENDING_INIT);
        SetConsoleCtrlHandler(NULL, FALSE);
        if (ws == WAIT_OBJECT_0) {
            dbgprintf(__FUNCTION__, "processended by CTRL_C_EVENT");
            goto finished;
        }
    }
#if defined(_DBGVIEW)
    else {
        dbgprintf(__FUNCTION__, "SetConsoleCtrlHandler failed %lu", GetLastError());
    }
#endif
    for (i = 0; i < 10; i++) {
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

        ws = WaitForSingleObject(processended, wn);
        if (ws == WAIT_OBJECT_0) {
            dbgprintf(__FUNCTION__, "processended signaled");
            goto finished;
        }
        else if (ws == WAIT_TIMEOUT) {
            dbgprintf(__FUNCTION__, "sending Y to child (attempt=%d)", i + 1);
            /**
             * Write Y to stdin pipe in case cmd.exe waits for
             * user reply to "Terminate batch job (Y/N)?"
             */
            WriteFile(inputpipewrs, yn, 2, &wr, NULL);
            FlushFileBuffers(inputpipewrs);
        }
        else {
            dbgprintf(__FUNCTION__, "WaitForSingleObject failed %lu", GetLastError());
            break;
        }
        wn = SVCBATCH_PENDING_WAIT;
    }
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
    dbgprintf(__FUNCTION__, "wait for processended signal");
    /**
     * Wait for main process to finish or times out.
     *
     * We are waiting at most for SVCBATCH_STOP_HINT
     * timeout and then kill all child processes.
     */
    ws = WaitForSingleObject(processended, SVCBATCH_STOP_HINT / 2);
    if (ws == WAIT_TIMEOUT) {
        dbgprintf(__FUNCTION__, "Child is still active, terminating");
        /**
         * WAIT_TIMEOUT means that child is
         * still running and we need to terminate
         * child tree by brute force
         */
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
        SAFE_CLOSE_HANDLE(childprocess);
        SAFE_CLOSE_HANDLE(childprocjob);
    }

finished:
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

    dbgprints(__FUNCTION__, "done");
    SetEvent(svcstopended);
    XENDTHREAD(0);
}

static unsigned int __stdcall iopipethread(void *unused)
{
    DWORD  rc = 0;

    dbgprints(__FUNCTION__, "started");
    while (rc == 0) {
        BYTE  rb[HBUFSIZ];
        DWORD rd = 0;
        HANDLE h = NULL;

        if (InterlockedExchange(&svcworking, 1) == 0) {
            HANDLE wh[2];

            wh[0] = workingevent;
            wh[1] = processended;

            dbgprints(__FUNCTION__, "waiting for continue...");
            if (WaitForMultipleObjects(2, wh, FALSE, INFINITE) != WAIT_OBJECT_0) {
                dbgprints(__FUNCTION__, "ended");
                XENDTHREAD(0);
            }
            dbgprints(__FUNCTION__, "resuming");
        }
        if (ReadFile(outputpiperd, rb, HBUFSIZ, &rd, NULL)) {
            if (rd == 0) {
                /**
                 * Read zero bytes from child should
                 * not happen.
                 */
                dbgprintf(__FUNCTION__, "Read 0 bytes err=%lu", GetLastError());
                rc = ERROR_NO_DATA;
            }
            else {
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);

                if (h != NULL)
                    rc = logappend(h, rb, rd);
                else
                    rc = ERROR_NO_MORE_FILES;

                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
            }
        }
        else {
            /**
             * Read from child failed.
             * ERROR_BROKEN_PIPE means that
             * child process closed its side of the pipe.
             */
            rc = GetLastError();
        }
    }
#if defined(_DBGVIEW)
    if (rc == ERROR_BROKEN_PIPE)
        dbgprints(__FUNCTION__, "pipe closed");
    else if (rc == ERROR_NO_MORE_FILES)
        dbgprints(__FUNCTION__, "logfile closed");
    else if (rc != ERROR_NO_DATA)
        dbgprintf(__FUNCTION__, "err=%lu", rc);
    dbgprints(__FUNCTION__, "done");
#endif

    XENDTHREAD(0);
}

static unsigned int __stdcall monitorthread(void *unused)
{
    HANDLE wh[2];
    DWORD  wc, rc = 0;

    dbgprints(__FUNCTION__, "started");

    wh[0] = monitorevent;
    wh[1] = processended;
    do {
        LONG cc;

        wc = WaitForMultipleObjects(2, wh, FALSE, INFINITE);
        switch (wc) {
            case WAIT_OBJECT_0:
                cc = InterlockedExchange(&monitorsig, 0);
                if (cc == 0) {
                    dbgprints(__FUNCTION__, "quit signaled");
                    rc = ERROR_WAIT_NO_CHILDREN;
                    break;
                }
                else if (cc == SVCBATCH_CTRL_BREAK) {
                    HANDLE h;

                    dbgprints(__FUNCTION__, "break signaled");
                    EnterCriticalSection(&logfilelock);
                    h = InterlockedExchangePointer(&logfhandle, NULL);

                    if (h != NULL) {
                        logfflush(h);
                        logwrline(h, "CTRL_BREAK_EVENT signaled\r\n");
                    }
                    InterlockedExchangePointer(&logfhandle, h);
                    LeaveCriticalSection(&logfilelock);
                    /**
                     * Danger Zone!!!
                     *
                     * Send CTRL_BREAK_EVENT to the child process.
                     * This is useful if batch file is running java
                     * CTRL_BREAK signal tells JDK to dump thread stack
                     *
                     * In case subchild does not handle CTRL_BREAK cmd.exe
                     * will probably block on "Terminate batch job (Y/N)?"
                     */
                    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
                }
                else if (cc == SVCBATCH_CTRL_ROTATE) {
                    dbgprints(__FUNCTION__, "log rotation signaled");
                    rc = rotatelogs();
                    if (rc != 0) {
                        setsvcstatusexit(rc);
                        xcreatethread(1, 0, &stopthread);
                        break;
                    }
                    rotatesynctime();
                }
                else if (cc == SERVICE_CONTROL_PAUSE) {
                    dbgprints(__FUNCTION__, "pause signaled");
                    InterlockedExchange(&svcworking, 0);
                    reportsvcstatus(SERVICE_PAUSED, 0);
                }
                else if (cc == SERVICE_CONTROL_CONTINUE) {
                    dbgprints(__FUNCTION__, "continue signaled");
                    InterlockedExchange(&svcworking, 1);
                    SetEvent(workingevent);
                    reportsvcstatus(SERVICE_RUNNING, 0);
                }
#if defined(_DBGVIEW)
                else {
                    dbgprintf(__FUNCTION__, "Unknown control: %lu", (DWORD)cc);
                }
#endif
                if (rc == 0)
                    ResetEvent(monitorevent);
            break;
            case WAIT_OBJECT_1:
                dbgprints(__FUNCTION__, "processended signaled");
                rc = ERROR_WAIT_NO_CHILDREN;
            break;
            default:
            break;
        }
    } while (rc == 0);
#if defined(_DBGVIEW)
    if ((rc != 0) && (rc != ERROR_WAIT_NO_CHILDREN))
        dbgprintf(__FUNCTION__, "log rotation failed: %lu", rc);
    dbgprints(__FUNCTION__, "done");
#endif
    XENDTHREAD(0);
}

static unsigned int __stdcall rotatethread(void *unused)
{
    HANDLE wh[2];
    HANDLE h = NULL;
    DWORD  wc, ms, rc = 0;

    dbgprintf(__FUNCTION__, "started");
    wc = WaitForSingleObject(processended, SVCBATCH_LOGROTATE_INI);
    if (wc != WAIT_TIMEOUT) {
#if defined(_DBGVIEW)
        if (wc == WAIT_OBJECT_0)
            dbgprints(__FUNCTION__, "processended signaled");
        else
            dbgprintf(__FUNCTION__, "processended %lu", wc);
#endif
        goto finished;
    }
    dbgprints(__FUNCTION__, "running");

    if (rotatesiz.QuadPart)
        ms = SVCBATCH_LOGROTATE_HINT;
    else
        ms = INFINITE;

    wh[0] = rotatedev;
    wh[1] = processended;
    while (rc == 0) {
        wc = WaitForMultipleObjects(2, wh, FALSE, ms);
        switch (wc) {
            case WAIT_TIMEOUT:
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h == NULL) {
                    rc = ERROR_NO_MORE_FILES;
                }
                else {
                    LARGE_INTEGER fs;
                    if (GetFileSizeEx(h, &fs)) {
                        InterlockedExchangePointer(&logfhandle, h);
                        if (fs.QuadPart >= rotatesiz.QuadPart) {
                            dbgprintf(__FUNCTION__, "rotate by size");
                            rc = rotatelogs();
                            if (rc != 0) {
                                setsvcstatusexit(rc);
                                xcreatethread(1, 0, &stopthread);
                            }
                            else {
                                rotatesynctime();
                            }
                        }
                        else {
                            FlushFileBuffers(logfhandle);
                            InterlockedExchange(&logwritten, 0);
                        }
                    }
                    else {
                        rc = GetLastError();
                        CloseHandle(h);
                        setsvcstatusexit(rc);
                        svcsyserror(__LINE__, rc, L"GetFileSizeEx");
                        xcreatethread(1, 0, &stopthread);
                    }
                }
                LeaveCriticalSection(&logfilelock);
            break;
            case WAIT_OBJECT_0:
                dbgprints(__FUNCTION__, "rotate by time");
                rc = rotatelogs();
                if (rc == 0) {
                    CancelWaitableTimer(wh[0]);
                    if (rotateint == ONE_DAY)
                        rotatetmo.QuadPart += rotateint;
                    SetWaitableTimer(wh[0], &rotatetmo, 0, NULL, NULL, 0);
                }
                if (rc != 0) {
                    setsvcstatusexit(rc);
                    xcreatethread(1, 0, &stopthread);
                }
            break;
            case WAIT_OBJECT_1:
                rc = ERROR_PROCESS_ABORTED;
                dbgprints(__FUNCTION__, "processended signaled");
            break;
            case WAIT_FAILED:
                rc = GetLastError();
                dbgprintf(__FUNCTION__, "wait failed %lu", rc);
            break;
            default:
                rc = wc;
                dbgprintf(__FUNCTION__, "wait error %lu", rc);
            break;
        }
    }

finished:
    dbgprints(__FUNCTION__, "done");
    SAFE_CLOSE_HANDLE(rotatedev);
    XENDTHREAD(0);
}

static unsigned int __stdcall runexecthread(void *unused)
{
    wchar_t *cmdline;
    HANDLE   wh[2];
    DWORD    rc;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;

    ResetEvent(svcexecended);

    dbgprints(__FUNCTION__, "started");

    cmdline = xappendarg(NULL, svcbatchexe);
    cmdline = xwcsappend(cmdline, L" -n ");
    cmdline = xappendarg(cmdline, servicename);
    cmdline = xwcsappend(cmdline, L" -w ");
    cmdline = xappendarg(cmdline, servicehome);
    cmdline = xwcsappend(cmdline, L" -o ");
    cmdline = xappendarg(cmdline, loglocation);
    cmdline = xwcsappend(cmdline, L" -u ");
    cmdline = xappendarg(cmdline, serviceuuid);
    cmdline = xwcsappend(cmdline, L" -d ");
    cmdline = xappendarg(cmdline, serviceexec);

    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);

    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb = DSIZEOF(STARTUPINFOW);

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                        NULL,
                        servicehome,
                       &si, &cp)) {
        rc = GetLastError();
        svcsyserror(__LINE__, rc, L"CreateProcess");
        goto finished;
    }
    dbgprintf(__FUNCTION__, "exec pid %lu", cp.dwProcessId);
    CloseHandle(cp.hThread);
    wh[0] = cp.hProcess;
    wh[1] = processended;

    rc = WaitForMultipleObjects(2, wh, FALSE, INFINITE);
    switch (rc) {
        case WAIT_OBJECT_0:
            dbgprints(__FUNCTION__, "exec process done");
        break;
        case WAIT_OBJECT_1:
            dbgprints(__FUNCTION__, "processended signaled");
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, cp.dwProcessId);
            if (WaitForSingleObject(cp.hProcess, SVCBATCH_STOP_HINT) == WAIT_TIMEOUT) {
                dbgprintf(__FUNCTION__, "Terminating exec child %lu", cp.dwProcessId);
                TerminateProcess(cp.hProcess, ERROR_BROKEN_PIPE);
            }
            else {
                dbgprintf(__FUNCTION__, "Exec child ended %lu", cp.dwProcessId);
            }
        break;
        case WAIT_FAILED:
            rc = GetLastError();
            dbgprintf(__FUNCTION__, "wait failed %lu", rc);
        break;
        default:
            dbgprintf(__FUNCTION__, "wait error %lu", rc);
        break;


    }
    CloseHandle(cp.hProcess);

finished:
    xfree(cmdline);
    dbgprints(__FUNCTION__, "done");
    SetEvent(svcexecended);
    InterlockedExchange(&rstarted, 0L);
    XENDTHREAD(0);
}

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    HANDLE h = NULL;
    const char *msg = "(unknown)";

    switch(ctrl) {
        case CTRL_CLOSE_EVENT:
            msg = "CTRL_CLOSE_EVENT signaled";
        break;
        case CTRL_SHUTDOWN_EVENT:
            msg = "CTRL_SHUTDOWN_EVENT signaled";
        break;
        case CTRL_C_EVENT:
            msg = "CTRL_C_EVENT signaled";
        break;
        case CTRL_BREAK_EVENT:
            msg = "CTRL_BREAK_EVENT signaled";
        break;
        case CTRL_LOGOFF_EVENT:
            msg = "CTRL_LOGOFF_EVENT signaled";
        break;
    }
    switch(ctrl) {
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
#if defined(_DBGVIEW)
            dbgprints(__FUNCTION__, msg);
#endif
            if (nonsvcmode) {
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h != NULL) {
                    logfflush(h);
                    logwrline(h, msg);
                }
                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
                reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
                xcreatethread(1, 0, &stopthread);
            }
        break;
        case CTRL_LOGOFF_EVENT:
            dbgprints(__FUNCTION__, msg);
        break;
        default:
            dbgprintf(__FUNCTION__, "unknown ctrl %lu", ctrl);
            return FALSE;
        break;
    }
    return TRUE;
}

static DWORD WINAPI servicehandler(DWORD ctrl, DWORD _xe, LPVOID _xd, LPVOID _xc)
{
    HANDLE h = NULL;

    switch(ctrl) {
        case SERVICE_CONTROL_SHUTDOWN:
#if defined(SERVICE_CONTROL_PRESHUTDOWN)
        case SERVICE_CONTROL_PRESHUTDOWN:
#endif
            EnterCriticalSection(&logfilelock);
            h = InterlockedExchangePointer(&logfhandle, NULL);
            if (h != NULL) {
                logfflush(h);
                logprintf(h, "Service SHUTDOWN (0x%08x) signaled", ctrl);
            }
            InterlockedExchangePointer(&logfhandle, h);
            LeaveCriticalSection(&logfilelock);
        case SERVICE_CONTROL_STOP:
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
            xcreatethread(1, 0, &stopthread);
        break;
        case SVCBATCH_CTRL_BREAK:
            if (hasctrlbreak == 0)
                return ERROR_CALL_NOT_IMPLEMENTED;
        case SVCBATCH_CTRL_ROTATE:
            /**
             * Signal to monitorthread that
             * user send custom service control
             */
            InterlockedExchange(&monitorsig, ctrl);
            SetEvent(monitorevent);
        break;
        case SVCBATCH_CTRL_EXEC:
            if (IS_EMPTY_WCS(svcbatchexe)) {
                dbgprints(__FUNCTION__, "SvcBatch exec undefined");
            }
            else {
                dbgprintf(__FUNCTION__, "Starting exec:  %S", svcbatchexe);
                if (InterlockedIncrement(&rstarted) > 1) {
                    dbgprints(__FUNCTION__, "already running");
                    return ERROR_ALREADY_EXISTS;
                }
                xcreatethread(1, 0, &runexecthread);
            }
        break;
        case SERVICE_CONTROL_PAUSE:
            /**
             * Signal to monitorthread that
             * service should enter pause state.
             */
            reportsvcstatus(SERVICE_PAUSE_PENDING, SVCBATCH_PENDING_WAIT);
            InterlockedExchange(&monitorsig, ctrl);
            SetEvent(monitorevent);
        break;
        case SERVICE_CONTROL_CONTINUE:
            /**
             * Signal to monitorthread that
             * service should continue reading from child.
             */
            reportsvcstatus(SERVICE_CONTINUE_PENDING, SVCBATCH_PENDING_WAIT);
            InterlockedExchange(&monitorsig, ctrl);
            SetEvent(monitorevent);
        break;
        case SERVICE_CONTROL_INTERROGATE:
        break;
        default:
            dbgprintf(__FUNCTION__, "unknown ctrl %lu", ctrl);
            return ERROR_CALL_NOT_IMPLEMENTED;
        break;
    }
    return 0;
}

static unsigned int __stdcall workerthread(void *unused)
{
    wchar_t *cmdline;
    HANDLE   wh[2];
    DWORD    rc;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;

    dbgprints(__FUNCTION__, "started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    cmdline = xappendarg(NULL, comspec);
    cmdline = xwcsappend(cmdline, L" /D /C \"");
    cmdline = xappendarg(cmdline, svcbatchfile);
    cmdline = xwcsappend(cmdline, L"\"");
    dbgprintf(__FUNCTION__, "comspec %S", comspec);
    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);

    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    rotatedev  = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (IS_INVALID_HANDLE(rotatedev)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"CreateWaitableTimer");
        goto finished;
    }
    if (!SetWaitableTimer(rotatedev, &rotatetmo, 0, NULL, NULL, 0)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"SetWaitableTimer");
        goto finished;
    }

    rc = createiopipes(&si);
    if (rc != 0) {
        setsvcstatusexit(rc);
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
        svcsyserror(__LINE__, rc, L"SetInformationJobObject");
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
        svcsyserror(__LINE__, rc, L"CreateProcess");
        goto finished;
    }
    childprocess = cp.hProcess;
    dbgprintf(__FUNCTION__, "child pid %lu", cp.dwProcessId);
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    if (!AssignProcessToJobObject(childprocjob, childprocess)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"AssignProcessToJobObject");
        TerminateProcess(childprocess, rc);
        goto finished;
    }
    wh[0] = childprocess;
    wh[1] = xcreatethread(0, CREATE_SUSPENDED, &iopipethread);
    if (IS_INVALID_HANDLE(wh[1])) {
        rc = ERROR_TOO_MANY_TCBS;
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"iopipethread");
        TerminateProcess(childprocess, ERROR_OUTOFMEMORY);
        goto finished;
    }

    ResumeThread(cp.hThread);
    ResumeThread(wh[1]);
    SAFE_CLOSE_HANDLE(cp.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    dbgprints(__FUNCTION__, "service running");
    if (runbatchmode == 0)
        xcreatethread(1, 0, &rotatethread);
    WaitForMultipleObjects(2, wh, TRUE, INFINITE);
    CloseHandle(wh[1]);

finished:
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    SetEvent(processended);
#if defined(_DBGVIEW)
    if (ssvcstatus.dwServiceSpecificExitCode)
        dbgprintf(__FUNCTION__, "ServiceSpecificExitCode=%lu",
                  ssvcstatus.dwServiceSpecificExitCode);
    dbgprints(__FUNCTION__, "done");
#endif

    XENDTHREAD(0);
}

static void WINAPI servicemain(DWORD argc, wchar_t **argv)
{
    int          i;
    DWORD        rv = 0;
    int          eblen = 0;
    wchar_t     *ep;
    HANDLE       wh[2] = { NULL, NULL };
    DWORD        wc = 1;

    ssvcstatus.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    ssvcstatus.dwCurrentState = SERVICE_START_PENDING;

    if (IS_EMPTY_WCS(servicename))
        servicename = xwcsdup(argv[0]);
    if (IS_EMPTY_WCS(servicename)) {
        svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, L"Missing servicename");
        exit(ERROR_INVALID_PARAMETER);
        return;
    }
    if (runbatchmode == 0) {
        hsvcstatus  = RegisterServiceCtrlHandlerExW(servicename, servicehandler, NULL);
        if (IS_INVALID_HANDLE(hsvcstatus)) {
            svcsyserror(__LINE__, GetLastError(), L"RegisterServiceCtrlHandlerEx");
            exit(ERROR_INVALID_HANDLE);
            return;
        }
    }
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    dbgprintf(__FUNCTION__, "started %S", servicename);
    rv = openlogfile(TRUE);
    if (rv != 0) {
        svcsyserror(__LINE__, rv, L"openlogfile");
        reportsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    logconfig(logfhandle);
    SetConsoleCtrlHandler(consolehandler, TRUE);
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    /**
     * Add additional environment variables
     * They are unique to this service instance
     */
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_BASE=",   servicebase);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_HOME=",   servicehome);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_LOGDIR=", loglocation);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_NAME=",   servicename);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_SELF=",   svcbatchexe);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_UUID=",   serviceuuid);

    qsort((void *)dupwenvp, dupwenvc, sizeof(wchar_t *), envsort);
    for (i = 0; i < dupwenvc; i++) {
        eblen += xwcslen(dupwenvp[i]) + 1;
    }
    wenvblock = xwalloc(eblen);
    for (i = 0, ep = wenvblock; i < dupwenvc; i++) {
        int nn = xwcslen(dupwenvp[i]);
        wmemcpy(ep, dupwenvp[i], nn);
        ep += nn + 1;
    }
    if (runbatchmode == 0) {
        wh[1] = xcreatethread(0, 0, &monitorthread);
        if (IS_INVALID_HANDLE(wh[1])) {
            rv = ERROR_TOO_MANY_TCBS;
            svcsyserror(__LINE__, rv, L"monitorthread");
            goto finished;
        }
        wc = 2;
    }
    wh[0] = xcreatethread(0, 0, &workerthread);
    if (IS_INVALID_HANDLE(wh[0])) {
        if (runbatchmode == 0) {
            InterlockedExchange(&monitorsig, 0);
            SetEvent(monitorevent);
            CloseHandle(wh[1]);
        }
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"workerthread");
        goto finished;
    }
    dbgprints(__FUNCTION__, "running");
    WaitForMultipleObjects(wc, wh, TRUE, INFINITE);
    SAFE_CLOSE_HANDLE(wh[0]);
    SAFE_CLOSE_HANDLE(wh[1]);

    wh[0] = svcstopended;
    wh[1] = svcexecended;
    dbgprintf(__FUNCTION__, "wait for stop and exec threads to finish");
    WaitForMultipleObjects(2, wh, TRUE, SVCBATCH_STOP_WAIT);

finished:

    SAFE_CLOSE_HANDLE(inputpipewrs);
    SAFE_CLOSE_HANDLE(outputpiperd);
    SAFE_CLOSE_HANDLE(childprocess);
    SAFE_CLOSE_HANDLE(childprocjob);

    closelogfile();
    dbgprints(__FUNCTION__, "done");
    reportsvcstatus(SERVICE_STOPPED, rv);
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
    SAFE_CLOSE_HANDLE(svcexecended);
    SAFE_CLOSE_HANDLE(monitorevent);
    SAFE_CLOSE_HANDLE(workingevent);
    SAFE_CLOSE_HANDLE(childprocjob);

    DeleteCriticalSection(&logfilelock);
    DeleteCriticalSection(&servicelock);
#if defined(_DBGVIEW)
    DeleteCriticalSection(&dbgviewlock);
#endif
}

int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
{
    int         i;
    int         rv = 0;
    wchar_t    *opath;
    wchar_t    *cpath;
    wchar_t    *bname = NULL;
    wchar_t    *rotateparam = NULL;
    int         envc       = 0;
    int         hasopts    = 1;
    HANDLE      h;
    SERVICE_TABLE_ENTRYW se[2];

    /**
     * Make sure children (cmd.exe) are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    for (i = 1; i < argc; i++) {
        const wchar_t *p = wargv[i];
        if (((p[0] == L'-') || (p[0] == L'/')) && (p[1] != L'\0') && (p[2] == L'\0')) {
            int pchar = towlower(p[1]);
            if (pchar == L'i' || pchar == L'd') {
                cnamestamp   = RUNBATCH_NAME " " SVCBATCH_VERSION_STR " " SVCBATCH_BUILD_STAMP;
                wnamestamp   = CPP_WIDEN(RUNBATCH_APPNAME);
                wnbatchapp   = CPP_WIDEN(RUNBATCH_NAME);
                if (pchar == L'i')
                    consolemode  = 1;
                if (pchar == L'd')
                    runbatchmode = 1;
            }
        }
    }
#if defined(_DBGVIEW)
    InitializeCriticalSection(&dbgviewlock);
#if defined(_DBGVIEW_SAVE)
    dbginitick = GetTickCount64();
#endif
    OutputDebugStringA(cnamestamp);
#endif
    if (wenv != NULL) {
        while (wenv[envc] != NULL)
            ++envc;
    }
    if (envc == 0)
        return svcsyserror(__LINE__, 0, L"Missing environment");
    /**
     * Simple case insensitive argument parsing
     * that allows both '-' and '/' as cmdswitches
     */
    for (i = 1; i < argc; i++) {
        const wchar_t *p = wargv[i];
        if (p[0] == L'\0')
            return svcsyserror(__LINE__, 0, L"Empty command line argument");
        if (hasopts) {
            if (servicehome == zerostring) {
                servicehome = getrealpathname(p, 1);
                if (servicehome == NULL)
                    return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, p);
                continue;
            }
            if (loglocation == zerostring) {
                loglocation = expandenvstrings(p);
                if (loglocation == NULL)
                    return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, p);
                continue;
            }
            if (serviceexec == zerostring) {
                serviceexec = xwcsdup(p);
                continue;
            }
            if (rotateparam == zerostring) {
                rotateparam = xrmspaces(xwcsdup(p), p);
                if (IS_EMPTY_WCS(rotateparam))
                    return svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, p);
                continue;
            }
            if (serviceuuid == zerostring) {
                serviceuuid = xwcsdup(p);
                continue;
            }
            if (servicename == zerostring) {
                servicename = xwcsdup(p);
                continue;
            }
            hasopts = 0;
            if ((p[0] == L'-') || (p[0] == L'/')) {
                if (p[1] == L'\0')
                    return svcsyserror(__LINE__, 0, L"Invalid command line option");
                if (p[2] == L'\0')
                    hasopts = 1;
                else if (p[0] == L'-')
                    return svcsyserror(__LINE__, 0, L"Invalid command line option");
            }
            if (hasopts) {
                int pchar = towlower(p[1]);
                switch (pchar) {
                    case L'b':
                        hasctrlbreak = 1;
                    break;
                    case L'c':
                        usecleanpath = 1;
                    break;
                    case L'i':
                        consolemode  = 1;
                    break;
                    case L'd':
                        runbatchmode = 1;
                    break;
                    case L'o':
                        loglocation  = zerostring;
                    break;
                    case L'r':
                        autorotate   = 1;
                        rotateparam  = zerostring;
                    break;
                    case L's':
                        usesafeenv   = 1;
                    break;
                    case L'w':
                        servicehome  = zerostring;
                    break;
                    case L'u':
                        serviceuuid  = zerostring;
                    break;
                    case L'x':
                        serviceexec  = zerostring;
                    break;
                    case L'n':
                        servicename  = zerostring;
                    break;
                    default:
                        return svcsyserror(__LINE__, 0, L"Unknown command line option");
                    break;
                }
                continue;
            }
        }
        if (IS_EMPTY_WCS(bname)) {
            bname = expandenvstrings(p);
            if (bname == NULL)
                return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, p);
        }
        else {
            /**
             * We have extra parameters after batch file.
             * This is user install error.
             */
            return svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, p);
        }
    }
    if (consolemode) {
        fputs(cnamestamp, stdout);
        fputs("\n\n",     stdout);
    }
#if defined(_CHECK_IF_SERVICE)
    else if (runbatchmode == 0} {
        if (runningasservice() == 0) {
            fputs(cnamestamp, stderr);
            fputs("\n" SVCBATCH_COPYRIGHT "\n\n", stderr);
            fputs("This program can only run as Windows Service\n", stderr);
            return svcsyserror(__LINE__, 0, L"Not a Windows Service");;
        }
    }
#endif
    nonsvcmode = consolemode + runbatchmode;
    if (nonsvcmode > 1)
        return svcsyserror(__LINE__, 0, L"Both -i and -d parameters are specified");
    if (IS_EMPTY_WCS(bname))
        return svcsyserror(__LINE__, 0, L"Missing batch file");

    if (resolvesvcbatchexe() == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, wargv[0]);
    if (IS_EMPTY_WCS(serviceuuid)) {
        if (runbatchmode)
            return svcsyserror(__LINE__, 0, L"Missing -u <SVCBATCH_SERVICE_UUID> parameter");
        else
            serviceuuid = xuuidstring();
    }
    if (IS_EMPTY_WCS(serviceuuid))
        return svcsyserror(__LINE__, GetLastError(), L"xuuidstring");
    if ((opath = xgetenv(L"PATH")) == NULL)
        return svcsyserror(__LINE__, ERROR_ENVVAR_NOT_FOUND, L"PATH");
    if ((cpath = xgetenv(L"COMSPEC")) == NULL)
        return svcsyserror(__LINE__, ERROR_ENVVAR_NOT_FOUND, L"COMSPEC");
    if ((comspec = getrealpathname(cpath, 0)) == NULL)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, cpath);
    xfree(cpath);

    if ((servicehome == NULL) && isrelativepath(bname)) {
        /**
         * Batch file is not absolute path
         * and we don't have provided workdir.
         * Use servicebase as cwd
         */
        servicehome = servicebase;
    }

    if (servicehome != NULL) {
        if (!SetCurrentDirectoryW(servicehome))
            return svcsyserror(__LINE__, GetLastError(), servicehome);
    }

    if (resolvebatchname(bname) == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, bname);
    xfree(bname);
    if (IS_EMPTY_WCS(servicehome)) {
        /**
         * Use batch file directory as new cwd
         */
        servicehome = batchdirname;
        if (!SetCurrentDirectoryW(servicehome))
            return svcsyserror(__LINE__, GetLastError(), servicehome);
    }
    if (IS_EMPTY_WCS(loglocation))
        loglocation = xwcsconcat(servicehome, L"\\" SVCBATCH_LOG_BASE);
    dupwenvp = waalloc(envc + 8);
    for (i = 0; i < envc; i++) {
        const wchar_t **e;
        const wchar_t  *p = wenv[i];

        if (IS_EMPTY_WCS(p))
            continue;
        if (usesafeenv) {
            e = safewinenv;
            p = NULL;
            while (*e != NULL) {
                if (strstartswith(wenv[i], *e)) {
                    p = wenv[i];
                    break;
                }
                e++;
            }
        }
        else {
            e = removeenv;
            while (*e != NULL) {
                if (strstartswith(p, *e)) {
                    p = NULL;
                    break;
                }
                e++;
            }
        }
        if (p != NULL)
            dupwenvp[dupwenvc++] = xwcsdup(p);
    }

    if (usecleanpath) {
        wchar_t *cp = xwcsvarcat(servicebase, L";",
                                 servicehome,
                                 stdwinpaths, NULL);
        xfree(opath);
        opath = expandenvstrings(cp);
        if (IS_EMPTY_WCS(opath))
            return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, cp);
        xfree(cp);
    }
    else {
        xcleanwinpath(opath);
    }
    dupwenvp[dupwenvc++] = xwcsconcat(L"PATH=", opath);
    xfree(opath);

    rv = resolverotate(rotateparam);
    if (rv != 0)
        return svcsyserror(rv, ERROR_INVALID_PARAMETER, rotateparam);
    memset(&ssvcstatus, 0, sizeof(SERVICE_STATUS));
    memset(&sazero,     0, sizeof(SECURITY_ATTRIBUTES));
    sazero.nLength = DSIZEOF(SECURITY_ATTRIBUTES);
    /**
     * Create logic state events
     */
    svcstopended = CreateEventW(&sazero, TRUE, TRUE,  NULL);
    if (IS_INVALID_HANDLE(svcstopended))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent");
    svcexecended = CreateEventW(&sazero, TRUE, TRUE,  NULL);
    if (IS_INVALID_HANDLE(svcexecended))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent");
    processended = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(processended))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent");
    monitorevent = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(monitorevent))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent");
    workingevent = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(workingevent))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent");
    childprocjob = CreateJobObjectW(&sazero, NULL);
    if (IS_INVALID_HANDLE(childprocjob))
        return svcsyserror(__LINE__, GetLastError(), L"CreateJobObject");

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
                return svcsyserror(__LINE__, GetLastError(), L"GetStdHandle");
            dbgprints(__FUNCTION__, "allocated new console");
        }
        else {
            return svcsyserror(__LINE__, GetLastError(), L"AllocConsole");
        }
    }
    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = NULL;
    se[1].lpServiceProc = NULL;
    if (nonsvcmode) {
        dbgprints(__FUNCTION__, "running batchfile");
        servicemain(1, cwargv);
        rv = ssvcstatus.dwServiceSpecificExitCode;
    }
    else {
        dbgprints(__FUNCTION__, "starting service");
        if (!StartServiceCtrlDispatcherW(se))
            rv = svcsyserror(__LINE__, GetLastError(), L"StartServiceCtrlDispatcher");
    }
#if defined(_DBGVIEW)
    dbgprints(__FUNCTION__, "done");
#if defined(_DBGVIEW_SAVE)
    EnterCriticalSection(&dbgviewlock);
    h = InterlockedExchangePointer(&dbgfhandle, NULL);
    if (h != NULL) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    LeaveCriticalSection(&dbgviewlock);
#endif
#endif
    return rv;
}
