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
#include "svcbatch.h"

static volatile LONG         monitorsig  = 0;
static volatile LONG         sstarted    = 0;
static SERVICE_STATUS_HANDLE hsvcstatus  = NULL;
static SERVICE_STATUS        ssvcstatus;
static CRITICAL_SECTION      servicelock;
static CRITICAL_SECTION      logfilelock;
static PROCESS_INFORMATION   cchild;
static SECURITY_ATTRIBUTES   sazero;
static HANDLE                cchildjob   = NULL;
static LONGLONG              rotateint   = ONE_DAY * 30;
static LARGE_INTEGER         rotatetmo   = {{ 0, 0} };
static LARGE_INTEGER         rotatesiz   = {{ 0, 0}};

static wchar_t  *comspec          = NULL;
static wchar_t **dupwenvp         = NULL;
static int       dupwenvc         = 0;
static wchar_t  *wenvblock        = NULL;
static int       hasctrlbreak     = 0;
static int       autorotate       = 0;

/**
 * Full path to the batch file
 */
static wchar_t  *svcbatchfile     = NULL;
static wchar_t  *batchdirname     = NULL;
static wchar_t  *svcbatchexe      = NULL;
static wchar_t  *servicebase      = NULL;
static wchar_t  *servicename      = NULL;
static wchar_t  *servicehome      = NULL;
static wchar_t  *serviceuuid      = NULL;

static wchar_t  *loglocation      = NULL;
static wchar_t  *logfilename      = NULL;
static HANDLE    logfhandle       = NULL;
static ULONGLONG logtickcount;

/**
 * Signaled by service stop thread
 * when done.
 */
static HANDLE    svcstopended     = NULL;
/**
 * Set when pipe and process threads
 * are done.
 */
static HANDLE    processended     = NULL;
/**
 * Set when SCM sends Custom signal
 */
static HANDLE    monitorevent     = NULL;
/**
 * Child process redirection pipes
 */
static HANDLE    stdoutputpipew   = NULL;
static HANDLE    stdoutputpiper   = NULL;
static HANDLE    stdinputpipewr   = NULL;
static HANDLE    stdinputpiperd   = NULL;

static wchar_t      zerostring[4] = { L'\0', L'\0', L'\0', L'\0' };
static const char   CRLF[4]       = { '\r', '\n', '\0', '\0' };

static const wchar_t *stdwinpaths = L";"    \
    L"%SystemRoot%\\System32;"              \
    L"%SystemRoot%;"                        \
    L"%SystemRoot%\\System32\\Wbem;"        \
    L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0";

/**
 * The following environment variables
 * are removed from the provided environment.
 */
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

/**
 * Safe malloc which calls _exit in case of ERROR_OUTOFMEMORY
 */
static void *xmalloc(size_t size)
{
    void *p = calloc(size, 1);
    if (p == NULL)
        _exit(ERROR_OUTOFMEMORY);

    return p;
}

static wchar_t *xwalloc(size_t size)
{
    wchar_t *p = (wchar_t *)calloc(size, sizeof(wchar_t));
    if (p == NULL)
        _exit(ERROR_OUTOFMEMORY);

    return p;
}

static wchar_t **waalloc(size_t size)
{
    return (wchar_t **)xmalloc((size + 1) * sizeof(wchar_t *));
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
    p = xwalloc(n + 2);
    wmemcpy(p, s, n);
    return p;
}

