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
#include "svcbatch.h"

static volatile LONG         monitorsig  = 0;
static volatile LONG         sstarted    = 0;
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
static int       autorotate       = 0;
static int       servicemode      = 1;
static int       svcmaxlogs       = SVCBATCH_MAX_LOGS;
static DWORD     preshutdown      = 0;

static wchar_t  *svcbatchfile     = NULL;
static wchar_t  *shutdownfile     = NULL;
static wchar_t  *svcbatchexe      = NULL;
static wchar_t  *exelocation      = NULL;
static wchar_t  *servicebase      = NULL;
static wchar_t  *servicename      = NULL;
static wchar_t  *servicehome      = NULL;
static wchar_t  *serviceuuid      = NULL;

static wchar_t  *loglocation      = NULL;
static wchar_t  *logfilename      = NULL;
static ULONGLONG logtickcount     = CPP_UINT64_C(0);

static HANDLE    childprocjob     = NULL;
static HANDLE    childprocess     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    ssignalevent     = NULL;
static HANDLE    shutdowndone     = NULL;
static HANDLE    processended     = NULL;
static HANDLE    monitorevent     = NULL;
static HANDLE    outputpiperd     = NULL;
static HANDLE    inputpipewrs     = NULL;

static wchar_t      zerostring[4] = { L'\0', L'\0', L'\0', L'\0' };
static wchar_t      CRLFW[4]      = { L'\r', L'\n', L'\0', L'\0' };
static char         CRLFA[4]      = { '\r', '\n', '\r', '\n' };
static char         YYES[2]       = { 'Y', '\n'};

static const char    *cnamestamp  = SVCBATCH_APPNAME " " SVCBATCH_VERSION_STR " " SVCBATCH_BUILD_STAMP;
static const wchar_t *cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);
static const wchar_t *cwslogname  = L"\\" SVCBATCH_LOGNAME;

