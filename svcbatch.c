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
#include <errno.h>
#include "svcbatch.h"

static volatile LONG         monitorsig  = 0;
static volatile LONG         sstarted    = 0;
static volatile LONG         sscstate    = SERVICE_START_PENDING;
static volatile LONG         rotatecount = 0;
static volatile LONG         logwritten  = 0;
static volatile LONG64       loglastwr   = CPP_INT64_C(0);
static volatile HANDLE       logfhandle  = NULL;
static SERVICE_STATUS_HANDLE hsvcstatus  = NULL;
static SERVICE_STATUS        ssvcstatus;
static CRITICAL_SECTION      servicelock;
static CRITICAL_SECTION      logfilelock;
static SECURITY_ATTRIBUTES   sazero;
static LONGLONG              rotateint   = SVCBATCH_LOGROTATE_DEF;
static LARGE_INTEGER         rotatetmo   = {{ 0, 0 }};
static LARGE_INTEGER         rotatesiz   = {{ 0, 0 }};
static BYTE                  ioreadbuffer[HBUFSIZ];

static wchar_t  *comspec          = NULL;
static wchar_t **dupwenvp         = NULL;
static int       dupwenvc         = 0;
static wchar_t  *wenvblock        = NULL;
static int       hasdebuginfo     = SVCBATCH_ISDEV_VERSION;
static int       hasctrlbreak     = 0;
static int       haslogstatus     = 1;
static int       haslogrotate     = 1;
static int       haspipedlogs     = 0;
static int       autorotate       = 0;
static int       uselocaltime     = 0;
static int       servicemode      = 1;
static int       svcmaxlogs       = SVCBATCH_MAX_LOGS;
static DWORD     preshutdown      = 0;
static DWORD     childprocpid     = 0;
static DWORD     pipedprocpid     = 0;

static int       xwoptind         = 1;   /* Index into parent argv vector */
static int       xwoption         = 0;   /* Character checked for validity */

static wchar_t   svcbatchexe[HBUFSIZ];
static wchar_t  *svcbatchfile     = NULL;
static wchar_t  *svcbatchname     = NULL;
static wchar_t  *shutdownfile     = NULL;
static wchar_t  *exelocation      = NULL;
static wchar_t  *servicebase      = NULL;
static wchar_t  *servicename      = NULL;
static wchar_t  *servicehome      = NULL;
static wchar_t  *serviceuuid      = NULL;
static wchar_t  *svcbatchargs     = NULL;

static wchar_t  *loglocation      = NULL;
static wchar_t  *logfilename      = NULL;
static wchar_t  *logfilepart      = NULL;
static wchar_t  *logredirect      = NULL;

static ULONGLONG logtickcount     = CPP_UINT64_C(0);

static HANDLE    childprocjob     = NULL;
static HANDLE    childprocess     = NULL;
static HANDLE    svcstopended     = NULL;
static HANDLE    ssignalevent     = NULL;
static HANDLE    rsignalevent     = NULL;
static HANDLE    processended     = NULL;
static HANDLE    monitorevent     = NULL;
static HANDLE    logrotatesig     = NULL;
static HANDLE    outputpiperd     = NULL;
static HANDLE    inputpipewrs     = NULL;
static HANDLE    pipedprocjob     = NULL;
static HANDLE    pipedprocess     = NULL;
static HANDLE    logrhandle       = NULL;

static wchar_t      zerostring[4] = { L'\0', L'\0', L'\0', L'\0' };
static wchar_t      CRLFW[4]      = { L'\r', L'\n', L'\0', L'\0' };
static char         CRLFA[4]      = { '\r', '\n', '\0', '\0' };
static char         YYES[4]       = { 'Y', '\r', '\n', '\0'  };

static const char    *cnamestamp  = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;
static const wchar_t *wnamestamp  = CPP_WIDEN(SVCBATCH_NAME) L" " CPP_WIDEN(SVCBATCH_VERSION_TXT);
static const wchar_t *cwsappname  = CPP_WIDEN(SVCBATCH_APPNAME);
static const wchar_t *svclogfname = SVCBATCH_LOGNAME;
static const wchar_t *rotateparam = NULL;
static const wchar_t *logdirparam = NULL;
static const wchar_t *xwoptarg    = NULL;