static wchar_t *xgetenv(const wchar_t *s)
{
    DWORD    n;
    wchar_t  e[2];
    wchar_t *d = NULL;

    if ((n = GetEnvironmentVariableW(s, e, 1)) != 0) {
        d = xwalloc(n + 1);
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

    cp = rv = xwalloc(l1 + l2 + 2);

    if(l1 > 0)
        wmemcpy(cp, s1, l1);
    cp += l1;
    if(l2 > 0)
        wmemcpy(cp, s2, l2);
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

    /* Pass one --- find length of required string */
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
    cp = rp = xwalloc(len + 2);
    if (sls[0] != 0) {
        wmemcpy(cp, p, sls[0]);
        cp += sls[0];
    }

    /* Pass two --- copy the argument strings into the result space */
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

/**
 * Check if str starts with src.
 * The check is case insensitive
 */
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

static void xreplacepathsep(wchar_t *p)
{
    while (*p != L'\0') {
        if (*p == L'/')
            *p = L'\\';
        p++;
    }
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

/**
 * Remove trailing backslash and path separator(s)
 */
static void rmtrailingps(wchar_t *s)
{
    int i;

    if (IS_EMPTY_WCS(s))
        return;
    for (i = 0; s[i] != L'\0'; i++) {
        if (s[i] == L'/')
            s[i] =  L'\\';
    }
    while (--i > 0) {
        if ((s[i] == L'\\') || (s[i] == L';'))
            s[i] = L'\0';
        else
            break;
    }
}

/**
 * Check if the path doesn't start
 * with \ or C:\
 */
static int isrelativepath(const wchar_t *path)
{
    if (path[0] < 128) {
        if (path[0] == L'\\')
            return 0;
        if ((isalpha(path[0]) != 0) && (path[1] == L':'))
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
/**
 * Use DebugView from SysInternal to see debug messages
 * since we don't have interactive console
 */
static void dbgprintf(const char *funcname, const char *format, ...)
{
    char    buf[MBUFSIZ];
    char   *bp;
    size_t  blen = MBUFSIZ - 2;
    int     n;
    va_list ap;

    memset(buf, 0, MBUFSIZ);
    n = _snprintf(buf, blen,
                  "[%.4lu] %-16s ",
                  GetCurrentThreadId(),
                  funcname);
    bp = buf + n;
    va_start(ap, format);
    _vsnprintf(bp, blen - n, format, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

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
        /**
         * Remove trailing spaces.
         */
        do {
            buf[len--] = L'\0';
        } while ((len > 0) && ((buf[len] == L'.') || (buf[len] < 33)));
        /**
         * Change embedded newline (\r\n\t) to space if present.
         */
        while (len-- > 0) {
            if (iswspace(buf[len]))
                buf[len] = L' ';
        }
    }
    else {
        _snwprintf(buf, bufsize, L"Unknown Win32 error code: %d", statcode);
        buf[bufsize - 1] = L'\0';
    }
}

static int setupeventlog(void)
{
    static int eset = 0;
    static const wchar_t emsg[] = L"%SystemRoot%\\System32\\netmsg.dll\0";

    DWORD c;
    HKEY  k;

    if (eset)
        return 1;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Services\\" \
                        L"EventLog\\Application\\" CPP_WIDEN(SVCBATCH_SVCNAME),
                        0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_READ | KEY_WRITE,
                        NULL, &k, &c) != ERROR_SUCCESS)
        return 0;

    if (c == REG_CREATED_NEW_KEY) {
        DWORD dw = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
                   EVENTLOG_INFORMATION_TYPE;
        RegSetValueExW(k, L"EventMessageFile", 0, REG_EXPAND_SZ,
                       (const BYTE *)emsg, DSIZEOF(emsg));
        RegSetValueExW(k, L"TypesSupported", 0, REG_DWORD,
                       (const BYTE *)&dw, 4);
    }
    RegCloseKey(k);
    eset = 1;
    return 1;
}

static DWORD svcsyserror(int line, DWORD ern, const wchar_t *err)
{
    wchar_t        buf[BBUFSIZ];
    wchar_t        erd[MBUFSIZ];
    HANDLE         es = NULL;
    const wchar_t *errarg[10];

    _snwprintf(buf, BBUFSIZ - 2, L"svcbatch.c(%d)", line);
    buf[BBUFSIZ - 1] = L'\0';
    errarg[0] = L"The " CPP_WIDEN(SVCBATCH_SVCNAME) L" named";
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

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "%S %S", errarg[0], errarg[1]);
#endif
    if (ern) {
        xwinapierror(erd, MBUFSIZ, ern);
        errarg[5] = L":";
        errarg[6] = erd;
        errarg[7] = L"\r\n";
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "%S %S : %S", buf, err, erd);
#endif
    }
    else {
        errarg[5] = L"\r\n";
        ern = ERROR_INVALID_PARAMETER;
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "%S %S", buf, err);
#endif
    }
    if (setupeventlog())
        es = RegisterEventSourceW(NULL, CPP_WIDEN(SVCBATCH_SVCNAME));
    if (IS_VALID_HANDLE(es)) {
        /**
         * Generic message: '%1 %2 %3 %4 %5 %6 %7 %8 %9'
         * The event code in netmsg.dll is 3299
         */
        ReportEventW(es, EVENTLOG_ERROR_TYPE,
                     0, 3299, NULL, 9, 0, errarg, NULL);
        DeregisterEventSource(es);
    }
    return ern;
}