static const wchar_t *removeenv[] = {
    L"COMSPEC",
    L"PATH",
    L"SVCBATCH_SERVICE_BASE",
    L"SVCBATCH_SERVICE_HOME",
    L"SVCBATCH_SERVICE_LOGDIR",
    L"SVCBATCH_SERVICE_NAME",
    L"SVCBATCH_SERVICE_UUID",
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

static int xwcsisenvvar(const wchar_t *str, const wchar_t *var)
{
    while (*str != L'\0') {
        if (towlower(*str) != towlower(*var))
            break;
        str++;
        var++;
        if (*var == L'\0')
            return *str == L'=';
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

    cp = rv = xwalloc(l1 + l2 + 2);

    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    cp += l1;
    if(l2 > 0) {
        if (wcshavespace(s2)) {
            *(cp++) = L'"';
            wmemcpy(cp, s2, l2);
            cp += l2;
            *(cp++) = L'"';
        }
        else
            wmemcpy(cp, s2, l2);
    }
    xfree(s1);
    return rv;
}

static void xcleanwinpath(wchar_t *s, int isdir)
{
    int i;

    if (IS_EMPTY_WCS(s))
        return;

    for (i = 0; s[i] != L'\0'; i++) {
        if (s[i] == L'/')
            s[i] =  L'\\';
    }
    if (isdir) {
        --i;
        while (i > 1) {
            if ((s[i] == L';') || (s[i] == L' ') || (s[i] == L'.'))
                s[i--] = L'\0';
            else
                break;

        }
        while (i > 1) {
            if ((s[i] ==  L'\\') && (s[i - 1] != L'.'))
                s[i--] = L'\0';
            else
                break;
        }
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

    bp = buf;
    n = _snprintf(bp, blen,
                  "[%.4lu] %-16s ",
                  GetCurrentThreadId(), funcname);
    bp = bp + n;
    strncat(bp, string, blen - n);
    buf[SBUFSIZ - 1] = '\0';
    OutputDebugStringA(buf);
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

    if (InterlockedIncrement(&eset) > 1)
        return ssrv;
    kname = xwcsconcat( L"SYSTEM\\CurrentControlSet\\Services\\" \
                        L"EventLog\\Application\\", cwsappname);
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

static DWORD svcsyserror(int line, DWORD ern, const wchar_t *err, const wchar_t *eds)
{
    wchar_t        buf[BBUFSIZ];
    wchar_t        erb[MBUFSIZ];
    const wchar_t *errarg[10];
    int            i = 0;

    memset(buf, 0, sizeof(buf));
    _snwprintf(buf, BBUFSIZ - 1, L"svcbatch.c(%d)", line);

    errarg[i++] = cwsappname;
    if (IS_EMPTY_WCS(servicename))
        errarg[i++] = L"(undefined)";
    else
        errarg[i++] = servicename;
    errarg[i++] = L"reported the following error:\r\n";
    errarg[i++] = buf;
    errarg[i++] = err;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "%S (%lu) %S %S",
              buf, ern, err, eds == NULL ? L"" : eds);
#endif
    if (ern) {
        memset(erb, 0, sizeof(erb));
        xwinapierror(erb, MBUFSIZ - 1, ern);
        errarg[i++] = L":";
        errarg[i++] = erb;
    }
    else {
        ern = ERROR_INVALID_PARAMETER;
    }
    if (eds != NULL) {
        errarg[i++] = L":";
        errarg[i++] = eds;
    }
    errarg[i++] = CRLFW;
    while (i < 10) {
        errarg[i++] = NULL;
    }
    if (setupeventlog()) {
        HANDLE es = RegisterEventSourceW(NULL, cwsappname);
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
    *s = L'\0';
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

static wchar_t *expandenvstrings(const wchar_t *str, int isdir)
{
    wchar_t  *buf = NULL;
    DWORD     siz;
    DWORD     len = 0;

    if (IS_EMPTY_WCS(str))
        return NULL;
    if (servicemode == 0)
        return xwcsdup(str);

    if ((str[0] == L'.') && ((str[1] == L'\\') || (str[1] == L'/'))) {
        /**
         * Remove leading './' or '.\'
         */
        str += 2;
    }
    for (siz = 0; str[siz] != L'\0'; siz++) {
        if (str[siz] == L'%')
            len++;
    }
    if (siz == 0)
        return NULL;
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
    xcleanwinpath(buf, isdir);

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

    es = expandenvstrings(path, isdir);
    if (es == NULL)
        return NULL;
    if (servicemode == 0)
        return es;

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

static int resolvesvcbatchexe(const wchar_t *a)
{
    int i;

    svcbatchexe = getrealpathname(a, 0);
    if (IS_EMPTY_WCS(svcbatchexe))
        return 0;

    i = xwcslen(svcbatchexe);
    while (--i > 0) {
        if (svcbatchexe[i] == L'\\') {
            svcbatchexe[i] = L'\0';
            exelocation = xwcsdup(svcbatchexe);
            svcbatchexe[i] = L'\\';
            return 1;
        }
    }
    return 0;
}

static int resolvebatchname(const wchar_t *a)
{
    int i;

    if (svcbatchfile != NULL)
        return 1;
    svcbatchfile = getrealpathname(a, 0);
    if (IS_EMPTY_WCS(svcbatchfile))
        return 0;

    i = xwcslen(svcbatchfile);
    while (--i > 0) {
        if (svcbatchfile[i] == L'\\') {
            svcbatchfile[i] = L'\0';
            servicebase = xwcsdup(svcbatchfile);
            svcbatchfile[i] = L'\\';
            return 1;
        }
    }
    return 0;
}

#if defined(_DBGVIEW)
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
            if (_wcsnicmp(name, L"Service-", 8) == 0) {
                if (GetUserObjectInformationW(ws, UOI_FLAGS, &uf,
                                              DSIZEOF(uf), &len)) {
                    if (uf.dwFlags != WSF_VISIBLE)
                        rv = 1;
                    else
                        OutputDebugStringW(name);
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

    if (servicemode == 0)
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
    InterlockedExchange(&sscstate, status);
    if (!SetServiceStatus(hsvcstatus, &ssvcstatus)) {
        svcsyserror(__LINE__, GetLastError(), L"SetServiceStatus", NULL);
        InterlockedExchange(&sscstate, SERVICE_STOPPED);
    }
finished:
    LeaveCriticalSection(&servicelock);
}

static DWORD createiopipes(LPSTARTUPINFOW si, LPHANDLE iwrs, LPHANDLE ords)
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
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe", NULL);
    if (!DuplicateHandle(cp, sh, cp,
                         iwrs, FALSE, 0,
                         DUPLICATE_SAME_ACCESS)) {
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle", NULL);
        goto finished;
    }
    SAFE_CLOSE_HANDLE(sh);

    /**
     * Create stdout/stderr pipe, with read side
     * of the pipe as non inheritable
     */
    if (!CreatePipe(&sh, &(si->hStdError), &sa, 0))
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe", NULL);
    if (!DuplicateHandle(cp, sh, cp,
                         ords, FALSE, 0,
                         DUPLICATE_SAME_ACCESS))
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle", NULL);
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
    logprintf(h, "Service name     : %S", servicename);
    logprintf(h, "Service uuid     : %S", serviceuuid);
    logprintf(h, "Batch file       : %S", svcbatchfile);
    logprintf(h, "Base directory   : %S", servicebase);
    logprintf(h, "Working directory: %S", servicehome);
    logprintf(h, "Log directory    : %S", loglocation);

    if (servicemode) {
        wchar_t *fs = NULL;

        if (autorotate)
            fs = xwcsappend(fs, L"autorotate, ");
        if (shutdownfile)
            fs = xwcsappend(fs, L"shutdown batch, ");
        if (hasctrlbreak)
            fs = xwcsappend(fs, L"ctrl+break, ");
        if (preshutdown)
            fs = xwcsappend(fs, L"accept preshutdown, ");

        if (fs != NULL) {
            int i = xwcslen(fs);
            fs[i - 2] = L'\0';
            logprintf(h, "Features         : %S", fs);
            xfree(fs);
        }
    }
    logfflush(h);
}

static DWORD openlogfile(BOOL firstopen)
{
    wchar_t sfx[4] = { L'.', L'0', L'\0', L'\0' };
    wchar_t *logpb = NULL;
    DWORD rc = 0;
    HANDLE h = NULL;
    WIN32_FILE_ATTRIBUTE_DATA ad;
    int i;

    logtickcount = GetTickCount64();

    if (logfilename == NULL) {
        wchar_t *pp = loglocation;

        loglocation = getrealpathname(pp, 1);
        if (loglocation == NULL) {
            rc = xcreatepath(pp);
            if (rc != 0) {
                svcsyserror(__LINE__, 0, L"xcreatepath", pp);
                return rc;
            }
            loglocation = getrealpathname(pp, 1);
            if (loglocation == NULL)
                return ERROR_PATH_NOT_FOUND;
        }
        xfree(pp);
        if (_wcsicmp(loglocation, servicehome) == 0) {
            svcsyserror(__LINE__, 0,
                        L"Loglocation cannot be the same as servicehome", NULL);
            return ERROR_BAD_PATHNAME;
        }
        logfilename = xwcsconcat(loglocation, cwslogname);
    }
    if (svcmaxlogs > 0) {
        if (GetFileAttributesExW(logfilename, GetFileExInfoStandard, &ad)) {
            DWORD mm = 0;

            if (firstopen)
                reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
            if (autorotate == 0) {
                mm = MOVEFILE_REPLACE_EXISTING;
                logpb = xwcsconcat(logfilename, sfx);
            }
            else {
                SYSTEMTIME st;
                wchar_t wrb[24];

                FileTimeToSystemTime(&ad.ftLastWriteTime, &st);
                _snwprintf(wrb, 22, L".%.4d-%.2d-%.2d.%.2d%.2d%.2d",
                           st.wYear, st.wMonth, st.wDay,
                           st.wHour, st.wMinute, st.wSecond);
                logpb = xwcsconcat(logfilename, wrb);
            }
            if (!MoveFileExW(logfilename, logpb, mm)) {
                rc = GetLastError();
                xfree(logpb);
                return svcsyserror(__LINE__, rc, L"MoveFileExW", logfilename);
            }
        }
        else {
            rc = GetLastError();
            if (rc != ERROR_FILE_NOT_FOUND) {
                return svcsyserror(__LINE__, rc, L"GetFileAttributesExW", logfilename);
            }
        }
    }
    if (firstopen)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    if ((autorotate == 0) && (svcmaxlogs > 0)) {
        /**
         * Rotate previous log files
         */
        for (i = svcmaxlogs; i > 0; i--) {
            wchar_t *logpn;

            sfx[1] = L'0' + i - 1;
            logpn  = xwcsconcat(logfilename, sfx);

            if (GetFileAttributesW(logpn) != INVALID_FILE_ATTRIBUTES) {
                wchar_t *lognn;

                sfx[1] = L'0' + i;
                lognn = xwcsconcat(logfilename, sfx);
                if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING)) {
                    rc = GetLastError();
                    svcsyserror(__LINE__, rc, L"MoveFileExW", lognn);
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
                    FILE_SHARE_READ, &sazero, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(h)) {
        goto failed;
    }
    if (rc == ERROR_ALREADY_EXISTS) {
        logfflush(h);
    }
    else {
        InterlockedExchange(&logwritten, 0);
    }
    logwrline(h, cnamestamp);
    if (firstopen)
        logwrtime(h, "Log opened");
    InterlockedExchangePointer(&logfhandle, h);
    xfree(logpb);
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
    h = InterlockedExchangePointer(&logfhandle, NULL);
    if (h == NULL) {
        LeaveCriticalSection(&logfilelock);
        return ERROR_FILE_NOT_FOUND;
    }
    logfflush(h);
    if (svcmaxlogs == 0) {
        logwrline(h, "Log rotatation disabled");
        logfflush(h);
        InterlockedExchangePointer(&logfhandle, h);
        LeaveCriticalSection(&logfilelock);
        return 0;
    }
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
        svcsyserror(__LINE__, rc, L"rotatelogs", NULL);
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
        return 0;
    }
    if (iswdigit(str[0]) && (str[1] == L'\0')) {
        svcmaxlogs = _wtoi(str);
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
            if (mm != 0 ) {
                if (mm < SVCBATCH_MIN_LOGRTIME)
                    return __LINE__;
                rotateint = mm * ONE_MINUTE * CPP_INT64_C(-1);
                dbgprintf(__FUNCTION__, "rotate in %d minutes", mm);
                rotatetmo.QuadPart = rotateint;
            }
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
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "rotate at %.4d-%.2d-%.2d %.2d:%.2d:%.2d",
                      st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond);
#endif
        }
    }
    if (rp != NULL) {
        LONGLONG siz;
        LONGLONG mux = CPP_INT64_C(1);
        wchar_t *ep  = zerostring;
        wchar_t  mm  = 0;

        if (iswdigit(*rp) == 0)
            return __LINE__;
        siz = _wcstoi64(rp, &ep, 10);
        if (siz < mux)
            return __LINE__;
        if (*ep != '\0') {
            mm = towupper(ep[0]);
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
        }
        rotatesiz.QuadPart = siz * mux;
        if (rotatesiz.QuadPart < SVCBATCH_MIN_LOGSIZE)
            return __LINE__;
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "rotate if > %lu%C", (DWORD)siz, mm);
#endif
    }
    xfree(sp);
    autorotate = 1;
    return 0;
}

static unsigned int __stdcall iopipethread(void *rdpipe)
{
    DWORD  rc = 0;

    dbgprints(__FUNCTION__, "started");
    while (rc == 0) {
        BYTE  rb[HBUFSIZ];
        DWORD rd = 0;
        HANDLE h = NULL;

        if (ReadFile((HANDLE)rdpipe, rb, HBUFSIZ, &rd, NULL)) {
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

static unsigned int __stdcall shutdownthread(void *unused)
{
    wchar_t *cmdline;
    HANDLE   wh[4];
    HANDLE   job = NULL;
    HANDLE   wrs = NULL;
    HANDLE   rds = NULL;
    DWORD    rc, ws;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;

    dbgprints(__FUNCTION__, "started");

    cmdline = xappendarg(NULL, svcbatchexe);
    cmdline = xwcsappend(cmdline, L" -x ");
    cmdline = xappendarg(cmdline, shutdownfile);
    cmdline = xwcsappend(cmdline, L" -n ");
    cmdline = xappendarg(cmdline, servicename);
    cmdline = xwcsappend(cmdline, L" -u ");
    cmdline = xwcsappend(cmdline, serviceuuid);
    if (autorotate) {
        cmdline = xwcsappend(cmdline, L" -r @0");
    }
    else {
        wchar_t rb[8];
        _snwprintf(rb, 6, L" -r %d", svcmaxlogs);
        cmdline = xwcsappend(cmdline, rb);
    }
    cmdline = xwcsappend(cmdline, L" -w ");
    cmdline = xappendarg(cmdline, servicehome);
    cmdline = xwcsappend(cmdline, L" -o ");
    cmdline = xappendarg(cmdline, loglocation);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);
#endif
    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    rc = createiopipes(&si, &wrs, &rds);
    if (rc != 0) {
        setsvcstatusexit(rc);
        goto finished;
    }

    ji.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    job = CreateJobObjectW(&sazero, NULL);
    if (IS_INVALID_HANDLE(job)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"CreateJobObjectW", NULL);
        goto finished;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &ji,
                                 DSIZEOF(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"SetInformationJobObject", NULL);
        goto finished;
    }

    if (!CreateProcessW(svcbatchexe, cmdline, NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
                        wenvblock,
                        servicehome,
                       &si, &cp)) {
        rc = GetLastError();
        svcsyserror(__LINE__, rc, L"CreateProcess", NULL);
        goto finished;
    }

    dbgprintf(__FUNCTION__, "child pid: %lu", cp.dwProcessId);
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    if (!AssignProcessToJobObject(job, cp.hProcess)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"AssignProcessToJobObject", NULL);
        TerminateProcess(cp.hProcess, rc);
        goto finished;
    }

    wh[0] = cp.hProcess;
    wh[1] = ssignalevent;
    wh[2] = processended;
    wh[3] = xcreatethread(0, 0, &iopipethread, rds);
    ResumeThread(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hThread);

    ws = WaitForMultipleObjects(4, wh, FALSE, INFINITE);
    switch (ws) {
        case WAIT_OBJECT_1:
        case WAIT_OBJECT_2:
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "generating CTRL_BREAK_EVENT for: %lu",
                      cp.dwProcessId);
#endif
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, cp.dwProcessId);
        case WAIT_OBJECT_3:
            rc = WaitForSingleObject(cp.hProcess, SVCBATCH_STOP_CHECK);
            if (rc != WAIT_OBJECT_0) {
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "calling TerminateProcess for child: %lu", cp.dwProcessId);
#endif
                TerminateProcess(cp.hProcess, ERROR_BROKEN_PIPE);
            }
        break;
#if defined(_DBGVIEW)
        case WAIT_OBJECT_0:
            dbgprintf(__FUNCTION__, "child process: %lu done", cp.dwProcessId);
        break;
#endif
        default:
        break;
    }
    CloseHandle(wh[3]);

finished:
    xfree(cmdline);
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    SAFE_CLOSE_HANDLE(rds);
    SAFE_CLOSE_HANDLE(wrs);
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hProcess);
    SAFE_CLOSE_HANDLE(job);

    dbgprints(__FUNCTION__, "done");
    SetEvent(shutdowndone);
    XENDTHREAD(0);
}