static wchar_t *xwmalloc(size_t size)
{
    wchar_t *p = (wchar_t *)malloc((size + 2) * sizeof(wchar_t));
    if (p == NULL) {
        _wperror(L"xwmalloc");
        _exit(1);
    }
    p[size] = L'\0';
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

static wchar_t **waalloc(size_t size)
{
    wchar_t **p = (wchar_t **)calloc(size + 2, sizeof(wchar_t *));
    if (p == NULL) {
        _wperror(L"waalloc");
        _exit(1);
    }
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
    p = xwmalloc(n);
    wmemcpy(p, s, n);
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

static wchar_t *xwcsconcat(const wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp, *rv;
    int l1 = xwcslen(s1);
    int l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;

    cp = rv = xwmalloc(l1 + l2);

    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    cp += l1;
    if(l2 > 0)
        wmemcpy(cp, s2, l2);
    return rv;
}

static wchar_t *xwcsmkpath(const wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp;
    int l1 = xwcslen(s1);
    int l2 = xwcslen(s2);

    if ((l1 == 0) || (l2 == 0))
        return NULL;

    cp = xwmalloc(l1 + l2 + 1);

    wmemcpy(cp, s1, l1);
    cp[l1] = L'\\';
    wmemcpy(cp + l1 + 1, s2, l2);
    return cp;
}

static wchar_t *xwcsappend(wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp, *rv;
    int l1 = xwcslen(s1);
    int l2 = xwcslen(s2);

    if (l2 == 0)
        return s1;

    cp = rv = xwmalloc(l1 + l2);

    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    wmemcpy(cp + l1, s2, l2);
    xfree(s1);
    return rv;
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

static wchar_t *xappendarg(int q, wchar_t *s1, const wchar_t *s2)
{
    wchar_t *cp, *rv;
    int i, l1, l2;
    int nq = 0, lx = 4;

    l2 = xwcslen(s2);
    if (l2 == 0)
        return s1;

    l1 = xwcslen(s1);
    if (q) {
        for (i = 0; i < l2; i++) {
            if (s2[i] == L'"')
                lx++;
            else if (iswspace(s2[i]))
                nq = 1;
        }
    }
    cp = rv = xwmalloc(l1 + l2 + lx);

    if(l1 > 0) {
        wmemcpy(cp, s1, l1);
        cp += l1;
        *(cp++) = L' ';
    }
    if (nq) {
        *(cp++) = L'"';
        for (i = 0; i < l2; i++) {
            if (s2[i] == L'"')
                *(cp++) = L'\\';
            *(cp++) = s2[i];
        }
        *(cp++) = L'"';
    }
    else {
        wmemcpy(cp, s2, l2);
        cp += l2;
    }
    *cp = L'\0';
    xfree(s1);
    return rv;
}

int xwgetopt(int nargc, const wchar_t **nargv, const wchar_t *opts)
{
    static const wchar_t *place = zerostring;
    const wchar_t *oli = NULL;

    xwoptarg = NULL;
    if (*place == L'\0') {

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
        if (*place == L'\0') {
            ++xwoptind;
            place = zerostring;
            return L'!';
        }
    }
    xwoption = *(place++);

    if (xwoption != L':') {
        oli = wcschr(opts, towlower((wchar_t)xwoption));
    }
    if (oli == NULL) {
        if (*place == L'\0') {
            ++xwoptind;
            place = zerostring;
        }
        return L'!';
    }

    /* Does this option need an argument? */
    if (oli[1] == L':') {
        /*
         * Option-argument is either the rest of this argument
         * or the entire next argument.
         */
        if (*place != L'\0') {
            xwoptarg = place;
        }
        else if (oli[2] != L':') {
            if (nargc > ++xwoptind) {
                xwoptarg = nargv[xwoptind];
            }
        }
        ++xwoptind;
        place = zerostring;
        if (IS_EMPTY_WCS(xwoptarg)) {
            /* Option-argument is absent or empty */
            return L':';
        }
    }
    else {
        /* Don't need argument */
        if (*place == L'\0') {
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

    for (i = 0; s[i] != L'\0'; i++) {
        if (s[i] == L'/')
            s[i] =  L'\\';
    }
    --i;
    while (i > 1) {
        if (iswspace(s[i]))
            s[i--] = L'\0';
        else
            break;

    }
    if (isdir) {
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
    b = xwmalloc(38);
    for (i = 0, x = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            b[x++] = '-';
        b[x++] = xb16[d[i] >> 4];
        b[x++] = xb16[d[i] & 0x0F];
    }
    b[x] = L'\0';
    return b;
}

static void dbgprints(const char *funcname, const char *string)
{
    if (hasdebuginfo) {
        char buf[SBUFSIZ];
        _snprintf(buf, SBUFSIZ - 1,
                  "[%.4lu] %d %-16s %s",
                  GetCurrentThreadId(),
                  servicemode,
                  funcname, string);
         buf[SBUFSIZ - 1] = '\0';
         OutputDebugStringA(buf);
    }
}

static void dbgprintf(const char *funcname, const char *format, ...)
{
    if (hasdebuginfo) {
        char    buf[SBUFSIZ];
        va_list ap;

        va_start(ap, format);
        _vsnprintf(buf, SBUFSIZ - 1, format, ap);
        va_end(ap);
        buf[SBUFSIZ - 1] = '\0';
        dbgprints(funcname, buf);
    }
}

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
    }
}

static int setupeventlog(void)
{
    static int ssrv = 0;
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
        return 0;
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

static DWORD svcsyserror(const char *fn, int line, DWORD ern, const wchar_t *err, const wchar_t *eds)
{
    wchar_t        hdr[BBUFSIZ];
    wchar_t        erb[MBUFSIZ];
    const wchar_t *errarg[10];
    int            i = 0;

    errarg[i++] = wnamestamp;
    if (servicename)
        errarg[i++] = servicename;
    errarg[i++] = L"reported the following error:\r\n";
    _snwprintf(hdr, BBUFSIZ, L"svcbatch.c(%d, %S)", line, fn);
    errarg[i++] = hdr;
    errarg[i++] = err;

    if (ern) {
        int c = _snwprintf(erb, BBUFSIZ, L"(err=%lu) ", ern);
        xwinapierror(erb + c, MBUFSIZ - c, ern);
        errarg[i++] = erb;
    }
    else {
        erb[0] = L'\0';
        erb[1] = L'\0';
        ern = ERROR_INVALID_PARAMETER;
    }
    if (eds != NULL) {
        errarg[i++] = eds;
        dbgprintf(fn, "%S %S %S: %S",  hdr, err, erb, eds);
    }
    else {
        dbgprintf(fn, "%S %S %S", hdr, err, erb);
    }
    errarg[i++] = CRLFW;
    while (i < 10) {
        errarg[i++] = L"";
    }
    if (setupeventlog()) {
        HANDLE es = RegisterEventSourceW(NULL, CPP_WIDEN(SVCBATCH_NAME));
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
    if (*str == L'\0')
        return NULL;
    for (siz = 0; str[siz] != L'\0'; siz++) {
        if (str[siz] == L'%')
            len++;
    }
    if (len < 2) {
        buf = xwmalloc(siz);
        wmemcpy(buf, str, siz);
    }
    while (buf == NULL) {
        buf = xwmalloc(siz);
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
    if ((es == NULL) || (servicemode == 0))
        return es;

    fh = CreateFileW(es, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, fa, NULL);
    xfree(es);
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
            svcbatchname = svcbatchfile + i + 1;
            servicebase  = xwcsdup(svcbatchfile);
            svcbatchfile[i] = L'\\';
            return 1;
        }
    }
    return 0;
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
        svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"SetServiceStatus", NULL);
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
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreatePipe", NULL);
    if (!DuplicateHandle(cp, sh, cp,
                         iwrs, FALSE, 0,
                         DUPLICATE_SAME_ACCESS)) {
        rc = svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"DuplicateHandle", NULL);
        goto finished;
    }
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
        rc = svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"DuplicateHandle", NULL);
    si->hStdOutput = si->hStdError;

finished:
    SAFE_CLOSE_HANDLE(sh);
    return rc;
}

static DWORD logappend(HANDLE h, LPCVOID buf, DWORD len)
{
    DWORD wr;
    LARGE_INTEGER ee = {{ 0, 0 }};

    if (haspipedlogs == 0) {
        if (!SetFilePointerEx(h, ee, NULL, FILE_END))
            return GetLastError();
    }
    if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0)) {
        if (InterlockedAdd(&logwritten, wr) >= SVCBATCH_LOGFLUSH_SIZE) {
            FlushFileBuffers(h);
            InterlockedExchange(&logwritten, 0);
        }
        InterlockedExchange64(&loglastwr, GetTickCount64());
        return 0;
    }
    else {
        return GetLastError();
    }
}

static DWORD logfflush(HANDLE h)
{
    DWORD wr;
    LARGE_INTEGER ee = {{ 0, 0 }};

    if (haspipedlogs == 0) {
        if (!SetFilePointerEx(h, ee, NULL, FILE_END))
            return GetLastError();
    }
    if (WriteFile(h, CRLFA, 2, &wr, NULL) && (wr != 0))
        FlushFileBuffers(h);
    else
        return GetLastError();
    InterlockedExchange(&logwritten, 0);
    InterlockedExchange64(&loglastwr, GetTickCount64());
    if (haspipedlogs == 0) {
        if (!SetFilePointerEx(h, ee, NULL, FILE_END))
            return GetLastError();
    }
    return 0;
}

static DWORD logwrline(HANDLE h, const char *str)
{
    char    buf[BBUFSIZ];
    ULONGLONG ct;
    DWORD   ss, ms;
    DWORD   wr;

    ct = GetTickCount64() - logtickcount;
    ms = (DWORD)((ct % MS_IN_SECOND));
    ss = (DWORD)((ct / MS_IN_SECOND));

    memset(buf, 0, sizeof(buf));
    sprintf(buf,
            "[%.8lu.%.3lu] [%.4lu:%.4lu] ",
            ss, ms,
            GetCurrentProcessId(),
            GetCurrentThreadId());
    buf[BBUFSIZ - 1] = '\0';
    if (WriteFile(h, buf, (DWORD)strlen(buf), &wr, NULL) && (wr != 0))
        InterlockedAdd(&logwritten, wr);
    else
        return GetLastError();
    if (WriteFile(h, str, (DWORD)strlen(str), &wr, NULL) && (wr != 0))
        InterlockedAdd(&logwritten, wr);
    else
        return GetLastError();
    if (WriteFile(h, CRLFA, 2, &wr, NULL) && (wr != 0))
        InterlockedAdd(&logwritten, wr);
    else
        return GetLastError();

    return 0;
}