static HANDLE xcreatethread(int detach,
                            DWORD (WINAPI *threadfn)(LPVOID),
                            LPVOID param)
{
    HANDLE h = CreateThread(NULL, 0, threadfn, param, 0, NULL);

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
    int       bsz;
    int       len;

    if ((bsz = xwcslen(str)) == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (wcschr(str, L'%') == 0)
        buf = xwcsdup(str);

    while (buf == NULL) {
        bsz = ALIGN_DEFAULT(bsz);
        buf = xwalloc(bsz);
        len = ExpandEnvironmentStringsW(str, buf, bsz - 1);
        if (len == 0) {
            xfree(buf);
            return NULL;
        }
        if (len >= bsz) {
            xfree(buf);
            buf = NULL;
            bsz = len + 1;
        }
    }
    return buf;
}

static wchar_t *getrealpathname(const wchar_t *path, int isdir)
{
    wchar_t    *es;
    wchar_t    *buf  = NULL;
    int         siz  = _MAX_FNAME;
    int         len  = 0;
    HANDLE      fh;
    DWORD       fa   = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

    if (IS_EMPTY_WCS(path))
        return NULL;
    if ((es = expandenvstrings(path)) == NULL)
        return NULL;
    rmtrailingps(es);
    fh = CreateFileW(es, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, fa, NULL);
    xfree(es);
    if (IS_INVALID_HANDLE(fh))
        return NULL;

    while (buf == NULL) {
        buf = xwalloc(siz);
        len = GetFinalPathNameByHandleW(fh, buf, siz - 1, VOLUME_NAME_DOS);
        if (len == 0) {
            CloseHandle(fh);
            xfree(buf);
            return NULL;
        }
        if (len >= siz) {
            xfree(buf);
            buf = NULL;
            siz = len + 1;
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
    wchar_t    *buf = NULL;
    DWORD       siz = BBUFSIZ;

    while (buf == NULL) {
        DWORD len;
        buf = xwalloc(siz);
        len = GetModuleFileNameW(NULL, buf, siz);

        if (len == 0) {
            xfree(buf);
            return 0;
        }
        else if (len == siz) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
                siz = siz * 2;
            else
                return 0;
            xfree(buf);
            buf = NULL;
        }
    }
    svcbatchexe = getrealpathname(buf, 0);
    xfree(buf);
    if (svcbatchexe != 0) {
        int i = xwcslen(svcbatchexe);
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

/**
 * Get the full path of the batch file
 * and get its directory
 *
 * Note that input parameter must have file extension.
 */
static int resolvebatchname(const wchar_t *batch)
{
    int i;

    svcbatchfile = getrealpathname(batch, 0);
    if (IS_EMPTY_WCS(svcbatchfile))
        return 0;

    i = xwcslen(svcbatchfile);
    while (--i > 0) {
        if (svcbatchfile[i] == L'\\') {
            svcbatchfile[i] = L'\0';
            batchdirname = xwcsdup(svcbatchfile);
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

    if (IS_VALID_HANDLE(ws)) {
        DWORD len;
        USEROBJECTFLAGS uf;
        wchar_t name[BBUFSIZ];

        if (GetUserObjectInformationW(ws, UOI_NAME, name,
                                      BBUFSIZ, &len)) {
            if (strstartswith(name, L"Service-"))
                rv = 1;

        }
        /**
         * Check if Window station has visible display surfaces
         * Interactive session are not supported.
         */
        if (GetUserObjectInformationW(ws, UOI_FLAGS, &uf,
                                      DSIZEOF(uf), &len)) {
            if (uf.dwFlags == WSF_VISIBLE)
                rv = 0;
        }
    }
    return rv;
}
#endif

/**
 * ServiceStatus support functions.
 */
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
    if (ssvcstatus.dwCurrentState == SERVICE_STOPPED)
        goto finished;
    ssvcstatus.dwControlsAccepted = 0;
    ssvcstatus.dwCheckPoint       = 0;
    ssvcstatus.dwWaitHint         = 0;

    if (status == SERVICE_RUNNING) {
        ssvcstatus.dwControlsAccepted =  SERVICE_ACCEPT_STOP |
                                         SERVICE_ACCEPT_SHUTDOWN;
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
    if (!SetServiceStatus(hsvcstatus, &ssvcstatus))
        svcsyserror(__LINE__, GetLastError(), L"SetServiceStatus");

finished:
    LeaveCriticalSection(&servicelock);
}

static PSECURITY_ATTRIBUTES getnullacl(void)
{
    static BYTE sb[BBUFSIZ];
    static SECURITY_ATTRIBUTES  sa;
    static PSECURITY_DESCRIPTOR sd = NULL;

    if (sd == NULL) {
        sd = (PSECURITY_DESCRIPTOR)sb;
        if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(sd, TRUE, (PACL)NULL, FALSE)) {
            sd = NULL;
            return NULL;
        }
    }
    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle       = TRUE;
    return &sa;
}

static DWORD createiopipes(void)
{
    LPSECURITY_ATTRIBUTES sa;
    DWORD  rc = 0;
    HANDLE sh = NULL;
    HANDLE cp = GetCurrentProcess();

    if ((sa = getnullacl()) == NULL)
        return svcsyserror(__LINE__, ERROR_ACCESS_DENIED, L"SetSecurityDescriptorDacl");
    /**
     * Create stdin pipe, with write side
     * of the pipe as non inheritable.
     */
    if (!CreatePipe(&stdinputpiperd, &sh, sa, 0))
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
    if (!DuplicateHandle(cp, sh, cp,
                         &stdinputpipewr, FALSE, 0,
                         DUPLICATE_SAME_ACCESS)) {
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle");
        goto finished;
    }
    SAFE_CLOSE_HANDLE(sh);

    /**
     * Create stdout/stderr pipe, with read side
     * of the pipe as non inheritable
     */
    if (!CreatePipe(&sh, &stdoutputpipew, sa, 0))
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
    if (!DuplicateHandle(cp, sh, cp,
                         &stdoutputpiper, FALSE, 0,
                         DUPLICATE_SAME_ACCESS))
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle");

finished:
    SAFE_CLOSE_HANDLE(sh);
    return rc;
}

static DWORD logappend(LPCVOID buf, DWORD len)
{
    DWORD w;
    LARGE_INTEGER ee = {{ 0, 0}};

    if (!SetFilePointerEx(logfhandle, ee, NULL, FILE_END))
        return GetLastError();
    if (!WriteFile(logfhandle, buf, len, &w, NULL))
        return GetLastError();
    else
        return 0;
}

/**
 * Set log file pointer to the EOF and write CRLF
 */
static void logfflush(void)
{
    FlushFileBuffers(logfhandle);
    logappend(CRLF, 2);
}

static void logwrline(const char *str)
{
    char    buf[BBUFSIZ];
    ULONGLONG ct;
    int     dd, hh, mm, ss, ms;
    DWORD   w;

    /**
     * Calculate time since the log
     * was opened and convert it to
     * DD:HH:MM:SS.mmm format
     *
     */
    ct = GetTickCount64() - logtickcount;
    ms = (int)((ct % MS_IN_SECOND));
    ss = (int)((ct / MS_IN_SECOND) % 60);
    mm = (int)((ct / MS_IN_MINUTE) % 60);
    hh = (int)((ct / MS_IN_HOUR)   % 24);
    dd = (int)((ct / MS_IN_DAY));

    sprintf(buf,
            "[%.2d:%.2d:%.2d:%.2d.%.3d] [%.4lu:%.4lu] ",
            dd, hh, mm, ss, ms,
            GetCurrentProcessId(),
            GetCurrentThreadId());
    WriteFile(logfhandle, buf, (DWORD)strlen(buf), &w, NULL);
    WriteFile(logfhandle, str, (DWORD)strlen(str), &w, NULL);

    WriteFile(logfhandle, CRLF, 2, &w, NULL);
}

static void logprintf(const char *format, ...)
{
    char    bp[MBUFSIZ];
    va_list ap;

    va_start(ap, format);
    _vsnprintf(bp, MBUFSIZ - 2, format, ap);
    va_end(ap);
    bp[MBUFSIZ - 1] = '\0';
    logwrline(bp);
}

static void logwrtime(const char *hdr)
{
    SYSTEMTIME tt;

    GetSystemTime(&tt);
    logprintf("%-16s : %.4d-%.2d-%.2d %.2d:%.2d:%.2d",
              hdr,
              tt.wYear, tt.wMonth, tt.wDay,
              tt.wHour, tt.wMinute, tt.wSecond);
}

/**
 * Write basic configuration when new logfile is created.
 */
static void logconfig(void)
{
    logprintf("Service name     : %S", servicename);
    logprintf("Service uuid     : %S", serviceuuid);
    logprintf("Batch file       : %S", svcbatchfile);
    logprintf("Base directory   : %S", servicebase);
    logprintf("Working directory: %S", servicehome);
    logprintf("Log directory    : %S", loglocation);
    logfflush();
}

/**
 *
 * Create service log file and rotate any previous
 * files in the Logs directory.
 */
static DWORD openlogfile(void)
{
    wchar_t  sfx[24];
    wchar_t *logpb = NULL;
    DWORD rc;
    WIN32_FILE_ATTRIBUTE_DATA ad;
    int i, m = 0;

    logtickcount = GetTickCount64();

    if (logfilename == NULL) {
        if (!CreateDirectoryW(loglocation, 0) &&
            (GetLastError() != ERROR_ALREADY_EXISTS))
            return GetLastError();
        if (isrelativepath(loglocation)) {
            wchar_t *n = loglocation;
            if ((loglocation = getrealpathname(n, 1)) == 0)
                return GetLastError();
            xfree(n);
        }
        logfilename = xwcsconcat(loglocation,
                                 L"\\" CPP_WIDEN(SVCBATCH_NAME) L".log");
    }
    memset(sfx, 0, 48);
    if (GetFileAttributesExW(logfilename, GetFileExInfoStandard, &ad)) {
        if (autorotate) {
            SYSTEMTIME st;

            FileTimeToSystemTime(&ad.ftCreationTime, &st);
            _snwprintf(sfx, 20, L".%.4d-%.2d%-.2d.%.2d%.2d%.2d",
                       st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond);
        }
        else {
            sfx[0] = L'.';
            sfx[1] = L'0';
        }
        logpb = xwcsconcat(logfilename, sfx);
        if (!MoveFileExW(logfilename, logpb, 0)) {
            rc = GetLastError();
            xfree(logpb);
            return svcsyserror(__LINE__, rc, L"MoveFileExW");
        }
    }

    if (ssvcstatus.dwCurrentState == SERVICE_START_PENDING)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    logfhandle = CreateFileW(logfilename,
                             GENERIC_WRITE,
                             FILE_SHARE_READ,
                            &sazero, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, NULL);

    if (IS_INVALID_HANDLE(logfhandle)) {
        goto failed;
    }
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
                    if (m > 0)
                        svcsyserror(__LINE__, rc, L"MoveFileExW already executed");
                    xfree(logpn);
                    xfree(lognn);
                    SetLastError(rc);
                    goto failed;
                }
                xfree(lognn);
                if (ssvcstatus.dwCurrentState == SERVICE_START_PENDING)
                    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
                m++;
            }
            xfree(logpn);
        }
    }
    xfree(logpb);

    logwrline(SVCBATCH_NAME " " SVCBATCH_VERSION_STR " " SVCBATCH_BUILD_STAMP);
    logwrtime("Log opened");
    return 0;

failed:
    rc = GetLastError();
    if (logpb != NULL) {
        MoveFileExW(logpb, logfilename, MOVEFILE_REPLACE_EXISTING);
        xfree(logpb);
    }
    return rc;
}

static DWORD rotatelogs(void)
{
    static int rotatecount = 1;
    DWORD rv = ERROR_FILE_NOT_FOUND;

    EnterCriticalSection(&logfilelock);
    if (IS_VALID_HANDLE(logfhandle)) {
        logfflush();
        logwrtime("Log rotated");
        FlushFileBuffers(logfhandle);
        SAFE_CLOSE_HANDLE(logfhandle);
        if ((rv = openlogfile()) == 0) {
            logprintf("Log generation   : %d", rotatecount++);
            logconfig();
        }
    }
    LeaveCriticalSection(&logfilelock);
    if (rv) {
        setsvcstatusexit(rv);
        svcsyserror(__LINE__, rv, L"rotatelogs");
    }
    return rv;
}

static void closelogfile(void)
{
    EnterCriticalSection(&logfilelock);
    if (IS_VALID_HANDLE(logfhandle)) {
        logfflush();
        logwrtime("Log closed");
        FlushFileBuffers(logfhandle);
        SAFE_CLOSE_HANDLE(logfhandle);
    }
    LeaveCriticalSection(&logfilelock);
}

static DWORD WINAPI stopthread(LPVOID unused)
{
    const char yn[2] = { 'Y', '\n'};
    DWORD wr, ws;
    BOOL  sc;
    int   i;

    if (InterlockedIncrement(&sstarted) > 1) {
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "already started");
#endif
        XENDTHREAD(0);
    }
    /**
     * Set stop event to non signaled.
     * This ensures that main thread will wait until we finish
     */
    ResetEvent(svcstopended);

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "started");
#endif
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

    EnterCriticalSection(&logfilelock);
    if (IS_VALID_HANDLE(logfhandle)) {
        logfflush();
        logwrline("Service STOP signaled\r\n");
    }
    LeaveCriticalSection(&logfilelock);

    /**
     * Calling SetConsoleCtrlHandler with the NULL and TRUE arguments
     * causes the calling process to ignore CTRL+C signals.
     * This attribute is inherited by child processes, but it can be
     * enabled or disabled by any process without affecting existing processes.
     */
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "raising CTRL_C_EVENT");
#endif
    sc = SetConsoleCtrlHandler(NULL, TRUE);