static unsigned int __stdcall stopthread(void *unused)
{
    DWORD wr, ws;
    int   i;

    if (InterlockedIncrement(&sstarted) > 1) {
        dbgprints(__FUNCTION__, "already started");
        XENDTHREAD(0);
    }
    ResetEvent(svcstopended);

#if defined(_DBGVIEW)
    if (servicemode)
        dbgprints(__FUNCTION__, "service stop");
    else
        dbgprints(__FUNCTION__, "shutdown stop ");
#endif
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

    if (shutdownfile != NULL) {
        ResetEvent(shutdowndone);
        dbgprints(__FUNCTION__, "creating shutdown process");
        xcreatethread(1, 0, &shutdownthread, NULL);
        ws = WaitForSingleObject(shutdowndone, SVCBATCH_STOP_CHECK);
        if (ws == WAIT_OBJECT_0) {
            dbgprints(__FUNCTION__, "processended by shutdown");
            goto finished;
        }
        dbgprints(__FUNCTION__, "shutdown still running");
    }
    if (SetConsoleCtrlHandler(NULL, TRUE)) {
        dbgprints(__FUNCTION__, "raising CTRL_C_EVENT");
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        ws = WaitForSingleObject(processended, SVCBATCH_STOP_SYNC);
        SetConsoleCtrlHandler(NULL, FALSE);
        if (ws == WAIT_OBJECT_0) {
            dbgprints(__FUNCTION__, "processended by CTRL_C_EVENT");
            goto finished;
        }
        dbgprints(__FUNCTION__, "process still running");
    }
#if defined(_DBGVIEW)
    else {
        dbgprintf(__FUNCTION__, "SetConsoleCtrlHandler failed %lu", GetLastError());
    }
#endif
    for (i = 0; i < 2; i++) {
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "sending Y to child (attempt=%d)", i + 1);
#endif
        /**
         * Write Y to stdin pipe in case cmd.exe waits for
         * user reply to "Terminate batch job (Y/N)?"
         */
        WriteFile(inputpipewrs, YYES, 2, &wr, NULL);

        ws = WaitForSingleObject(processended, SVCBATCH_STOP_STEP);
        if (ws == WAIT_OBJECT_0) {
            dbgprints(__FUNCTION__, "processended signaled");
            goto finished;
        }
    }
    dbgprints(__FUNCTION__, "Child is still active, terminating");
    /**
     * WAIT_TIMEOUT means that child is
     * still running and we need to terminate
     * child tree by brute force
     */
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    SAFE_CLOSE_HANDLE(childprocess);
    SAFE_CLOSE_HANDLE(childprocjob);