static DWORD logprintf(HANDLE h, const char *format, ...)
{
    char    buf[MBUFSIZ];
    va_list ap;

    memset(buf, 0, sizeof(buf));
    va_start(ap, format);
    _vsnprintf(buf, MBUFSIZ - 1, format, ap);
    va_end(ap);
    return logwrline(h, buf);
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
    if (svcbatchargs != NULL)
        logprintf(h, "      arguments  : %S", svcbatchargs);
    logprintf(h, "Base directory   : %S", servicebase);
    logprintf(h, "Working directory: %S", servicehome);
    if (loglocation != NULL)
        logprintf(h, "Log directory    : %S", loglocation);
    if (haspipedlogs)
        logprintf(h, "Log redirected to: %S", logredirect);

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

static DWORD createlogdir(void)
{
    if (logdirparam != NULL) {
        DWORD rc;

        loglocation = getrealpathname(logdirparam, 1);
        if (loglocation == NULL) {
            rc = xcreatepath(logdirparam);
            if (rc != 0)
                return svcsyserror(__FUNCTION__, __LINE__, rc, L"xcreatepath", logdirparam);
            loglocation = getrealpathname(logdirparam, 1);
            if (loglocation == NULL)
                return svcsyserror(__FUNCTION__, __LINE__, ERROR_PATH_NOT_FOUND, L"getrealpathname", logdirparam);
        }
        if (_wcsicmp(loglocation, servicehome) == 0) {
            svcsyserror(__FUNCTION__, __LINE__, 0,
                        L"Loglocation cannot be the same as servicehome",
                        loglocation);
            return ERROR_BAD_PATHNAME;
        }
    }
    return 0;
}

static DWORD openlogpipe(void)
{
    DWORD    rc;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;
    HANDLE wr = NULL;
    wchar_t *cmdline = NULL;
    wchar_t *workdir = loglocation;

    if (workdir == NULL)
        workdir = servicehome;

    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb      = DSIZEOF(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;

    rc = createiopipes(&si, &wr, &logrhandle);
    if (rc != 0) {
        return rc;
    }
    pipedprocjob = CreateJobObjectW(&sazero, NULL);
    if (IS_INVALID_HANDLE(pipedprocjob)) {
        rc = GetLastError();
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateJobObjectW", NULL);
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

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    cmdline = xappendarg(1, NULL,    logredirect);
    cmdline = xappendarg(1, cmdline, logfilepart);
    cmdline = xappendarg(0, cmdline, rotateparam);

    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);
    if (!CreateProcessW(logredirect, cmdline, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
                        wenvblock,
                        workdir,
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

    if (haslogstatus) {
        logwrline(wr, cnamestamp);
    }
    InterlockedExchangePointer(&logfhandle, wr);
    dbgprintf(__FUNCTION__, "running pipe log process: %lu", pipedprocpid);
    xfree(cmdline);

    return 0;

failed:
    SAFE_CLOSE_HANDLE(pipedprocess);
    SAFE_CLOSE_HANDLE(pipedprocjob);

    return rc;
}

static DWORD openlogfile(BOOL firstopen)
{
    wchar_t sfx[4] = { L'.', L'0', L'\0', L'\0' };
    wchar_t *logpb = NULL;
    DWORD rc;
    HANDLE h = NULL;
    int i, rotateold;

    if (!firstopen)
        logtickcount = GetTickCount64();
    if (autorotate)
        rotateold = 0;
    else
        rotateold = svcmaxlogs;
    if (logfilename == NULL)
        logfilename = xwcsmkpath(loglocation, logfilepart);

    if (svcmaxlogs > 0) {
        if (GetFileAttributesW(logfilename) != INVALID_FILE_ATTRIBUTES) {
            DWORD mm = 0;

            if (firstopen)
                reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
            if (autorotate) {
                SYSTEMTIME st;
                wchar_t wrb[24];

                if (uselocaltime)
                    GetLocalTime(&st);
                else
                    GetSystemTime(&st);
                _snwprintf(wrb, 22, L".%.4d-%.2d-%.2d.%.2d%.2d%.2d",
                           st.wYear, st.wMonth, st.wDay,
                           st.wHour, st.wMinute, st.wSecond);
                logpb = xwcsconcat(logfilename, wrb);
            }
            else {
                mm = MOVEFILE_REPLACE_EXISTING;
                logpb = xwcsconcat(logfilename, sfx);
            }
            if (!MoveFileExW(logfilename, logpb, mm)) {
                rc = GetLastError();
                xfree(logpb);
                return svcsyserror(__FUNCTION__, __LINE__, rc, L"MoveFileExW", logfilename);
            }
        }
        else {
            rc = GetLastError();
            if (rc != ERROR_FILE_NOT_FOUND) {
                return svcsyserror(__FUNCTION__, __LINE__, rc, L"GetFileAttributesW", logfilename);
            }
        }
    }
    if (firstopen)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    if (rotateold) {
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
                    svcsyserror(__FUNCTION__, __LINE__, rc, L"MoveFileExW", lognn);
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
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateFileW", logfilename);
        goto failed;
    }
    if (rc == ERROR_ALREADY_EXISTS) {
        dbgprintf(__FUNCTION__, "reusing: %S", logfilename);
        logfflush(h);
    }
    else {
        InterlockedExchange(&logwritten, 0);
        InterlockedExchange64(&loglastwr, GetTickCount64());
    }
    if (haslogstatus) {
        logwrline(h, cnamestamp);
        if (firstopen)
            logwrtime(h, "Log opened");
    }
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
    if (haslogstatus) {
        logfflush(h);
        logwrtime(h, "Log rotatation initialized");
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    rc = openlogfile(FALSE);
    if (rc == 0) {
        if (haslogstatus) {
            logwrtime(logfhandle, "Log rotated");
            logprintf(logfhandle, "Log generation   : %lu",
                      InterlockedIncrement(&rotatecount));
            logconfig(logfhandle);
        }
    }
    else {
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, 0, L"rotatelogs failed", NULL);
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
        if (haslogstatus) {
            logfflush(h);
            logwrtime(h, "Log closed");
        }
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    LeaveCriticalSection(&logfilelock);
    if (IS_VALID_HANDLE(pipedprocess)) {
        DWORD ws;

        SAFE_CLOSE_HANDLE(logrhandle);
        ws = WaitForSingleObject(pipedprocess, SVCBATCH_STOP_HINT);
        if (ws != WAIT_OBJECT_0)
            dbgprintf(__FUNCTION__, "wait for piped log returned: %lu", ws);
        SAFE_CLOSE_HANDLE(pipedprocess);
        SAFE_CLOSE_HANDLE(pipedprocjob);
    }

}

static int resolverotate(const wchar_t *str)
{
    wchar_t   *rp, *sp;

    if (IS_EMPTY_WCS(str)) {
        return 0;
    }
    if ((str[0] ==  L'@') && (str[1] ==  L'\0')) {
        /* Special case for shutdown autorotate */
        autorotate = 1;
        if (servicemode)
            return __LINE__;
        else
            return 0;
    }
    if ((str[0] >=  L'0') && (str[0] <=  L'9') && (str[1] ==  L'\0')) {
        svcmaxlogs = str[0] - L'0';
        dbgprintf(__FUNCTION__, "max rotate logs: %d", svcmaxlogs);
        return 0;
    }
    rotatetmo.QuadPart = rotateint;
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
            if ((mm < SVCBATCH_MIN_LOGRTIME) || (errno == ERANGE)) {
                dbgprintf(__FUNCTION__, "invalid rotate timeout %S", rp);
                return __LINE__;
            }
            rotateint = mm * ONE_MINUTE * CPP_INT64_C(-1);
            dbgprintf(__FUNCTION__, "rotate each %d minutes", mm);
            rotatetmo.QuadPart = rotateint;
            rp = p;
        }
        else {
            SYSTEMTIME     st;
            FILETIME       ft;
            ULARGE_INTEGER ui;

            *(p++) = L'\0';
            hh = _wtoi(rp);
            if ((hh < 0) || (hh > 23) || (errno == ERANGE))
                return __LINE__;
            rp = p;
            if ((p = wcschr(rp, L':')) == NULL)
                return __LINE__;
            *(p++) = L'\0';
            mm = _wtoi(rp);
            if ((mm < 0) || (mm > 59) || (errno == ERANGE))
                return __LINE__;
            rp = p;
            if ((p = wcschr(rp, L'~')) != NULL)
                *(p++) = L'\0';
            ss = _wtoi(rp);
            if (((ss < 0) || ss > 59) || (errno == ERANGE))
                return __LINE__;

            rp = p;
            rotateint = ONE_DAY;
            if (uselocaltime)
                GetLocalTime(&st);
            else
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
        LONGLONG len;
        LONGLONG siz;
        LONGLONG mux = CPP_INT64_C(1);
        wchar_t *ep  = zerostring;
        wchar_t  mm  = L'B';

        siz = _wcstoi64(rp, &ep, 10);
        if ((siz <= CPP_INT64_C(0)) || (ep == rp) || (errno == ERANGE))
            return __LINE__;
        if (*ep != '\0') {
            mm = towupper(ep[0]);
            switch (mm) {
                case L'B':
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
        }
        len = siz * mux;
        if (len < SVCBATCH_MIN_LOGSIZE)
            return __LINE__;
        if (hasdebuginfo) {
            if (mm != 'B')
                dbgprintf(__FUNCTION__, "rotate if > %lu %Cb", (DWORD)siz, mm);
            else
                dbgprintf(__FUNCTION__, "rotate if > %lu bytes", (DWORD)siz);
        }
        rotatesiz.QuadPart = len;
    }
    xfree(sp);
    autorotate = 1;
    return 0;
}


static int runshutdown(DWORD rt)
{
    wchar_t  xparam[10];
    wchar_t *cmdline;
    HANDLE   wh[2];
    HANDLE   job = NULL;
    DWORD    rc = 0;
    int      ip = 0;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji;

    dbgprints(__FUNCTION__, "started");
    cmdline = xappendarg(1, NULL, svcbatchexe);
    xparam[ip++] = L'-';
    xparam[ip++] = L'x';
    if (hasdebuginfo)
        xparam[ip++] = L'd';
    if (uselocaltime)
        xparam[ip++] = L'l';
    if (haslogstatus == 0)
        xparam[ip++] = L'q';
    if (haspipedlogs == 0) {
        xparam[ip++] = L'r';
        if (autorotate)
            xparam[ip++] = L'@';
        else
            xparam[ip++] = L'0' + svcmaxlogs;
    }
    xparam[ip] = L'\0';
    cmdline = xappendarg(0, cmdline, xparam);
    cmdline = xappendarg(0, cmdline, L"-z");
    cmdline = xappendarg(1, cmdline, servicename);
    cmdline = xappendarg(0, cmdline, L"-u");
    cmdline = xappendarg(0, cmdline, serviceuuid);
    cmdline = xappendarg(0, cmdline, L"-w");
    cmdline = xappendarg(1, cmdline, servicehome);
    if (loglocation != NULL) {
        cmdline = xappendarg(0, cmdline, L"-o");
        cmdline = xappendarg(1, cmdline, loglocation);
    }
    if (haspipedlogs) {
        cmdline = xappendarg(0, cmdline, L"-e");
        cmdline = xappendarg(1, cmdline, logredirect);
        if (rotateparam) {
            cmdline = xappendarg(0, cmdline, L"-r");
            cmdline = xappendarg(1, cmdline, rotateparam);
        }
    }
    cmdline = xappendarg(0, cmdline, L"-n");
    cmdline = xappendarg(1, cmdline, svclogfname);
    cmdline = xappendarg(1, cmdline, shutdownfile);
    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);

    memset(&ji, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));
    si.cb = DSIZEOF(STARTUPINFOW);

    ji.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    job = CreateJobObjectW(&sazero, NULL);
    if (IS_INVALID_HANDLE(job)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateJobObjectW", NULL);
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

    if (!CreateProcessW(svcbatchexe, cmdline, NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_CONSOLE,
                        wenvblock,
                        servicehome,
                       &si, &cp)) {
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

    dbgprintf(__FUNCTION__, "waiting for shutdown process: %lu to finish", cp.dwProcessId);
    rc = WaitForMultipleObjects(2, wh, FALSE, rt + SVCBATCH_STOP_STEP);
    switch (rc) {
        case WAIT_OBJECT_0:
            dbgprintf(__FUNCTION__, "shutdown process: %lu done",
                      cp.dwProcessId);
        break;
        case WAIT_OBJECT_1:
            dbgprintf(__FUNCTION__, "processended for: %lu",
                      cp.dwProcessId);
        break;
        case WAIT_TIMEOUT:
            dbgprintf(__FUNCTION__, "sending signal event to: %lu",
                      cp.dwProcessId);
            SetEvent(ssignalevent);
        break;
        default:
        break;
    }
    if (rc != WAIT_OBJECT_0) {
        if (WaitForSingleObject(cp.hProcess, rt) != WAIT_OBJECT_0) {
            dbgprintf(__FUNCTION__, "calling TerminateProcess for: %lu",
                       cp.dwProcessId);
            TerminateProcess(cp.hProcess, ERROR_BROKEN_PIPE);
        }
    }
    if (!GetExitCodeProcess(cp.hProcess, &rc)) {
        rc = GetLastError();
        dbgprintf(__FUNCTION__, "GetExitCodeProcess failed with: %lu", rc);
    }
finished:
    xfree(cmdline);
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(cp.hProcess);
    SAFE_CLOSE_HANDLE(job);

    dbgprints(__FUNCTION__, "done");
    return rc;
}

static unsigned int __stdcall stopthread(void *unused)
{
    if (servicemode)
        dbgprints(__FUNCTION__, "service stop");
    else
        dbgprints(__FUNCTION__, "shutdown stop ");
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
    if (shutdownfile != NULL) {
        DWORD rc;

        dbgprints(__FUNCTION__, "creating shutdown process");
        rc = runshutdown(SVCBATCH_STOP_CHECK);
        dbgprintf(__FUNCTION__, "runshutdown returned: %lu", rc);
        if (WaitForSingleObject(processended, 0) == WAIT_OBJECT_0) {
            dbgprints(__FUNCTION__, "processended by shutdown");
            goto finished;
        }
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
        if (rc == 0) {
            if (WaitForSingleObject(ssignalevent, 0) != WAIT_OBJECT_0) {
                dbgprints(__FUNCTION__, "wait for processended");
                if (WaitForSingleObject(processended, SVCBATCH_STOP_STEP) == WAIT_OBJECT_0) {
                    dbgprints(__FUNCTION__, "processended");
                    goto finished;
                }
            }
        }
    }
    dbgprintf(__FUNCTION__, "raising CTRL_C_EVENT for: %S", svcbatchname);
    if (SetConsoleCtrlHandler(NULL, TRUE)) {
        DWORD ws;

        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        ws = WaitForSingleObject(processended, SVCBATCH_STOP_STEP);
        SetConsoleCtrlHandler(NULL, FALSE);
        if (ws == WAIT_OBJECT_0) {
            dbgprints(__FUNCTION__, "processended by CTRL_C_EVENT");
            goto finished;
        }
    }
    else {
        dbgprintf(__FUNCTION__, "SetConsoleCtrlHandler failed %lu", GetLastError());
    }
    dbgprints(__FUNCTION__, "process still running");
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    dbgprints(__FUNCTION__, "child is still active ... terminating");
    SAFE_CLOSE_HANDLE(childprocess);
    SAFE_CLOSE_HANDLE(childprocjob);

finished:
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_CHECK);
    SetEvent(svcstopended);
    dbgprints(__FUNCTION__, "done");
    XENDTHREAD(0);
}

static void createstopthread(void)
{
    if (InterlockedIncrement(&sstarted) == 1) {
        ResetEvent(svcstopended);
        xcreatethread(1, 0, &stopthread, NULL);
    }
    else {
        dbgprints(__FUNCTION__, "already started");
    }
}

static unsigned int __stdcall rdpipethread(void *unused)
{
    DWORD rc = 0;

    dbgprints(__FUNCTION__, "started");
    while (rc == 0) {
        DWORD rd = 0;
        HANDLE h = NULL;

        if (ReadFile(outputpiperd, ioreadbuffer, DSIZEOF(ioreadbuffer), &rd, NULL)) {
            if (rd == 0) {
                rc = GetLastError();
            }
            else {
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);

                if (h != NULL)
                    rc = logappend(h, ioreadbuffer, rd);
                else
                    rc = ERROR_NO_MORE_FILES;

                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
            }
        }
        else {
            rc = GetLastError();
        }
    }
    if (rc) {
        if ((rc == ERROR_BROKEN_PIPE) || (rc == ERROR_NO_DATA))
            dbgprints(__FUNCTION__, "pipe closed");
        else if (rc == ERROR_NO_MORE_FILES)
            dbgprints(__FUNCTION__, "logfile closed");
        else
            dbgprintf(__FUNCTION__, "err=%lu", rc);
        if (WaitForSingleObject(childprocess, SVCBATCH_PENDING_WAIT) == WAIT_TIMEOUT) {
            if (GetExitCodeProcess(pipedprocess, &rc)) {
                dbgprintf(__FUNCTION__, "piped process exited with: %lu", rc);
            }
            else {
                rc = GetLastError();
                dbgprintf(__FUNCTION__, "piped GetExitCodeProcess failed with: %lu", rc);
            }
            if (InterlockedAdd(&sstarted, 0) == 0) {
                svcsyserror(__FUNCTION__, __LINE__, rc, L"log redirection process failed", NULL);
                dbgprints(__FUNCTION__, "terminating child process");
                TerminateProcess(childprocess, ERROR_PROCESS_ABORTED);
                dbgprints(__FUNCTION__, "terminated");
            }
            else {
                dbgprints(__FUNCTION__, "stop already started");
            }
        }
        else {
            dbgprints(__FUNCTION__, "done with childprocess");
        }
    }
    dbgprints(__FUNCTION__, "done");

    XENDTHREAD(0);
}

static unsigned int __stdcall wrpipethread(void *unused)
{
    DWORD  wr, rc = 0;

    dbgprints(__FUNCTION__, "started");

    if (WriteFile(inputpipewrs, YYES, 3, &wr, NULL) && (wr != 0)) {
        dbgprintf(__FUNCTION__, "send %lu chars to: %lu", wr, childprocpid);
        if (!FlushFileBuffers(inputpipewrs))
            rc = GetLastError();
    }
    else {
        rc = GetLastError();
    }

    if (rc) {
        if ((rc == ERROR_BROKEN_PIPE) || (rc == ERROR_NO_DATA))
            dbgprints(__FUNCTION__, "pipe closed");
        else
            dbgprintf(__FUNCTION__, "err=%lu", rc);
    }
    dbgprints(__FUNCTION__, "done");
    XENDTHREAD(0);
}

static void monitorshutdown(void)
{
    HANDLE wh[3];
    HANDLE h;
    DWORD  ws;

    dbgprints(__FUNCTION__, "started");

    wh[0] = processended;
    wh[1] = monitorevent;
    wh[2] = ssignalevent;

    ws = WaitForMultipleObjects(3, wh, FALSE, INFINITE);
    switch (ws) {
        case WAIT_OBJECT_0:
            dbgprints(__FUNCTION__, "processended signaled");
        break;
        case WAIT_OBJECT_1:
            dbgprints(__FUNCTION__, "monitorevent signaled");
        break;
        case WAIT_OBJECT_2:
            dbgprints(__FUNCTION__, "shutdown stop signaled");
            if (haslogstatus) {
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h != NULL) {
                    logfflush(h);
                    logwrline(h, "Received shutdown stop signal");
                }
                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
            }
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
            createstopthread();
        break;
        default:
        break;
    }
    dbgprints(__FUNCTION__, "done");
}

static void monitorservice(void)
{
    HANDLE wh[2];
    HANDLE h;
    DWORD  ws, rc = 0;

    dbgprints(__FUNCTION__, "started");

    wh[0] = processended;
    wh[1] = monitorevent;

    do {
        DWORD  cc;
        LONG64 ct;

        ws = WaitForMultipleObjects(2, wh, FALSE, SVCBATCH_MONITOR_STEP);
        switch (ws) {
            case WAIT_TIMEOUT:
                EnterCriticalSection(&logfilelock);
                ct = GetTickCount64() - InterlockedAdd64(&loglastwr, 0);
                if (ct >= SVCBATCH_LOGFLUSH_HINT) {
                    if (InterlockedAdd(&logwritten, 0) > 0) {
                        h = InterlockedExchangePointer(&logfhandle, NULL);
                        if (h != NULL) {
                            FlushFileBuffers(h);
                            dbgprints(__FUNCTION__, "logfile flushed");
                        }
                        InterlockedExchangePointer(&logfhandle, h);
                        InterlockedExchange(&logwritten, 0);
                    }
                    InterlockedExchange64(&loglastwr, GetTickCount64());
                }
                LeaveCriticalSection(&logfilelock);
            break;
            case WAIT_OBJECT_0:
                dbgprints(__FUNCTION__, "processended signaled");
                rc = 1;
            break;
            case WAIT_OBJECT_1:
                cc = (DWORD)InterlockedExchange(&monitorsig, 0);
                if (cc == 0) {
                    dbgprints(__FUNCTION__, "quit signaled");
                    rc = 1;
                }
                else if (cc == SVCBATCH_CTRL_BREAK) {
                    dbgprints(__FUNCTION__, "ctrl+break signaled");
                    if (haslogstatus) {

                        EnterCriticalSection(&logfilelock);
                        h = InterlockedExchangePointer(&logfhandle, NULL);

                        if (h != NULL) {
                            logfflush(h);
                            logwrline(h, "Signaled CTRL_BREAK_EVENT");
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
                    dbgprints(__FUNCTION__, "console ctrl+break send");
                }
                else {
                    dbgprintf(__FUNCTION__, "Unknown control: %lu", cc);
                }
                ResetEvent(monitorevent);
            break;
            default:
                rc = 1;
            break;
        }
    } while (rc == 0);

    dbgprints(__FUNCTION__, "done");
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
    HANDLE wh[3];
    HANDLE h = NULL;
    DWORD  wc, ms, rc = 0;

    dbgprintf(__FUNCTION__, "started");
    wh[0] = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (IS_INVALID_HANDLE(wh[0])) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateWaitableTimer", NULL);
        goto finished;
    }
    if (!SetWaitableTimer(wh[0], &rotatetmo, 0, NULL, NULL, 0)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"SetWaitableTimer", NULL);
        goto finished;
    }

    wc = WaitForSingleObject(processended, SVCBATCH_LOGROTATE_INIT);
    if (wc != WAIT_TIMEOUT) {
        if (wc == WAIT_OBJECT_0)
            dbgprints(__FUNCTION__, "processended signaled");
        else
            dbgprintf(__FUNCTION__, "processended: %lu", wc);
        goto finished;
    }
    dbgprints(__FUNCTION__, "running");

    if (rotatesiz.QuadPart)
        ms = SVCBATCH_LOGROTATE_STEP;
    else
        ms = INFINITE;

    wh[1] = logrotatesig;
    wh[2] = processended;
    while (rc == 0) {
        wc = WaitForMultipleObjects(3, wh, FALSE, ms);
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
                            dbgprints(__FUNCTION__, "rotate by size");
                            rc = rotatelogs();
                            if (rc != 0) {
                                setsvcstatusexit(rc);
                                createstopthread();
                            }
                            else {
                                if (rotateint != ONE_DAY) {
                                    CancelWaitableTimer(wh[0]);
                                    SetWaitableTimer(wh[0], &rotatetmo, 0, NULL, NULL, 0);
                                }
                            }
                        }
                    }
                    else {
                        rc = GetLastError();
                        CloseHandle(h);
                        setsvcstatusexit(rc);
                        svcsyserror(__FUNCTION__, __LINE__, rc, L"GetFileSizeEx", NULL);
                        createstopthread();
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
                else {
                    setsvcstatusexit(rc);
                    createstopthread();
                }
            break;
            case WAIT_OBJECT_1:
                dbgprints(__FUNCTION__, "rotate by signal");
                rc = rotatelogs();
                if (rc == 0) {
                    if (rotateint != ONE_DAY) {
                        CancelWaitableTimer(wh[0]);
                        SetWaitableTimer(wh[0], &rotatetmo, 0, NULL, NULL, 0);
                    }
                    ResetEvent(logrotatesig);
                }
                else {
                    setsvcstatusexit(rc);
                    createstopthread();
                }
            break;
            case WAIT_OBJECT_2:
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
    SAFE_CLOSE_HANDLE(wh[0]);
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

    dbgprints(__FUNCTION__, "started");
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    cmdline = xappendarg(1, NULL, comspec);
    cmdline = xappendarg(0, cmdline, L"/D /C");
    if (svcbatchargs != NULL) {
        wchar_t *b = xappendarg(1, NULL, svcbatchfile);
        cmdline = xwcsappend(cmdline, L" \"");
        cmdline = xwcsappend(cmdline, b);
        cmdline = xappendarg(0, cmdline, svcbatchargs);
        cmdline = xwcsappend(cmdline, L"\"");
        xfree(b);
    }
    else {
        cmdline = xappendarg(1, cmdline, svcbatchfile);
    }
    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);
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
    childprocjob = CreateJobObjectW(&sazero, NULL);
    if (IS_INVALID_HANDLE(childprocjob)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__FUNCTION__, __LINE__, rc, L"CreateJobObjectW", NULL);
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
    dbgprintf(__FUNCTION__, "running child pid: %lu", childprocpid);
    if (haslogrotate) {
        xcreatethread(1, 0, &rotatethread, NULL);
    }
    WaitForMultipleObjects(3, wh, TRUE, INFINITE);
    CloseHandle(wh[1]);
    CloseHandle(wh[2]);

    dbgprintf(__FUNCTION__, "finished %S pid: %lu",
              svcbatchname, childprocpid);
    if (!GetExitCodeProcess(childprocess, &rc)) {
        rc = GetLastError();
        dbgprintf(__FUNCTION__, "GetExitCodeProcess failed with: %lu", rc);
    }
    if (rc) {
        if (servicemode) {
            if (rc != ERROR_EA_LIST_INCONSISTENT) {
                /**
                  * 255 is exit code when CTRL_C is send to cmd.exe
                  */
                dbgprintf(__FUNCTION__, "%S exited with: %lu",
                          svcbatchname, rc);
                setsvcstatusexit(ERROR_PROCESS_ABORTED);
            }
        }
        else {
            setsvcstatusexit(rc);
            dbgprintf(__FUNCTION__, "%S exited with: %lu",
                      svcbatchname, rc);
        }
    }

finished:
    SAFE_CLOSE_HANDLE(cp.hThread);
    SAFE_CLOSE_HANDLE(si.hStdInput);
    SAFE_CLOSE_HANDLE(si.hStdError);
    SetEvent(processended);

    dbgprints(__FUNCTION__, "done");
    XENDTHREAD(0);
}

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    BOOL rv = TRUE;

    if (hasdebuginfo) {
        switch (ctrl) {
            case CTRL_CLOSE_EVENT:
                dbgprints(__FUNCTION__, "signaled CTRL_CLOSE_EVENT");
            break;
            case CTRL_SHUTDOWN_EVENT:
                dbgprints(__FUNCTION__, "signaled CTRL_SHUTDOWN_EVENT");
            break;
            case CTRL_C_EVENT:
                dbgprints(__FUNCTION__, "signaled CTRL_C_EVENT");
            break;
            case CTRL_BREAK_EVENT:
                dbgprints(__FUNCTION__, "signaled CTRL_BREAK_EVENT");
            break;
            case CTRL_LOGOFF_EVENT:
                dbgprints(__FUNCTION__, "signaled CTRL_LOGOFF_EVENT");
            break;
            default:
                dbgprintf(__FUNCTION__, "unknown ctrl: %lu", ctrl);
                rv = FALSE;
            break;
        }
    }
    return rv;
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
            /* fall through */
        case SERVICE_CONTROL_SHUTDOWN:
            /* fall through */
        case SERVICE_CONTROL_STOP:
            dbgprints(__FUNCTION__, msg);
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_WAIT);
            if (haslogstatus) {
                EnterCriticalSection(&logfilelock);
                h = InterlockedExchangePointer(&logfhandle, NULL);
                if (h != NULL) {
                    logfflush(h);
                    logwrline(h, msg);
                }
                InterlockedExchangePointer(&logfhandle, h);
                LeaveCriticalSection(&logfilelock);
            }
            createstopthread();
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
            if (IS_VALID_HANDLE(rsignalevent)) {
                dbgprintf(__FUNCTION__, "signaled rotate to: %lu", pipedprocpid);
                SetEvent(rsignalevent);
                return 0;
            }
            if (IS_VALID_HANDLE(logrotatesig)) {
                /**
                 * Signal to rotatethread that
                 * user send custom service control
                 */
                dbgprints(__FUNCTION__, "signaled SVCBATCH_CTRL_ROTATE");
                SetEvent(logrotatesig);
                return 0;
            }
            dbgprints(__FUNCTION__, "log rotation is disabled");
            return ERROR_CALL_NOT_IMPLEMENTED;
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
    int          i, n, x;
    DWORD        rv = 0;
    int          eblen = 0;
    int          ebsiz[64];
    wchar_t     *ep;
    HANDLE       wh[4] = { NULL, NULL, NULL, NULL };
    DWORD        ws;

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
        hsvcstatus = RegisterServiceCtrlHandlerExW(servicename, servicehandler, NULL);
        if (IS_INVALID_HANDLE(hsvcstatus)) {
            svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"RegisterServiceCtrlHandlerEx", NULL);
            exit(ERROR_INVALID_HANDLE);
            return;
        }
        dbgprintf(__FUNCTION__, "started %S", servicename);
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

        rv = createlogdir();
        if (rv != 0) {
            svcsyserror(__FUNCTION__, __LINE__, 0, L"openlog failed", NULL);
            reportsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        dupwenvp[dupwenvc++] = xwcsdup(L"SVCBATCH_SERVICE_MODE=1");
    }
    else {
        dbgprintf(__FUNCTION__, "shutting down %S", servicename);

        loglocation = xwcsdup(logdirparam);
        dupwenvp[dupwenvc++] = xwcsdup(L"SVCBATCH_SERVICE_MODE=0");
    }
    /**
     * Add additional environment variables
     * They are unique to this service instance
     */
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_BASE=", servicebase);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_HOME=", servicehome);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_NAME=", servicename);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_UUID=", serviceuuid);
    if (loglocation != NULL)
        dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_LOGDIR=", loglocation);

    qsort((void *)dupwenvp, dupwenvc, sizeof(wchar_t *), envsort);
    for (i = 0, x = 0; i < dupwenvc; i++, x++) {
        n = xwcslen(dupwenvp[i]);
        if (x < 64)
            ebsiz[x] = n;
        eblen += (n + 1);
    }
    wenvblock = xwcalloc(eblen);
    for (i = 0, x = 0, ep = wenvblock; i < dupwenvc; i++, x++) {
        if (x < 64)
            n = ebsiz[x];
        else
            n = xwcslen(dupwenvp[i]);
        wmemcpy(ep, dupwenvp[i], n);
        ep += (n + 1);
    }
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    if (haspipedlogs) {
        rv = openlogpipe();
    }
    else {
        rv = openlogfile(TRUE);
    }
    if (rv != 0) {
        svcsyserror(__FUNCTION__, __LINE__, 0, L"openlog failed", NULL);
        reportsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    if (haslogstatus)
        logconfig(logfhandle);
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
    dbgprints(__FUNCTION__, "running");
    WaitForMultipleObjects(2, wh, TRUE, INFINITE);
    SAFE_CLOSE_HANDLE(wh[0]);
    SAFE_CLOSE_HANDLE(wh[1]);

    if (WaitForSingleObject(svcstopended, 0) == WAIT_OBJECT_0) {
        dbgprints(__FUNCTION__, "stopped");
    }
    else {
        dbgprints(__FUNCTION__, "waiting for stopthread to finish");
        ws = WaitForSingleObject(svcstopended, SVCBATCH_STOP_HINT);
        if (ws == WAIT_TIMEOUT) {
            if (shutdownfile != NULL) {
                dbgprints(__FUNCTION__, "sending shutdown stop signal");
                SetEvent(ssignalevent);
                ws = WaitForSingleObject(svcstopended, SVCBATCH_STOP_CHECK);
            }
        }
        dbgprintf(__FUNCTION__, "stopthread status: %lu", ws);
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
    SAFE_CLOSE_HANDLE(ssignalevent);
    SAFE_CLOSE_HANDLE(rsignalevent);
    SAFE_CLOSE_HANDLE(monitorevent);
    SAFE_CLOSE_HANDLE(logrotatesig);
    SAFE_CLOSE_HANDLE(childprocjob);
    SAFE_CLOSE_HANDLE(pipedprocjob);

    DeleteCriticalSection(&logfilelock);
    DeleteCriticalSection(&servicelock);
}

int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
{
    int         i;
    int         opt;
    int         envc  = 0;
    int         rv    = 0;
    wchar_t     bb[2] = { L'\0', L'\0' };
    HANDLE      h;
    SERVICE_TABLE_ENTRYW se[2];
    const wchar_t *batchparam  = NULL;
    const wchar_t *shomeparam  = NULL;
    const wchar_t *svcendparam = NULL;
    const wchar_t *lredirparam = NULL;

    /**
     * Make sure children (cmd.exe) are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    i = GetModuleFileNameW(NULL, svcbatchexe, HBUFSIZ);
    if ((i == 0) || (i > (HBUFSIZ - 4))) {
        /**
         * Guard against installations with large paths
         */
        return ERROR_INSUFFICIENT_BUFFER;
    }
    else {
        while (--i > 0) {
            if (svcbatchexe[i] == L'\\') {
                svcbatchexe[i] = L'\0';
                exelocation = xwcsdup(svcbatchexe);
                svcbatchexe[i] = L'\\';
            }
        }
    }
    if (argc == 1) {
        fputs(cnamestamp, stdout);
        fputs("\n\nVisit " SVCBATCH_PROJECT_URL " for more details", stdout);

        return 0;
    }
    /**
     * Check if running as service or as a child process.
     */
    if (argc > 6) {
        const wchar_t *p = wargv[1];
        if ((p[0] == L'-') && (p[1] == L'x')) {
            servicemode  = 0;
            cnamestamp = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT ;
            cwsappname = CPP_WIDEN(SHUTDOWN_APPNAME);
            wnamestamp = CPP_WIDEN(SHUTDOWN_APPNAME) L" " CPP_WIDEN(SVCBATCH_VERSION_TXT);
        }
    }
    logtickcount = GetTickCount64();
    dbgprints(__FUNCTION__, cnamestamp);
    if (wenv != NULL) {
        while (wenv[envc] != NULL)
            ++envc;
    }
    if (envc == 0)
        return svcsyserror(__FUNCTION__, __LINE__, 0, L"System", L"Missing environment");

    while ((opt = xwgetopt(argc, wargv, L"bdlqpe:o:r:s:w:n:u:xz:")) != EOF) {
        switch (opt) {
            case L'b':
                hasctrlbreak = 1;
            break;
            case L'd':
                hasdebuginfo = 1;
            break;
            case L'l':
                uselocaltime = 1;
            break;
            case L'q':
                haslogstatus = 0;
            break;
            case L'o':
                logdirparam = xwoptarg;
            break;
            case L'e':
                lredirparam  = xwoptarg;
            break;
            case L'n':
                svclogfname  = xwoptarg;
            break;
            case L'p':
                preshutdown  = SERVICE_ACCEPT_PRESHUTDOWN;
            break;
            case L'r':
                rotateparam  = xwoptarg;
            break;
            case L's':
                svcendparam  = xwoptarg;
            break;
            case L'w':
                shomeparam   = xwoptarg;
            break;
            /**
             * Private options
             */
            case L'z':
                servicename  = xwcsdup(xwoptarg);
            break;
            case L'u':
                serviceuuid  = xwcsdup(xwoptarg);
            break;
            case L'x':
                servicemode  = 0;
            break;
            case L':':
                bb[0] = xwoption;
                return svcsyserror(__FUNCTION__, __LINE__, 0, L"Invalid command line option value", bb);
            break;
            default:
                bb[0] = xwoption;
                return svcsyserror(__FUNCTION__, __LINE__, 0, L"Unknown command line option", bb);
            break;

        }
    }
    argc  -= xwoptind;
    wargv += xwoptind;
    if (argc > 0) {
        batchparam = wargv[0];
        for (i = 1; i < argc; i++) {
            /**
             * Add arguments for batch file
             */
            svcbatchargs = xappendarg(1, svcbatchargs,  wargv[i]);
        }
    }
    if (IS_EMPTY_WCS(batchparam))
        return svcsyserror(__FUNCTION__, __LINE__, 0, L"Missing batch file", NULL);
    if (IS_EMPTY_WCS(serviceuuid)) {
        if (servicemode)
            serviceuuid = xuuidstring();
        else
            return svcsyserror(__FUNCTION__, __LINE__, 0, L"Runtime error",
                               L"Missing -u <SVCBATCH_SERVICE_UUID> parameter");
    }
    if (IS_EMPTY_WCS(serviceuuid))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"xuuidstring", NULL);
    if ((comspec = xgetenv(L"COMSPEC")) == NULL)
        return svcsyserror(__FUNCTION__, __LINE__, ERROR_ENVVAR_NOT_FOUND, L"COMSPEC", NULL);

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
                    return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), exelocation, NULL);
            }
            servicehome = getrealpathname(shomeparam, 1);
            if (IS_EMPTY_WCS(servicehome))
                return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, shomeparam, NULL);
        }
    }
    else {
        if (resolvebatchname(batchparam) == 0)
            return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, batchparam, NULL);

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
                    return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), servicebase, NULL);
            }
            servicehome = getrealpathname(shomeparam, 1);
            if (IS_EMPTY_WCS(servicehome))
                return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, shomeparam, NULL);
        }
    }
    if (!SetCurrentDirectoryW(servicehome))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), servicehome, NULL);

    if (resolvebatchname(batchparam) == 0)
        return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, batchparam, NULL);

    if (IS_EMPTY_WCS(lredirparam)) {
        if (logdirparam == NULL)
            logdirparam = SVCBATCH_LOG_BASE;

        rv = resolverotate(rotateparam);
        if (rv != 0)
            return svcsyserror(__FUNCTION__, rv, 0, L"Cannot resolve", rotateparam);
    }
    else {
        logredirect = getrealpathname(lredirparam, 0);
        if (IS_EMPTY_WCS(logredirect))
            return svcsyserror(__FUNCTION__, __LINE__, ERROR_PATH_NOT_FOUND, lredirparam, NULL);
        haspipedlogs = 1;
        svcmaxlogs   = 0;
    }

    if (servicemode) {
        logfilepart = xwcsconcat(svclogfname, SVCBATCH_LOGFEXT);
        if (svcendparam) {
            shutdownfile = getrealpathname(svcendparam, 0);
            if (IS_EMPTY_WCS(shutdownfile))
                return svcsyserror(__FUNCTION__, __LINE__, ERROR_FILE_NOT_FOUND, svcendparam, NULL);
        }
        haslogrotate = svcmaxlogs;
    }
    else {
        logfilepart  = xwcsconcat(svclogfname, SHUTDOWN_LOGFEXT);
        haslogrotate = 0;
    }

    dupwenvp = waalloc(envc + 8);
    for (i = 0; i < envc; i++) {
        /**
         * Remove all environment variables
         * starting with SVCBATCH_SERVICE_
         */
        if (_wcsnicmp(wenv[i], L"SVCBATCH_SERVICE_", 17) < 0)
            dupwenvp[dupwenvc++] = xwcsdup(wenv[i]);
    }

    memset(&ssvcstatus, 0, sizeof(SERVICE_STATUS));
    memset(&sazero,     0, sizeof(SECURITY_ATTRIBUTES));
    sazero.nLength = DSIZEOF(SECURITY_ATTRIBUTES);
    /**
     * Create logic state events
     */
    svcstopended = CreateEventW(&sazero, TRUE, TRUE,  NULL);
    if (IS_INVALID_HANDLE(svcstopended))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", NULL);
    processended = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(processended))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", NULL);
    if (servicemode) {
        if (shutdownfile != NULL) {
            wchar_t *psn = xwcsconcat(SHUTDOWN_IPCNAME, serviceuuid);
            ssignalevent = CreateEventW(&sazero, TRUE, FALSE, psn);
            if (IS_INVALID_HANDLE(ssignalevent))
                return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", psn);
            xfree(psn);
        }
        if (haspipedlogs) {
            wchar_t *psn = xwcsconcat(DOROTATE_IPCNAME, serviceuuid);

            rsignalevent = CreateEventW(&sazero, FALSE, FALSE, psn);
            if (IS_INVALID_HANDLE(rsignalevent))
                return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", psn);
            xfree(psn);
        }
    }
    else {
        wchar_t *psn = xwcsconcat(SHUTDOWN_IPCNAME, serviceuuid);
        ssignalevent = OpenEventW(SYNCHRONIZE, FALSE, psn);
        if (IS_INVALID_HANDLE(ssignalevent))
            return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"OpenEvent", psn);
        xfree(psn);
    }
    if (haslogrotate) {
        logrotatesig = CreateEventW(&sazero, TRUE, FALSE, NULL);
        if (IS_INVALID_HANDLE(logrotatesig))
            return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", NULL);
    }
    monitorevent = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(monitorevent))
        return svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"CreateEvent", NULL);
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
            dbgprints(__FUNCTION__, "allocated new console");
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
        dbgprints(__FUNCTION__, "starting service");
        if (!StartServiceCtrlDispatcherW(se))
            rv = svcsyserror(__FUNCTION__, __LINE__, GetLastError(), L"StartServiceCtrlDispatcher", NULL);
    }
    else {
        dbgprints(__FUNCTION__, "starting shutdown");
        servicemain(0, NULL);
        rv = ssvcstatus.dwServiceSpecificExitCode;
    }
    dbgprints(__FUNCTION__, "done");
    return rv;
}