#if defined(_DBGVIEW)
    if (sc == FALSE)
        dbgprintf(__FUNCTION__, "SetConsoleCtrlHandler failed %d", GetLastError());
#endif
    GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    if (sc) {
        Sleep(SVCBATCH_PENDING_INIT);
        SetConsoleCtrlHandler(NULL, FALSE);
    }
    for (i = 0; i < 10; i++) {
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

        ws = WaitForSingleObject(processended, i == 0 ? SVCBATCH_PENDING_INIT : SVCBATCH_PENDING_WAIT);
        if (ws == WAIT_OBJECT_0) {
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "#%d processended", i);
#endif
            goto finished;
        }
        else if (ws == WAIT_TIMEOUT) {
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "#%d sending Y to child", i);
#endif
            /**
             * Write Y to stdin pipe in case cmd.exe waits for
             * user reply to "Terminate batch job (Y/N)?"
             */
            if (!WriteFile(stdinputpipewr, yn, 2, &wr, NULL)) {
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "#%d WriteFile Y failed %d", i, GetLastError());
#endif
                if (i > 8)
                    break;
            }
            FlushFileBuffers(stdinputpipewr);
        }
        else {
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "#%d WaitForSingleObject failed %d", i, GetLastError());
#endif
            break;
        }
    }
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "wait for processended signal");
#endif
    /**
     * Wait for main process to finish or times out.
     *
     * We are waiting at most for SVCBATCH_STOP_HINT
     * timeout and then kill all child processes.
     */
    ws = WaitForSingleObject(processended, SVCBATCH_STOP_HINT / 2);
    if (ws != WAIT_OBJECT_0) {
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "Child is still active (%d), terminating", ws);
#endif
        /**
         * WAIT_TIMEOUT means that child is
         * still running and we need to terminate
         * child tree by brute force
         */
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
        SAFE_CLOSE_HANDLE(cchild.hProcess);
        SAFE_CLOSE_HANDLE(cchildjob);
    }