finished:
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    SetEvent(ssignalevent);
    SetEvent(svcstopended);
    dbgprints(__FUNCTION__, "done");
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
                        logwrline(h, "signaled CTRL_BREAK_EVENT");
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
                        xcreatethread(1, 0, &stopthread, NULL);
                        break;
                    }
                    rotatesynctime();
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
    wc = WaitForSingleObject(processended, SVCBATCH_LOGROTATE_INIT);
    if (wc != WAIT_TIMEOUT) {
#if defined(_DBGVIEW)
        if (wc == WAIT_OBJECT_0)
            dbgprints(__FUNCTION__, "processended signaled");
        else
            dbgprintf(__FUNCTION__, "processended: %lu", wc);
#endif
        goto finished;
    }
    dbgprints(__FUNCTION__, "running");

    if (rotatesiz.QuadPart)
        ms = SVCBATCH_LOGROTATE_STEP;
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
                                xcreatethread(1, 0, &stopthread, NULL);
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
                        svcsyserror(__LINE__, rc, L"GetFileSizeEx", NULL);
                        xcreatethread(1, 0, &stopthread, NULL);
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
                    xcreatethread(1, 0, &stopthread, NULL);
                }
            break;
            case WAIT_OBJECT_1:
                rc = ERROR_PROCESS_ABORTED;
                dbgprints(__FUNCTION__, "processended signaled");
            break;
            case WAIT_FAILED:
                rc = GetLastError();
                dbgprintf(__FUNCTION__, "wait failed: %lu", rc);
            break;
            default:
                rc = wc;
                dbgprintf(__FUNCTION__, "wait error: %lu", rc);
            break;
        }
    }