finished:
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif

    SetEvent(svcstopended);
    XENDTHREAD(0);
}

static DWORD WINAPI iopipethread(LPVOID unused)
{
    DWORD  rc = 0;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "started");
#endif
    while (rc == 0) {
        BYTE  rb[HBUFSIZ];
        DWORD rd = 0;

        if (!ReadFile(stdoutputpiper, rb, HBUFSIZ, &rd, NULL) || (rd == 0)) {
            /**
             * Read from child failed.
             * ERROR_BROKEN_PIPE or ERROR_NO_DATA means that
             * child process closed its side of the pipe.
             */
            rc = GetLastError();
        }
        else {
            EnterCriticalSection(&logfilelock);
            if (IS_VALID_HANDLE(logfhandle))
                rc = logappend(rb, rd);
            else
                rc = ERROR_NO_MORE_FILES;
            LeaveCriticalSection(&logfilelock);
        }
    }
#if defined(_DBGVIEW)
    if (rc == ERROR_BROKEN_PIPE)
        dbgprintf(__FUNCTION__, "ERROR_BROKEN_PIPE");
    else if (rc == ERROR_NO_DATA)
        dbgprintf(__FUNCTION__, "ERROR_NO_DATA");
    else if (rc == ERROR_NO_MORE_FILES)
        dbgprintf(__FUNCTION__, "ERROR_NO_MORE_FILES, logfile closed");
    else
        dbgprintf(__FUNCTION__, "err=%d", rc);
    dbgprintf(__FUNCTION__, "done");
#endif

    XENDTHREAD(0);
}

static DWORD WINAPI monitorthread(LPVOID unused)
{

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "started");
#endif

    while (WaitForSingleObject(monitorevent, INFINITE) == WAIT_OBJECT_0) {
        LONG cc = InterlockedExchange(&monitorsig, 0);

        if (cc == 0) {
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "quit signaled");
#endif
            break;
        }
        else if (cc == SVCBATCH_CTRL_BREAK) {
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "break signaled");
#endif
            EnterCriticalSection(&logfilelock);
            if (IS_VALID_HANDLE(logfhandle)) {
                logfflush();
                logwrline("CTRL_BREAK_EVENT signaled\r\n");
            }
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
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "log rotation signaled");
#endif
            if (rotatelogs() != 0) {
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "log rotation failed");
#endif
                /**
                 * Create stop thread and exit.
                 */
                xcreatethread(1, &stopthread, NULL);
                break;
            }
        }
        ResetEvent(monitorevent);
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
    XENDTHREAD(0);
}

static DWORD WINAPI rotatethread(LPVOID unused)
{
    HANDLE wh[2];
    DWORD  wc, rc;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "started");
#endif

    wh[0] = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (IS_INVALID_HANDLE(wh[0])) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"CreateWaitableTimer");
        XENDTHREAD(0);
    }
    wh[1] = processended;

    if (!SetWaitableTimer(wh[0], &rotatetmo, 0, NULL, NULL, 0)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        svcsyserror(__LINE__, rc, L"SetWaitableTimer");
        goto finished;
    }
    do {
        wc = WaitForMultipleObjects(2, wh, FALSE, INFINITE);
        switch (wc) {
            case WAIT_TIMEOUT:
                if (rotatesiz.QuadPart) {
#if defined(_DBGVIEW)
                    dbgprintf(__FUNCTION__, "autorotate check");
#endif
                    EnterCriticalSection(&logfilelock);
                    if (IS_VALID_HANDLE(logfhandle)) {
                        LARGE_INTEGER fs;
                        if (GetFileSizeEx(logfhandle, &fs)) {
                            if (fs.QuadPart >= rotatesiz.QuadPart) {
                                LeaveCriticalSection(&logfilelock);
                                if (rotatelogs() != 0) {
#if defined(_DBGVIEW)
                                    dbgprintf(__FUNCTION__, "log rotation failed");
#endif
                                    xcreatethread(1, &stopthread, NULL);
                                    goto finished;
                                }
                                EnterCriticalSection(&logfilelock);
                            }
                        }
                        else {
                            rc = GetLastError();
                            LeaveCriticalSection(&logfilelock);
                            setsvcstatusexit(rc);
                            svcsyserror(__LINE__, rc, L"GetFileSizeEx");
#if defined(_DBGVIEW)
                            dbgprintf(__FUNCTION__, "get logfile size failed");
#endif
                            xcreatethread(1, &stopthread, NULL);
                            goto finished;

                        }
                    }
                    LeaveCriticalSection(&logfilelock);
                }
            break;
            case WAIT_OBJECT_0:
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "autorotate timeout");
#endif
                if (rotatelogs() != 0) {
    #if defined(_DBGVIEW)
                    dbgprintf(__FUNCTION__, "log rotation failed");
    #endif
                    goto finished;
                }
                rotatetmo.QuadPart += rotateint;
                if (!SetWaitableTimer(wh[0], &rotatetmo, 0, NULL, NULL, 0)) {
                    svcsyserror(__LINE__, GetLastError(), L"SetWaitableTimer");
                    xcreatethread(1, &stopthread, NULL);
                    goto finished;
                }
            break;
            case WAIT_OBJECT_1:
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "processended signaled");
#endif
            break;
            default:
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "wc=%d", wc);
#endif
            break;
        }
    } while ((wc == WAIT_OBJECT_0) || (wc == WAIT_TIMEOUT));

finished:
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
    CloseHandle(wh[0]);
    XENDTHREAD(0);
}

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    switch(ctrl) {
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            /**
             * Do we need that signal?
             */
            EnterCriticalSection(&logfilelock);
            if (IS_VALID_HANDLE(logfhandle)) {
                logfflush();
                logwrline("CTRL_SHUTDOWN_EVENT signaled");
            }
            LeaveCriticalSection(&logfilelock);
        break;
        case CTRL_C_EVENT:
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "CTRL_C_EVENT signaled");
#endif
        break;
        case CTRL_BREAK_EVENT:
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "CTRL_BREAK_EVENT signaled");
#endif
        break;
        case CTRL_LOGOFF_EVENT:
        break;
        default:
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "unknown ctrl %d", ctrl);
#endif
            return FALSE;
        break;
    }
    return TRUE;
}

static DWORD WINAPI servicehandler(DWORD ctrl, DWORD _xe, LPVOID _xd, LPVOID _xc)
{
    switch(ctrl) {
        case SERVICE_CONTROL_SHUTDOWN:
#if defined(SERVICE_CONTROL_PRESHUTDOWN)
        case SERVICE_CONTROL_PRESHUTDOWN:
#endif
            EnterCriticalSection(&logfilelock);
            if (IS_VALID_HANDLE(logfhandle)) {
                logfflush();
                logprintf("Service SHUTDOWN (0x%08x) signaled", ctrl);
            }
            LeaveCriticalSection(&logfilelock);
        case SERVICE_CONTROL_STOP:
            reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
            xcreatethread(1, &stopthread, NULL);
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
        case SERVICE_CONTROL_INTERROGATE:
        break;
        default:
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "unknown ctrl %d", ctrl);
#endif
            return ERROR_CALL_NOT_IMPLEMENTED;
        break;
    }
    return 0;
}

static DWORD WINAPI workerthread(LPVOID unused)
{
    STARTUPINFOW si;
    wchar_t *cmdline;
    wchar_t *arg0;
    wchar_t *arg1;
    HANDLE   wh[2] = {0, 0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "started");
#endif
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    /**
     * Create a command line
     */
    if (wcshavespace(comspec))
        arg0 = xwcsvarcat(L"\"", comspec, L"\"", 0);
    else
        arg0 = comspec;
    if (wcshavespace(svcbatchfile))
        arg1 = xwcsvarcat(L"\"", svcbatchfile, L"\"", 0);
    else
        arg1 = svcbatchfile;
    cmdline = xwcsvarcat(arg0, L" /D /C \"", arg1, L"\"", 0);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "program %S", comspec);
    dbgprintf(__FUNCTION__, "cmdline %S", cmdline);
#endif
    if (createiopipes() != 0)
        goto finished;
    memset(&job, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    job.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(cchildjob,
                                 JobObjectExtendedLimitInformation,
                                &job,
                                 sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))) {
        svcsyserror(__LINE__, GetLastError(), L"SetInformationJobObject");
        goto finished;
    }

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    memset(&si, 0, sizeof(STARTUPINFOW));
    si.cb         = DSIZEOF(STARTUPINFOW);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdinputpiperd;
    si.hStdOutput = stdoutputpipew;
    si.hStdError  = stdoutputpipew;

    if (!CreateProcessW(comspec, cmdline, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        wenvblock,
                        servicehome,
                       &si, &cchild)) {
        /**
         * CreateProcess failed ... nothing we can do.
         */
        setsvcstatusexit(GetLastError());
        svcsyserror(__LINE__, GetLastError(), L"CreateProcess");
        goto finished;
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "child id %d", cchild.dwProcessId);
#endif
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(stdoutputpipew);
    SAFE_CLOSE_HANDLE(stdinputpiperd);
    if (!AssignProcessToJobObject(cchildjob, cchild.hProcess)) {
        svcsyserror(__LINE__, GetLastError(), L"AssignProcessToJobObject");
        TerminateProcess(cchild.hProcess, ERROR_ACCESS_DENIED);
        CloseHandle(cchild.hThread);
        setsvcstatusexit(ERROR_ACCESS_DENIED);
        goto finished;
    }
    wh[0] = cchild.hProcess;
    wh[1] = xcreatethread(0, &iopipethread, NULL);
    if (IS_INVALID_HANDLE(wh[1])) {
        svcsyserror(__LINE__, ERROR_TOO_MANY_TCBS, L"iopipethread");
        TerminateProcess(cchild.hProcess, ERROR_OUTOFMEMORY);
        CloseHandle(cchild.hThread);
        setsvcstatusexit(ERROR_TOO_MANY_TCBS);
        goto finished;
    }

    ResumeThread(cchild.hThread);
    CloseHandle(cchild.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "service running");
#endif
    xcreatethread(1, &rotatethread, NULL);
    WaitForMultipleObjects(2, wh, TRUE, INFINITE);
    CloseHandle(wh[1]);

finished:
    SetEvent(processended);
#if defined(_DBGVIEW)
    if (ssvcstatus.dwServiceSpecificExitCode)
        dbgprintf(__FUNCTION__, "ServiceSpecificExitCode=%d",
                  ssvcstatus.dwServiceSpecificExitCode);
#endif
    InterlockedExchange(&monitorsig, 0);
    SetEvent(monitorevent);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif

    XENDTHREAD(0);
}

/**
 * Main Service function
 * This thread is created by SCM. SCM should provide
 * the service name as first argument.
 */