finished:
    dbgprints(__FUNCTION__, "done");
    SAFE_CLOSE_HANDLE(rotatedev);
    XENDTHREAD(0);
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
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "comspec %S", comspec);
    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);
#endif
    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    if (servicemode) {
        rotatedev = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (IS_INVALID_HANDLE(rotatedev)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            svcsyserror(__LINE__, rc, L"CreateWaitableTimer", NULL);
            goto finished;
        }
        if (!SetWaitableTimer(rotatedev, &rotatetmo, 0, NULL, NULL, 0)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            svcsyserror(__LINE__, rc, L"SetWaitableTimer", NULL);
            goto finished;
        }
    }
    rc = createiopipes(&si, &inputpipewrs, &outputpiperd);
    if (rc != 0) {
        setsvcstatusexit(rc);
        goto finished;
    }
    childprocjob = CreateJobObjectW(&sazero, NULL);
    if (IS_INVALID_HANDLE(childprocjob)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"CreateJobObjectW", NULL);
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
        svcsyserror(__LINE__, rc, L"SetInformationJobObject", NULL);
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
        svcsyserror(__LINE__, rc, L"CreateProcess", NULL);
        goto finished;
    }
    childprocess = cp.hProcess;
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    if (!AssignProcessToJobObject(childprocjob, childprocess)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"AssignProcessToJobObject", NULL);
        TerminateProcess(childprocess, rc);
        goto finished;
    }
    wh[0] = childprocess;
    wh[1] = xcreatethread(0, CREATE_SUSPENDED, &iopipethread, outputpiperd);
    if (IS_INVALID_HANDLE(wh[1])) {
        rc = ERROR_TOO_MANY_TCBS;
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"iopipethread", NULL);
        TerminateProcess(childprocess, ERROR_OUTOFMEMORY);
        goto finished;
    }

    ResumeThread(cp.hThread);
    ResumeThread(wh[1]);
    SAFE_CLOSE_HANDLE(cp.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "running child pid: %lu", cp.dwProcessId);
#endif
    if (servicemode)
        xcreatethread(1, 0, &rotatethread, NULL);
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

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    const char *msg = NULL;

    switch (ctrl) {
        case CTRL_CLOSE_EVENT:
            msg = "signaled CTRL_CLOSE_EVENT";
        break;
        case CTRL_SHUTDOWN_EVENT:
            msg = "signaled CTRL_SHUTDOWN_EVENT";
        break;
        case CTRL_C_EVENT:
            msg = "signaled CTRL_C_EVENT";
        break;
        case CTRL_BREAK_EVENT:
            msg = "Signaled CTRL_BREAK_EVENT";
        break;
    }

    switch (ctrl) {
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
        case CTRL_C_EVENT:
#if defined(_DBGVIEW)
            dbgprints(__FUNCTION__, msg);
#endif
        break;
        case CTRL_BREAK_EVENT:
#if defined(_DBGVIEW)
            dbgprints(__FUNCTION__, "signaled CTRL_BREAK_EVENT");
#endif
            if (servicemode == 0) {
                HANDLE h = NULL;
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h != NULL) {
                    logfflush(h);
                    logwrline(h, msg);
                }
                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
                reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
                xcreatethread(1, 0, &stopthread, NULL);
            }
        break;
        case CTRL_LOGOFF_EVENT:
            dbgprints(__FUNCTION__, "signaled CTRL_LOGOFF_EVENT");
        break;
        default:
            dbgprintf(__FUNCTION__, "unknown ctrl: %lu", ctrl);
            return FALSE;
        break;
    }
    return TRUE;
}

static DWORD WINAPI servicehandler(DWORD ctrl, DWORD _xe, LPVOID _xd, LPVOID _xc)
{
    HANDLE h = NULL;
    const char *msg = NULL;

    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
            msg = "Signaled SERVICE_CONTROL_PRESHUTDOWN";
        break;
        case SERVICE_CONTROL_SHUTDOWN:
            msg = "Signaled SERVICE_CONTROL_SHUTDOWN";
        break;
        case SERVICE_CONTROL_STOP:
            msg = "Signaled SERVICE_CONTROL_STOP";
        break;
    }

    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
        case SERVICE_CONTROL_SHUTDOWN:
        case SERVICE_CONTROL_STOP:
#if defined(_DBGVIEW)
            dbgprints(__FUNCTION__, msg);
#endif
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
            EnterCriticalSection(&logfilelock);
            h = InterlockedExchangePointer(&logfhandle, NULL);
            if (h != NULL) {
                logfflush(h);
                logwrline(h, msg);
            }
            InterlockedExchangePointer(&logfhandle, h);
            LeaveCriticalSection(&logfilelock);
            xcreatethread(1, 0, &stopthread, NULL);
        break;
        case SVCBATCH_CTRL_BREAK:
            dbgprints(__FUNCTION__, "signaled SVCBATCH_CTRL_BREAK");
            if (hasctrlbreak == 0) {
                dbgprints(__FUNCTION__, "disabled SVCBATCH_CTRL_BREAK signal");
                return ERROR_CALL_NOT_IMPLEMENTED;
            }
            InterlockedExchange(&monitorsig, ctrl);
            SetEvent(monitorevent);
        break;
        case SVCBATCH_CTRL_ROTATE:
            /**
             * Signal to monitorthread that
             * user send custom service control
             */
            dbgprints(__FUNCTION__, "signaled SVCBATCH_CTRL_ROTATE");
            InterlockedExchange(&monitorsig, ctrl);
            SetEvent(monitorevent);
        break;
        case SERVICE_CONTROL_INTERROGATE:
            dbgprints(__FUNCTION__, "signaled SERVICE_CONTROL_INTERROGATE");
        break;
        default:
            dbgprintf(__FUNCTION__, "unknown ctrl: %lu", ctrl);
            return ERROR_CALL_NOT_IMPLEMENTED;
        break;
    }
    return 0;
}

static void WINAPI servicemain(DWORD argc, wchar_t **argv)
{
    int          i;
    DWORD        rv = 0;
    int          eblen = 0;
    wchar_t     *ep;
    HANDLE       wh[4] = { NULL, NULL, NULL, NULL };
    DWORD        ws, wc = 1;

    ssvcstatus.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    ssvcstatus.dwCurrentState = SERVICE_START_PENDING;

    if (IS_EMPTY_WCS(servicename) && (argc > 0))
        servicename = xwcsdup(argv[0]);
    if (IS_EMPTY_WCS(servicename)) {
        svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, L"Missing servicename", NULL);
        exit(ERROR_INVALID_PARAMETER);
        return;
    }
    if (servicemode) {
        hsvcstatus = RegisterServiceCtrlHandlerExW(servicename, servicehandler, NULL);
        if (IS_INVALID_HANDLE(hsvcstatus)) {
            svcsyserror(__LINE__, GetLastError(), L"RegisterServiceCtrlHandlerEx", NULL);
            exit(ERROR_INVALID_HANDLE);
            return;
        }
        dbgprintf(__FUNCTION__, "started %S", servicename);
    }
#if defined(_DBGVIEW)
    else {
        dbgprintf(__FUNCTION__, "running shutdown for %S service", servicename);
    }
#endif
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    rv = openlogfile(TRUE);
    if (rv != 0) {
        svcsyserror(__LINE__, rv, L"openlogfile", logfilename);
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
    if (servicemode) {
        wh[1] = xcreatethread(0, 0, &monitorthread, NULL);
        if (IS_INVALID_HANDLE(wh[1])) {
            rv = ERROR_TOO_MANY_TCBS;
            svcsyserror(__LINE__, rv, L"monitorthread", NULL);
            goto finished;
        }
        wc = 2;
    }
    wh[0] = xcreatethread(0, 0, &workerthread, NULL);
    if (IS_INVALID_HANDLE(wh[0])) {
        if (servicemode) {
            InterlockedExchange(&monitorsig, 0);
            SetEvent(monitorevent);
            CloseHandle(wh[1]);
        }
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"workerthread", NULL);
        goto finished;
    }
    dbgprints(__FUNCTION__, "running");
    WaitForMultipleObjects(wc, wh, TRUE, INFINITE);
    SAFE_CLOSE_HANDLE(wh[0]);
    SAFE_CLOSE_HANDLE(wh[1]);

    dbgprints(__FUNCTION__, "waiting for stop");
    wh[0] = svcstopended;
    wh[1] = shutdowndone;
    ws = WaitForMultipleObjects(2, wh, TRUE, SVCBATCH_STOP_HINT);
    if (ws == WAIT_TIMEOUT) {
        dbgprints(__FUNCTION__, "sending stop signal");
        SetEvent(ssignalevent);
        WaitForMultipleObjects(2, wh, TRUE, SVCBATCH_STOP_CHECK);
    }

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
    SAFE_CLOSE_HANDLE(shutdowndone);
    SAFE_CLOSE_HANDLE(ssignalevent);
    SAFE_CLOSE_HANDLE(monitorevent);
    SAFE_CLOSE_HANDLE(childprocjob);

    DeleteCriticalSection(&logfilelock);
    DeleteCriticalSection(&servicelock);
}