static void WINAPI servicemain(DWORD argc, wchar_t **argv)
{
    int          i;
    DWORD        rv    = 0;
    int          eblen = 0;
    wchar_t     *ep;
    HANDLE       wh[2] = { NULL, NULL};

    ssvcstatus.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    ssvcstatus.dwCurrentState = SERVICE_START_PENDING;

    if (argc == 0) {
        svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, L"Missing servicename");
        exit(ERROR_INVALID_PARAMETER);
        return;
    }

    servicename = xwcsdup(argv[0]);
    hsvcstatus  = RegisterServiceCtrlHandlerExW(servicename, servicehandler, NULL);
    if (IS_INVALID_HANDLE(hsvcstatus)) {
        svcsyserror(__LINE__, GetLastError(), L"RegisterServiceCtrlHandlerEx");
        exit(ERROR_INVALID_HANDLE);
        return;
    }
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "started %S", servicename);
#endif

    if ((rv = openlogfile()) != 0) {
        svcsyserror(__LINE__, rv, L"OpenLogfile");
        reportsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    logconfig();
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
    wenvblock = xwalloc(eblen + 2);
    for (i = 0, ep = wenvblock; i < dupwenvc; i++) {
        int nn = xwcslen(dupwenvp[i]);
        wmemcpy(ep, dupwenvp[i], nn);
        ep += nn + 1;
    }

    wh[0] = xcreatethread(0, &monitorthread, NULL);
    if (IS_INVALID_HANDLE(wh[0])) {
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"monitorthread");
        goto finished;
    }
    wh[1] = xcreatethread(0, &workerthread,  NULL);
    if (IS_INVALID_HANDLE(wh[1])) {
        InterlockedExchange(&monitorsig, 0);
        SignalObjectAndWait(monitorevent, wh[0], INFINITE, FALSE);
        CloseHandle(wh[0]);
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"workerthread");
        goto finished;
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "running");
#endif
    /**
     * This is main wait loop that waits
     * for worker and monitor threads to finish.
     */
    WaitForMultipleObjects(2, wh, TRUE, INFINITE);
    CloseHandle(wh[0]);
    CloseHandle(wh[1]);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "wait for stop thread to finish");
#endif
    /**
     * Wait for stop thread to finish if started
     */
    WaitForSingleObject(svcstopended, SVCBATCH_STOP_WAIT);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "svcstopended");
#endif

finished:

    SAFE_CLOSE_HANDLE(stdoutputpipew);
    SAFE_CLOSE_HANDLE(stdoutputpiper);
    SAFE_CLOSE_HANDLE(stdinputpipewr);
    SAFE_CLOSE_HANDLE(stdinputpiperd);
    SAFE_CLOSE_HANDLE(cchild.hProcess);
    SAFE_CLOSE_HANDLE(cchildjob);

    closelogfile();
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
    reportsvcstatus(SERVICE_STOPPED, rv);
}

static int resolverotate(wchar_t *rp)
{
    SYSTEMTIME st;
    FILETIME   ft;

    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);

    rotatetmo.HighPart  = ft.dwHighDateTime;
    rotatetmo.LowPart   = ft.dwLowDateTime;
    rotatetmo.QuadPart += rotateint;

    if (IS_EMPTY_WCS(rp)) {
        if (autorotate)
            return __LINE__;
        else
            return 0;
    }
    if (*rp == L'@') {
        int      hh, mm, ss;
        wchar_t *p;
        ULARGE_INTEGER ui;
        SYSTEMTIME     ct;

        rotateint = ONE_DAY;
        if ((p = wcschr(rp + 1, L':')) == NULL)
            return __LINE__;
        *(p++) = L'\0';
        hh = _wtoi(rp);
        rp = p;
        if ((p = wcschr(rp, L':')) == NULL)
            return __LINE__;
        *(p++) = L'\0';
        mm = _wtoi(rp);
        rp = p;
        if ((p = wcschr(rp, L'|')) != NULL)
            *(p++) = L'\0';
        ss = _wtoi(rp);
        rp = p;
        if ((hh > 23) || (mm > 59) || (ss > 59))
            return __LINE__;
        /**
         * Add one day
         * TODO: Create some macros or __inline
         *       for converting FILETIME to LARGE_INTEGER
         *       and vice versa.
         */
        ui.HighPart  = ft.dwHighDateTime;
        ui.LowPart   = ft.dwLowDateTime;
        ui.QuadPart += rotateint;
        ft.dwHighDateTime = ui.HighPart;
        ft.dwLowDateTime  = ui.LowPart;
        FileTimeToSystemTime(&ft, &ct);
        ct.wHour   = hh;
        ct.wMinute = mm;
        ct.wSecond = ss;
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "rotate @%.2d:%.2d:%.2d", hh, mm, ss);
#endif
        SystemTimeToFileTime(&ct, &ft);
        rotatetmo.HighPart  = ft.dwHighDateTime;
        rotatetmo.LowPart   = ft.dwLowDateTime;
    }
    if (rp != NULL) {
        LONGLONG siz;
        LONGLONG mux = CPP_INT64_C(1);
        wchar_t *ep;

#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "rotatesize: %S", rp);
#endif
        siz = _wcstoui64(rp, &ep, 10);
        if (siz == 0)
            return __LINE__;
        if (ep) {
            switch (*ep) {
                case L'K':
                    mux = KILOBYTES(1);
                break;
                case L'M':
                    mux = KILOBYTES(1024);
                break;
                case L'G':
                    mux = KILOBYTES(1024 * 1024);
                break;
                default:
                    return __LINE__;
                break;
            }
        }
        rotatesiz.QuadPart = siz * mux;
        if (rotatesiz.QuadPart < SVCBATCH_MIN_LOGSIZE) {
            /**
             * Ensure rotate size is at least SVCBATCH_MIN_LOGSIZE
             */
            return __LINE__;
        }
    }
    return 0;
}
/**
 * Needed to release conhost.exe
 */
static void __cdecl cconsolecleanup(void)
{
    SetConsoleCtrlHandler(consolehandler, FALSE);
    FreeConsole();
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
}

/**
 * Cleanup created resources ...
 */
static void __cdecl objectscleanup(void)
{
    SAFE_CLOSE_HANDLE(processended);
    SAFE_CLOSE_HANDLE(svcstopended);
    SAFE_CLOSE_HANDLE(monitorevent);
    SAFE_CLOSE_HANDLE(cchildjob);

    DeleteCriticalSection(&logfilelock);
    DeleteCriticalSection(&servicelock);

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
}

/**
 * SvcBatch main program entry
 */