int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
{
    int         i  = 1;
    int         rv = 0;
    wchar_t    *orgpath;
    wchar_t    *batchparam  = NULL;
    wchar_t    *shomeparam  = NULL;
    int         envc        = 0;
    int         hasopts     = 1;
    HANDLE      h;
    SERVICE_TABLE_ENTRYW se[2];
    const wchar_t *rotateparam = NULL;
    const wchar_t *svcendparam = NULL;

    /**
     * Make sure children (cmd.exe) are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    /**
     * Check if running as service or as a child process.
     */
    if (argc > 6) {
        const wchar_t *p = wargv[1];
        if ((p[0] == L'-') && (p[1] == L'x') && (p[2] == L'\0')) {
            servicemode  = 0;
            cnamestamp = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_STR " " SVCBATCH_BUILD_STAMP;
            cwsappname = CPP_WIDEN(SHUTDOWN_APPNAME);
            cwslogname = L"\\" SHUTDOWN_LOGNAME;
            batchparam = xwcsdup(wargv[2]);
            i = 3;
        }
    }
#if defined(_DBGVIEW)
    dbgprints(__FUNCTION__, cnamestamp);
#endif
    if (wenv != NULL) {
        while (wenv[envc] != NULL)
            ++envc;
    }
    if (envc == 0)
        return svcsyserror(__LINE__, 0, L"Missing environment", NULL);
    if (resolvesvcbatchexe(wargv[0]) == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, wargv[0], NULL);
    /**
     * Simple case insensitive argument parsing
     * that allows both '-' and '/' as cmdswitches
     */
    while (i < argc) {
        const wchar_t *p = wargv[i++];
        if (p[0] == L'\0')
            return svcsyserror(__LINE__, 0, L"Empty command line argument", NULL);
        if (hasopts) {
            if (shomeparam == zerostring) {
                shomeparam = expandenvstrings(p, 1);
                if (shomeparam == NULL)
                    return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, p, NULL);
                continue;
            }
            if (loglocation == zerostring) {
                loglocation = expandenvstrings(p, 1);
                if (loglocation == NULL)
                    return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, p, NULL);
                continue;
            }
            if (svcendparam == zerostring) {
                svcendparam = p;
                continue;
            }
            if (rotateparam == zerostring) {
                rotateparam = p;
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
                    return svcsyserror(__LINE__, 0, L"Invalid command line option", p);
                if (p[2] == L'\0')
                    hasopts = 1;
                else if (p[0] == L'-')
                    return svcsyserror(__LINE__, 0, L"Invalid command line option", p);
            }
            if (hasopts) {
                int pchar = towlower(p[1]);
                switch (pchar) {
                    case L'b':
                        hasctrlbreak = 1;
                    break;
                    case L'o':
                        loglocation  = zerostring;
                    break;
                    case L'p':
                        preshutdown  = SERVICE_ACCEPT_PRESHUTDOWN;
                    break;
                    case L'r':
                        rotateparam  = zerostring;
                    break;
                    case L's':
                        svcendparam  = zerostring;
                    break;
                    case L'w':
                        shomeparam   = zerostring;
                    break;
                    /**
                     * Private options
                     */
                    case L'n':
                        servicename  = zerostring;
                    break;
                    case L'u':
                        serviceuuid  = zerostring;
                    break;
                    default:
                        return svcsyserror(__LINE__, 0, L"Unknown cmdline option", p);
                    break;
                }
                continue;
            }
        }
        if (IS_EMPTY_WCS(batchparam)) {
            batchparam = expandenvstrings(p, 0);
            if (batchparam == NULL)
                return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, p, NULL);
        }
        else {
            /**
             * We have extra parameters after batch file.
             * This is user install error.
             */
            return svcsyserror(__LINE__, 0, L"Extra parameters", p);
        }
    }
#if defined(_DBGVIEW)
    if (servicemode) {
        if (runningasservice() == 0) {
            fputs(cnamestamp, stderr);
            fputs("\n" SVCBATCH_COPYRIGHT "\n\n", stderr);
            fputs("This program can only run as Windows Service\n", stderr);
            return ERROR_INVALID_PARAMETER;
        }
    }