int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
{
    int         i;
    wchar_t    *opath;
    wchar_t    *cpath;
    wchar_t    *bname = NULL;
    wchar_t    *autorotatep = NULL;
    int         envc  = 0;
    int         cleanpath  = 0;
    int         usesafeenv = 0;
    int         hasopts    = 1;
    HANDLE      hstdin;
    SERVICE_TABLE_ENTRYW se[2];

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
                rmtrailingps(loglocation);
                continue;
            }
            if (autorotatep == zerostring) {
                autorotatep = xwcsdup(p);
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
                switch (p[1]) {
                    case L'b':
                    case L'B':
                        hasctrlbreak = 1;
                    break;
                    case L'c':
                    case L'C':
                        cleanpath    = 1;
                    break;
                    case L'o':
                    case L'O':
                        loglocation  = zerostring;
                    break;
                    case L'r':
                    case L'R':
                        autorotate   = 1;
                        autorotatep  = zerostring;
                    break;
                    case L's':
                    case L'S':
                        usesafeenv   = 1;
                    break;
                    case L'w':
                    case L'W':
                        servicehome  = zerostring;
                    break;
                    default:
                        return svcsyserror(__LINE__, 0, L"Unknown command line option");
                    break;
                }
                continue;
            }
        }
        if (bname == NULL) {
            /**
             * First argument after options is batch file
             * that we are going to execute.
             */
            if ((bname = expandenvstrings(p)) == NULL)
                return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, p);
            xreplacepathsep(bname);
        }
        else {
            /**
             * We have extra parameters after batch file.
             * This is user install error.
             */
            return svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, p);
        }
    }
#if defined(_DBGVIEW)
    OutputDebugStringA("Lets go");
#endif
    /**
     * Check if we are running as service
     */
#if defined(_CHECK_IF_SERVICE)
    if (runningasservice() == 0) {
        fputs("\n" SVCBATCH_NAME " " SVCBATCH_VERSION_STR, stderr);
        fputs(" "  SVCBATCH_BUILD_STAMP, stderr);
        fputs("\n" SVCBATCH_COPYRIGHT "\n\n", stderr);
        fputs("This program can only run as Windows Service\n", stderr);
        return 1;
    }
#endif
    if (bname == NULL)
        return svcsyserror(__LINE__, 0, L"Missing batch file");

    if (resolvesvcbatchexe() == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, wargv[0]);

    if ((opath = xgetenv(L"PATH")) == NULL)
        return svcsyserror(__LINE__, ERROR_ENVVAR_NOT_FOUND, L"PATH");

    if ((serviceuuid = xuuidstring()) == NULL)
        return svcsyserror(__LINE__, GetLastError(), L"CryptGenRandom");

    /**
     * Get full path to cmd.exe
     */
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
    if (servicehome == NULL) {
        /**
         * Use batch file directory as new cwd
         */
        servicehome = batchdirname;
        if (!SetCurrentDirectoryW(servicehome))
            return svcsyserror(__LINE__, GetLastError(), servicehome);
    }
    if (loglocation == NULL)
        loglocation = xwcsdup(SVCBATCH_LOG_BASE);
    if (autorotate) {
        int rv = resolverotate(autorotatep);
        if (rv)
            return svcsyserror(rv, ERROR_INVALID_PARAMETER, autorotatep);
    }
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
                    /**
                     * Found safe environment variable
                     */
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
                    /**
                     * Skip private environment variable
                     */
                    p = NULL;
                    break;
                }
                e++;
            }
        }
        if (p != NULL)
            dupwenvp[dupwenvc++] = xwcsdup(p);
    }

    if (cleanpath) {
        wchar_t *cp = xwcsvarcat(servicebase, L";",
                                 servicehome,
                                 stdwinpaths, NULL);
        xfree(opath);

        if ((opath = expandenvstrings(cp)) == NULL)
            return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, cp);
        xfree(cp);
    }
    rmtrailingps(opath);
    dupwenvp[dupwenvc++] = xwcsconcat(L"PATH=", opath);
    xfree(opath);

    memset(&cchild,     0, sizeof(PROCESS_INFORMATION));
    memset(&ssvcstatus, 0, sizeof(SERVICE_STATUS));
    memset(&sazero,     0, sizeof(SECURITY_ATTRIBUTES));
    sazero.nLength = DSIZEOF(SECURITY_ATTRIBUTES);
    /**
     * Create logic state events
     */
    svcstopended = CreateEventW(&sazero, TRUE, TRUE,  NULL);
    processended = CreateEventW(&sazero, TRUE, FALSE, NULL);
    monitorevent = CreateEventW(&sazero, TRUE, FALSE, NULL);
    if (IS_INVALID_HANDLE(processended) ||
        IS_INVALID_HANDLE(svcstopended) ||
        IS_INVALID_HANDLE(monitorevent))
        return svcsyserror(__LINE__, ERROR_OUTOFMEMORY, L"CreateEvent");
    cchildjob = CreateJobObjectW(&sazero, NULL);
    if (IS_INVALID_HANDLE(cchildjob))
        return svcsyserror(__LINE__, GetLastError(), L"CreateJobObject");

    InitializeCriticalSection(&servicelock);
    InitializeCriticalSection(&logfilelock);
    atexit(objectscleanup);

    hstdin = GetStdHandle(STD_INPUT_HANDLE);
    if (IS_VALID_HANDLE(hstdin))
        return svcsyserror(__LINE__, 0, L"Console already exists");
    if (AllocConsole()) {
        /**
         * AllocConsole should create new set of
         * standard i/o handles
         */
        atexit(cconsolecleanup);
        hstdin = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (IS_INVALID_HANDLE(hstdin))
        return svcsyserror(__LINE__, GetLastError(), L"GetStdHandle");

    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = NULL;
    se[1].lpServiceProc = NULL;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "start service");
#endif
    if (!StartServiceCtrlDispatcherW(se))
        return svcsyserror(__LINE__, GetLastError(), L"StartServiceCtrlDispatcher");
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
    return 0;
}