#endif
    if (IS_EMPTY_WCS(batchparam))
        return svcsyserror(__LINE__, 0, L"Missing batch file", NULL);
    if (IS_EMPTY_WCS(serviceuuid)) {
        if (servicemode)
            serviceuuid = xuuidstring();
        else
            return svcsyserror(__LINE__, 0, L"Missing -u <SVCBATCH_SERVICE_UUID> parameter", NULL);
    }
    if (IS_EMPTY_WCS(serviceuuid))
        return svcsyserror(__LINE__, GetLastError(), L"xuuidstring", NULL);
    if ((comspec = xgetenv(L"COMSPEC")) == NULL)
        return svcsyserror(__LINE__, ERROR_ENVVAR_NOT_FOUND, L"COMSPEC", NULL);
    if ((orgpath = xgetenv(L"PATH")) == NULL)
        return svcsyserror(__LINE__, ERROR_ENVVAR_NOT_FOUND, L"PATH", NULL);

    xcleanwinpath(comspec, 0);
    xcleanwinpath(orgpath, 1);

    if (isrelativepath(batchparam)) {
        if (IS_EMPTY_WCS(shomeparam)) {
            /**
             * Batch file is not absolute path
             * and we don't have provided workdir.
             * Use exelocation as cwd
             */
            servicehome = exelocation;
        }
        else {
            if (isrelativepath(shomeparam)) {
                if (!SetCurrentDirectoryW(exelocation))
                    return svcsyserror(__LINE__, GetLastError(), exelocation, NULL);
            }
            servicehome = getrealpathname(shomeparam, 1);
            if (IS_EMPTY_WCS(servicehome))
                return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, shomeparam, NULL);
        }
    }
    else {
        if (resolvebatchname(batchparam) == 0)
            return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, batchparam, NULL);

        if (IS_EMPTY_WCS(shomeparam)) {
            /**
             * Batch file is an absolute path
             * and we don't have provided workdir.
             * Use servicebase as cwd
             */
            servicehome = servicebase;
        }
        else {
            if (isrelativepath(shomeparam)) {
                if (!SetCurrentDirectoryW(servicebase))
                    return svcsyserror(__LINE__, GetLastError(), servicebase, NULL);
            }
            servicehome = getrealpathname(shomeparam, 1);
            if (IS_EMPTY_WCS(servicehome))
                return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, shomeparam, NULL);
        }
    }
    if (!SetCurrentDirectoryW(servicehome))
        return svcsyserror(__LINE__, GetLastError(), servicehome, NULL);

    if (resolvebatchname(batchparam) == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, batchparam, NULL);

    xfree(batchparam);
    xfree(shomeparam);
    if (IS_EMPTY_WCS(loglocation))
        loglocation = xwcsconcat(servicehome, L"\\" SVCBATCH_LOG_BASE);
    if (svcendparam != NULL) {
        shutdownfile = getrealpathname(svcendparam, 0);
        if (IS_EMPTY_WCS(shutdownfile))
            return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, svcendparam, NULL);
    }

    dupwenvp = waalloc(envc + 10);
    for (i = 0; i < envc; i++) {
        const wchar_t **e = removeenv;
        const wchar_t  *p = wenv[i];

        while (*e != NULL) {
            if (xwcsisenvvar(p, *e)) {
                p = NULL;
                break;
            }
            e++;
        }
        if (p != NULL)
            dupwenvp[dupwenvc++] = xwcsdup(p);
    }

    dupwenvp[dupwenvc++] = xwcsconcat(L"COMSPEC=", comspec);
    dupwenvp[dupwenvc++] = xwcsconcat(L"PATH=",    orgpath);
    xfree(orgpath);

    rv = resolverotate(rotateparam);
    if (rv != 0)
        return svcsyserror(rv, 0, L"Cannot resolve", rotateparam);
    memset(&ssvcstatus, 0, sizeof(SERVICE_STATUS));
    memset(&sazero,     0, sizeof(SECURITY_ATTRIBUTES));
    sazero.nLength = DSIZEOF(SECURITY_ATTRIBUTES);
    /**
     * Create logic state events
     */
    svcstopended = CreateEventW(&sazero, TRUE, TRUE,  NULL);
    if (IS_INVALID_HANDLE(svcstopended))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent", NULL);
    shutdowndone = CreateEventW(&sazero, TRUE, TRUE,  NULL);
    if (IS_INVALID_HANDLE(shutdowndone))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent", NULL);
    ssignalevent = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(ssignalevent))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent", NULL);
    processended = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(processended))
        return svcsyserror(__LINE__, GetLastError(), L"CreateEvent", NULL);
    if (servicemode) {
        monitorevent = CreateEventW(&sazero, TRUE, FALSE, NULL);
        if (IS_INVALID_HANDLE(monitorevent))
            return svcsyserror(__LINE__, GetLastError(), L"CreateEvent", NULL);
    }

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
                return svcsyserror(__LINE__, GetLastError(), L"GetStdHandle", NULL);
            dbgprints(__FUNCTION__, "allocated new console");
        }
        else {
            return svcsyserror(__LINE__, GetLastError(), L"AllocConsole", NULL);
        }
    }
    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = NULL;
    se[1].lpServiceProc = NULL;
    if (servicemode) {
        dbgprints(__FUNCTION__, "starting service");
        if (!StartServiceCtrlDispatcherW(se))
            rv = svcsyserror(__LINE__, GetLastError(), L"StartServiceCtrlDispatcher", NULL);
    }
    else {
        dbgprints(__FUNCTION__, "starting shutdown");
        servicemain(0, NULL);
        rv = ssvcstatus.dwServiceSpecificExitCode;
    }
    dbgprints(__FUNCTION__, "done");
    return rv;
}
