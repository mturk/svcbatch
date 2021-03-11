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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <process.h>
#include "svcbatch.h"

static volatile LONG         monitorsig  = 0;
static SERVICE_STATUS_HANDLE hsvcstatus  = 0;
static SERVICE_STATUS        ssvcstatus;
static CRITICAL_SECTION      servicelock;
static CRITICAL_SECTION      logfilelock;
static PROCESS_INFORMATION   cchild;
static SECURITY_ATTRIBUTES   sazero;

static wchar_t  *comspec          = 0;
static wchar_t **dupwenvp         = 0;
static int       dupwenvc         = 0;
static wchar_t  *wenvblock        = 0;
static int       hasctrlbreak     = 0;

/**
 * Full path to the batch file
 */
static wchar_t  *svcbatchfile     = 0;
static wchar_t  *batchdirname     = 0;
static wchar_t  *svcbatchexe      = 0;
static wchar_t  *servicebase      = 0;
static wchar_t  *servicename      = 0;
static wchar_t  *servicehome      = 0;
static wchar_t  *serviceuuid      = 0;

static wchar_t  *loglocation      = 0;
static wchar_t  *logfilename      = 0;
static HANDLE    logfhandle       = 0;
static ULONGLONG logtickcount;

/**
 * Signaled by service stop thread
 * when done.
 */
static HANDLE    svcstopended     = 0;
/**
 * Set when pipe and process threads
 * are done.
 */
static HANDLE    processended     = 0;
/**
 * Set when SCM sends Custom signal
 */
static HANDLE    monitorevent     = 0;
/**
 * Child process redirection pipes
 */
static HANDLE    stdoutputpipew   = 0;
static HANDLE    stdoutputpiper   = 0;
static HANDLE    stdinputpipewr   = 0;
static HANDLE    stdinputpiperd   = 0;

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
    0
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
    0
};

/**
 * Safe malloc which calls _exit in case of ERROR_OUTOFMEMORY
 */
static void *xmalloc(size_t size)
{
    void *p = calloc(size, 1);
    if (p == 0)
        _exit(ERROR_OUTOFMEMORY);

    return p;
}

static wchar_t *xwalloc(size_t size)
{
    wchar_t *p = (wchar_t *)calloc(size, sizeof(wchar_t));
    if (p == 0)
        _exit(ERROR_OUTOFMEMORY);

    return p;
}

static wchar_t **waalloc(size_t size)
{
    return (wchar_t **)xmalloc((size + 1) * sizeof(wchar_t *));
}

static void xfree(void *m)
{
    if (m != 0)
        free(m);
}

static wchar_t *xwcsdup(const wchar_t *s)
{
    wchar_t *p;
    size_t   n;

    if (IS_EMPTY_WCS(s))
        return 0;
    n = wcslen(s);
    p = xwalloc(n + 2);
    wmemcpy(p, s, n);
    return p;
}

static wchar_t *xgetenv(const wchar_t *s)
{
    DWORD    n;
    wchar_t  e[2];
    wchar_t *d = 0;

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
        return 0;

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
    while ((ap = va_arg(vap, const wchar_t *)) != 0) {
        sls[cnt] = xwcslen(ap);
        len += sls[cnt];
        if (cnt++ > 30)
            break;
    }
    va_end(vap);
    len += sls[0];

    if (len == 0)
        return 0;
    cp = rp = xwalloc(len + 2);
    if (sls[0] != 0) {
        wmemcpy(cp, p, sls[0]);
        cp += sls[0];
    }

    /* Pass two --- copy the argument strings into the result space */
    cnt = 1;

    va_start(vap, p);
    while ((ap = va_arg(vap, const wchar_t *)) != 0) {
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
 * with \\ (something like \\?\) or C:\
 */
static int isrelativepath(const wchar_t *path)
{
    if (path[0] < 128) {
        if ((path[0] == L'\\') && (path[1] == L'\\'))
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

    if (CryptAcquireContext(&h, 0, 0, PROV_RSA_FULL,
                            CRYPT_VERIFYCONTEXT | CRYPT_SILENT) == 0)
        return 0;
    if (CryptGenRandom(h, 16, d) == 0)
        return 0;
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
                         0,
                         statcode,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         buf,
                         bufsize,
                         0);
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
                        0, 0, 0,
                        KEY_QUERY_VALUE | KEY_READ | KEY_WRITE,
                        0, &k, &c) != ERROR_SUCCESS)
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
    wchar_t        buf[SBUFSIZ];
    wchar_t        erd[SBUFSIZ];
    HANDLE         es = 0;
    const wchar_t *errarg[9];

    memset(buf, 0, SBUFSIZ * sizeof(wchar_t));
    _snwprintf(buf, SBUFSIZ - 2, L"svcbatch.c(%d) %s", line, err);

    errarg[0] = L"The " CPP_WIDEN(SVCBATCH_SVCNAME) L" named";
    if (IS_EMPTY_WCS(servicename))
        errarg[1] = L"(undefined)";
    else
        errarg[1] = servicename;
    errarg[2] = L"reported the following error:\r\n>>>";
    errarg[3] = buf;
    errarg[4] = 0;
    errarg[5] = 0;
    errarg[6] = 0;
    errarg[7] = 0;
    errarg[8] = 0;

    if (ern) {
        xwinapierror(erd, SBUFSIZ, ern);
        errarg[4] = L": ";
        errarg[5] = erd;
    }
    else {
        ern = ERROR_INVALID_PARAMETER;
    }
    if (setupeventlog())
        es = RegisterEventSourceW(0, CPP_WIDEN(SVCBATCH_SVCNAME));
    if (IS_VALID_HANDLE(es)) {
        /**
         * Generic message: '%1 %2 %3 %4 %5 %6 %7 %8 %9'
         * The event code in netmsg.dll is 3299
         */
        ReportEventW(es, EVENTLOG_ERROR_TYPE,
                     0, 3299, 0, 9, 0, errarg, 0);
        DeregisterEventSource(es);
    }
    return ern;
}

static DWORD killprocesstree(DWORD pid, UINT err)
{
    DWORD  r = 0;
    DWORD  c = 0;
    DWORD  a[SBUFSIZ];
    DWORD  i, x;
    HANDLE h;
    HANDLE p;
    PROCESSENTRY32W e;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "%d", pid);
#endif
    h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (IS_INVALID_HANDLE(h))
        return GetLastError();

    e.dwSize = DSIZEOF(PROCESSENTRY32W);
    if (Process32FirstW(h, &e) == 0) {
        r = GetLastError();
        CloseHandle(h);
        return r == ERROR_NO_MORE_FILES ? 0 : r;
    }
    do {
        if (e.th32ParentProcessID == pid) {
            if (c == SBUFSIZ) {
                /**
                 * Process has more then 1K child processes !?
                 */
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "%d overflow", pid);
#endif
                break;
            }
            p = OpenProcess(PROCESS_QUERY_INFORMATION, 0, e.th32ProcessID);
            if (IS_VALID_HANDLE(p)) {
                if (GetExitCodeProcess(p, &x))
                    a[c++] = e.th32ProcessID;
                CloseHandle(p);
            }
        }

    } while (Process32NextW(h, &e));
    CloseHandle(h);

#if defined(_DBGTRACE)
    dbgprintf(__FUNCTION__, "%d has %d subtrees", pid, c);
#endif
    for (i = 0; i < c; i++) {
        /**
         * Terminate each child and its children
         */
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "%d killing subtree: %d", pid, a[i]);
#endif
        killprocesstree(a[i], err);
    }
    if (c == SBUFSIZ) {
        /**
         * Do recursive call on overflow
         */
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "%d killing recursive ...", pid);
#endif
        killprocesstree(pid, err);
    }
    if (pid == cchild.dwProcessId) {
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "%d is child pid", pid);
#endif
        return 0;
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "%d terminating", pid);
#endif
    p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, 0, pid);
    if (IS_INVALID_HANDLE(p)) {
        r = GetLastError();
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "%d open failed: %d", pid, r);
#endif
        return r;
    }
    if (GetExitCodeProcess(p, &x)) {
        if (x == STILL_ACTIVE) {
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "%d STILL_ACTIVE", pid);
#endif
            if (TerminateProcess(p, err) == 0)
                r = GetLastError();
#if defined(_DBGVIEW)
            if (r)
                dbgprintf(__FUNCTION__, "%d terminate failed: %d", pid, r);
#endif
        }
#if defined(_DBGVIEW)
        else {
            dbgprintf(__FUNCTION__, "%d EXIT_CODE %d ", pid, x);
        }
#endif
    }
    else {
        r = GetLastError();
    }
    CloseHandle(p);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "%d done %d", pid, r);
#endif
    return r;
}

static DWORD killprocessmain(UINT err)
{
    DWORD x = 0;
    DWORD r = 0;

#if defined(_DBGTRACE)
    dbgprintf(__FUNCTION__, "%d", cchild.dwProcessId);
#endif
    if (IS_INVALID_HANDLE(cchild.hProcess)) {
#if defined(_DBGTRACE)
        dbgprintf(__FUNCTION__, "%d INVALID_HANDLE", cchild.dwProcessId);
#endif
        return 0;
    }
    if (GetExitCodeProcess(cchild.hProcess, &x)) {
        if (x == STILL_ACTIVE) {
#if defined(_DBGTRACE)
            dbgprintf(__FUNCTION__, "%d STILL_ACTIVE", cchild.dwProcessId);
#endif
            if (TerminateProcess(cchild.hProcess, err) == 0)
                r = GetLastError();
        }
    }
    else {
        r = GetLastError();
    }
#if defined(_DBGTRACE)
    dbgprintf(__FUNCTION__, "%d done %d ", cchild.dwProcessId, r);
#endif

    return r;
}

static HANDLE xcreatethread(int detach,
                            DWORD (WINAPI *threadfn)(LPVOID),
                            LPVOID param)
{
    HANDLE h = CreateThread(0, 0, threadfn, param, 0, 0);

    if (IS_INVALID_HANDLE(h))
        return INVALID_HANDLE_VALUE;
    if (detach) {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    return h;
}

static wchar_t *expandenvstrings(const wchar_t *str)
{
    wchar_t  *buf = 0;
    int       bsz;
    int       len;

    if ((bsz = xwcslen(str)) == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (wcschr(str, L'%') == 0)
        buf = xwcsdup(str);

    while (buf == 0) {
        bsz = ALIGN_DEFAULT(bsz);
        buf = xwalloc(bsz);
        len = ExpandEnvironmentStringsW(str, buf, bsz - 1);
        if (len == 0) {
            xfree(buf);
            return 0;
        }
        if (len >= bsz) {
            xfree(buf);
            buf = 0;
            bsz = len + 1;
        }
    }
    return buf;
}

static wchar_t *getrealpathname(const wchar_t *path, int isdir)
{
    wchar_t    *es;
    wchar_t    *buf  = 0;
    int         siz  = _MAX_FNAME;
    int         len  = 0;
    HANDLE      fh;
    DWORD       fa   = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

    if (IS_EMPTY_WCS(path))
        return 0;
    if ((es = expandenvstrings(path)) == 0)
        return 0;
    rmtrailingps(es);
    fh = CreateFileW(es, GENERIC_READ, FILE_SHARE_READ, 0,
                     OPEN_EXISTING, fa, 0);
    xfree(es);
    if (IS_INVALID_HANDLE(fh))
        return 0;

    while (buf == 0) {
        buf = xwalloc(siz);
        len = GetFinalPathNameByHandleW(fh, buf, siz - 1, VOLUME_NAME_DOS);
        if (len == 0) {
            CloseHandle(fh);
            xfree(buf);
            return 0;
        }
        if (len >= siz) {
            xfree(buf);
            buf = 0;
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
    wchar_t    *buf = 0;
    DWORD       siz = BBUFSIZ;

    while (buf == 0) {
        DWORD len;
        buf = xwalloc(siz);
        len = GetModuleFileNameW(0, buf, siz);

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
            buf = 0;
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
    if (SetServiceStatus(hsvcstatus, &ssvcstatus) == 0)
        svcsyserror(__LINE__, GetLastError(), L"SetServiceStatus");

finished:
    LeaveCriticalSection(&servicelock);
}

static PSECURITY_ATTRIBUTES getnullacl(void)
{
    static BYTE sb[BBUFSIZ];
    static SECURITY_ATTRIBUTES  sa;
    static PSECURITY_DESCRIPTOR sd = 0;

    if (sd == 0) {
        sd = (PSECURITY_DESCRIPTOR)sb;
        if ((InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) == 0) ||
            (SetSecurityDescriptorDacl(sd, 1, (PACL)0, 0) == 0)) {
            sd = 0;
            return 0;
        }
    }
    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle       = 1;
    return &sa;
}

static DWORD createiopipes(void)
{
    LPSECURITY_ATTRIBUTES sa;
    DWORD  rc = 0;
    HANDLE sh = 0;
    HANDLE cp = GetCurrentProcess();

    if ((sa = getnullacl()) == 0)
        return svcsyserror(__LINE__, ERROR_ACCESS_DENIED, L"SetSecurityDescriptorDacl");
    /**
     * Create stdin pipe, with write side
     * of the pipe as non inheritable.
     */
    if (CreatePipe(&stdinputpiperd, &sh, sa, 0) == 0)
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
    if (DuplicateHandle(cp, sh, cp,
                        &stdinputpipewr, 0, 0,
                        DUPLICATE_SAME_ACCESS) == 0) {
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle");
        goto finished;
    }
    SAFE_CLOSE_HANDLE(sh);

    /**
     * Create stdout/stderr pipe, with read side
     * of the pipe as non inheritable
     */
    if (CreatePipe(&sh, &stdoutputpipew, sa, 0) == 0)
        return svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
    if (DuplicateHandle(cp, sh, cp,
                        &stdoutputpiper, 0, 0,
                        DUPLICATE_SAME_ACCESS) == 0)
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle");

finished:
    SAFE_CLOSE_HANDLE(sh);
    return rc;
}

static DWORD logappend(LPCVOID buf, DWORD len)
{
    DWORD w;
    LARGE_INTEGER ee = {{ 0, 0}};

    if (IS_INVALID_HANDLE(logfhandle))
        return ERROR_FILE_NOT_FOUND;
    if (SetFilePointerEx(logfhandle, ee, 0, FILE_END) == 0)
        return GetLastError();
    if (WriteFile(logfhandle, buf, len, &w, 0) == 0)
        return GetLastError();
    else
        return 0;
}

/**
 * Set log file pointer to the EOF and write CRLF
 */
static void logfflush(void)
{
    if (IS_INVALID_HANDLE(logfhandle))
        return;
    FlushFileBuffers(logfhandle);
    logappend(CRLF, 2);
}

static void logwrline(const char *str)
{
    char    buf[BBUFSIZ];
    ULONGLONG ct;
    int     dd, hh, mm, ss, ms;
    DWORD   w;

    if (IS_INVALID_HANDLE(logfhandle))
        return;
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
    WriteFile(logfhandle, buf, (DWORD)strlen(buf), &w, 0);
    WriteFile(logfhandle, str, (DWORD)strlen(str), &w, 0);

    WriteFile(logfhandle, CRLF, 2, &w, 0);
}

static void logprintf(const char *format, ...)
{
    char    bp[MBUFSIZ];
    va_list ap;

    memset(bp, 0, MBUFSIZ);
    va_start(ap, format);
    _vsnprintf(bp, MBUFSIZ - 2, format, ap);
    va_end(ap);
    logwrline(bp);
}

static void logwrtime(const char *hdr)
{
    SYSTEMTIME tt;

    GetLocalTime(&tt);
    logprintf("%-16s : %d-%.2d-%.2d %.2d:%.2d:%.2d",
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
    wchar_t  sfx[4] = { L'.', L'\0', L'\0', L'\0' };
    wchar_t *logpb = 0;
    DWORD rc;
    int i;

    logtickcount = GetTickCount64();

    if (logfilename == 0) {
        if ((CreateDirectoryW(loglocation, 0) == 0) &&
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
    if (GetFileAttributesW(logfilename) != INVALID_FILE_ATTRIBUTES) {
        sfx[1] = L'0';
        logpb = xwcsconcat(logfilename, sfx);
        if (MoveFileExW(logfilename, logpb, 0) == 0)
            return GetLastError();
    }

    if (ssvcstatus.dwCurrentState == SERVICE_START_PENDING)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    logfhandle = CreateFileW(logfilename,
                             GENERIC_WRITE,
                             FILE_SHARE_READ,
                            &sazero, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, 0);

    if (IS_INVALID_HANDLE(logfhandle))
        goto failed;

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
            if (MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING) == 0)
                goto failed;
            xfree(lognn);
            if (ssvcstatus.dwCurrentState == SERVICE_START_PENDING)
                reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
        }
        xfree(logpn);
    }
    xfree(logpb);

    logwrline(SVCBATCH_NAME " " SVCBATCH_VERSION_STR " " SVCBATCH_BUILD_STAMP);
    logwrtime("Log opened");
    return 0;

failed:
    rc = GetLastError();
    if (logpb != 0) {
        MoveFileExW(logpb, logfilename, MOVEFILE_REPLACE_EXISTING);
        xfree(logpb);
    }
    return rc;
}

/**
 * Simple log rotation
 */
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
    static LONG volatile sstarted = 0;
    const char yn[2] = { 'Y', '\n'};
    DWORD wr;
    BOOL  sc;

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
    sc = SetConsoleCtrlHandler(0, 1);
#if defined(_DBGVIEW)
    if (sc == 0)
        dbgprintf(__FUNCTION__, "SetConsoleCtrlHandler failed %d", GetLastError());
#endif
    GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    if (sc) {
        Sleep(100);
        SetConsoleCtrlHandler(0, 0);
    }
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

    /**
     * Wait some time for process to finish.
     *
     * If still active write Y to stdin pipe
     * to handle case when cmd.exe waits for
     * user reply to "Terminate batch job (Y/N)?"
     */
    if (WaitForSingleObject(processended,
                            SVCBATCH_PENDING_WAIT) == WAIT_TIMEOUT) {
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "sending Y to child");
#endif
        WriteFile(stdinputpipewr, yn, 2, &wr, 0);
    }
    reportsvcstatus(SERVICE_STOP_PENDING,
                    SVCBATCH_STOP_HINT + SVCBATCH_PENDING_WAIT);
    /**
     * Wait for main process to finish or times out.
     *
     * We are waiting at most for SVCBATCH_STOP_HINT
     * timeout and then kill all child processes.
     */
    if (WaitForSingleObject(processended, SVCBATCH_STOP_HINT) == WAIT_TIMEOUT) {
        DWORD rc;
        /**
         * WAIT_TIMEOUT means that child is
         * still running and we need to terminate
         * child tree by brute force
         */
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);
        rc = killprocesstree(cchild.dwProcessId, ERROR_INVALID_FUNCTION);
        if (rc != 0)
            svcsyserror(__LINE__, rc, L"killprocesstree");
        rc = killprocessmain(ERROR_INVALID_FUNCTION);
        if (rc != 0)
            svcsyserror(__LINE__, rc, L"killprocessmain");
    }
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_PENDING_WAIT);

#if defined(_DBGVIEW)
    {
        DWORD rc = 0;
        if (GetExitCodeProcess(cchild.hProcess, &rc) == 0)
            dbgprintf(__FUNCTION__, "GetExitCodeProcess failed %d", GetLastError());
        else
            dbgprintf(__FUNCTION__, "GetExitCodeProcess %d", rc);
        dbgprintf(__FUNCTION__, "done");
    }
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

        if ((ReadFile(stdoutputpiper, rb, HBUFSIZ, &rd, 0) == 0) || (rd == 0)) {
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
                xcreatethread(1, &stopthread, 0);
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
            xcreatethread(1, &stopthread, 0);
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

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    memset(&si, 0, sizeof(STARTUPINFOW));
    si.cb         = DSIZEOF(STARTUPINFOW);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdinputpiperd;
    si.hStdOutput = stdoutputpipew;
    si.hStdError  = stdoutputpipew;

    if (CreateProcessW(comspec, cmdline, 0, 0, 1,
                       CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                       wenvblock,
                       servicehome,
                       &si, &cchild) == 0) {
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

    wh[0] = cchild.hProcess;
    wh[1] = xcreatethread(0, &iopipethread, 0);
    if (IS_INVALID_HANDLE(wh[1])) {
        svcsyserror(__LINE__, ERROR_TOO_MANY_TCBS, L"iopipethread");
        CloseHandle(cchild.hThread);
        TerminateProcess(cchild.hProcess, ERROR_OUTOFMEMORY);
        setsvcstatusexit(ERROR_TOO_MANY_TCBS);
        goto finished;
    }

    ResumeThread(cchild.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    CloseHandle(cchild.hThread);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "service running");
#endif
    WaitForMultipleObjects(2, wh, 1, INFINITE);
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
    HANDLE       wh[2] = { 0, 0};

    ssvcstatus.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    ssvcstatus.dwCurrentState = SERVICE_START_PENDING;

    if (argc == 0) {
        svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, L"Missing servicename");
        exit(ERROR_INVALID_PARAMETER);
        return;
    }

    servicename = xwcsdup(argv[0]);
    hsvcstatus  = RegisterServiceCtrlHandlerExW(servicename, servicehandler, 0);
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
    SetConsoleCtrlHandler(consolehandler, 1);
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

    wh[0] = xcreatethread(0, &monitorthread, 0);
    if (IS_INVALID_HANDLE(wh[0])) {
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"monitorthread");
        goto finished;
    }
    wh[1] = xcreatethread(0, &workerthread,  0);
    if (IS_INVALID_HANDLE(wh[1])) {
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"workerthread");
        SignalObjectAndWait(monitorevent, wh[0], INFINITE, 0);
        goto finished;
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "running");
#endif
    /**
     * This is main wait loop that waits
     * for worker and monitor threads to finish.
     */
    WaitForMultipleObjects(2, wh, 1, INFINITE);
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

    SAFE_CLOSE_HANDLE(wh[0]);
    SAFE_CLOSE_HANDLE(wh[1]);

    SAFE_CLOSE_HANDLE(stdoutputpipew);
    SAFE_CLOSE_HANDLE(stdoutputpiper);
    SAFE_CLOSE_HANDLE(stdinputpipewr);
    SAFE_CLOSE_HANDLE(stdinputpiperd);
    SAFE_CLOSE_HANDLE(cchild.hProcess);

    closelogfile();
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
    reportsvcstatus(SERVICE_STOPPED, rv);
}

/**
 * Needed to release conhost.exe
 */
static void __cdecl cconsolecleanup(void)
{
    SetConsoleCtrlHandler(consolehandler, 0);
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
    wchar_t    *bname = 0;
    int         envc  = 0;
    int         cleanpath  = 0;
    int         usesafeenv = 0;
    int         hasopts    = 1;
    HANDLE      hstdin;
    SERVICE_TABLE_ENTRYW se[2];

    if (wenv != 0) {
        while (wenv[envc] != 0)
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
                if (servicehome == 0)
                    return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, p);
                continue;
            }
            if (loglocation == zerostring) {
                loglocation = expandenvstrings(p);
                if (loglocation == 0)
                    return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, p);
                rmtrailingps(loglocation);
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
        if (bname == 0) {
            /**
             * First argument after options is batch file
             * that we are going to execute.
             */
            if ((bname = expandenvstrings(p)) == 0)
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
    if (runningasservice() == 0) {
        fputs("\n" SVCBATCH_NAME " " SVCBATCH_VERSION_STR, stderr);
        fputs(" "  SVCBATCH_BUILD_STAMP, stderr);
        fputs("\n" SVCBATCH_COPYRIGHT "\n\n", stderr);
        fputs("This program can only run as Windows Service\n", stderr);
        return 1;
    }
    if (bname == 0)
        return svcsyserror(__LINE__, 0, L"Missing batch file");

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
    if (resolvesvcbatchexe() == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, wargv[0]);

    if ((opath = xgetenv(L"PATH")) == 0)
        return svcsyserror(__LINE__, ERROR_ENVVAR_NOT_FOUND, L"PATH");

    if ((serviceuuid = xuuidstring()) == 0)
        return svcsyserror(__LINE__, GetLastError(), L"CryptGenRandom");

    /**
     * Get full path to cmd.exe
     */
    if ((cpath = xgetenv(L"COMSPEC")) == 0)
        return svcsyserror(__LINE__, ERROR_ENVVAR_NOT_FOUND, L"COMSPEC");
    if ((comspec = getrealpathname(cpath, 0)) == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, cpath);
    xfree(cpath);

    if ((servicehome == 0) && isrelativepath(bname)) {
        /**
         * Batch file is not absolute path
         * and we don't have provided workdir.
         * Use servicebase as cwd
         */
        servicehome = servicebase;
    }

    if (servicehome != 0) {
        if (SetCurrentDirectoryW(servicehome) == 0)
            return svcsyserror(__LINE__, GetLastError(), servicehome);
    }

    if (resolvebatchname(bname) == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, bname);
    xfree(bname);
    if (servicehome == 0) {
        /**
         * Use batch file directory as new cwd
         */
        servicehome = batchdirname;
        if (SetCurrentDirectoryW(servicehome) == 0)
            return svcsyserror(__LINE__, GetLastError(), servicehome);
    }
    if (loglocation == 0)
        loglocation = xwcsdup(SVCBATCH_LOG_BASE);
    dupwenvp = waalloc(envc + 8);
    for (i = 0; i < envc; i++) {
        const wchar_t **e;
        const wchar_t  *p = wenv[i];

        if (IS_EMPTY_WCS(p))
            continue;
        if (usesafeenv) {
            e = safewinenv;
            p = 0;
            while (*e != 0) {
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
            while (*e != 0) {
                if (strstartswith(p, *e)) {
                    /**
                     * Skip private environment variable
                     */
                    p = 0;
                    break;
                }
                e++;
            }
        }
        if (p != 0)
            dupwenvp[dupwenvc++] = xwcsdup(p);
    }

    if (cleanpath) {
        wchar_t *cp = xwcsvarcat(servicebase, L";",
                                 servicehome,
                                 stdwinpaths, 0);
        xfree(opath);

        if ((opath = expandenvstrings(cp)) == 0)
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
    svcstopended = CreateEventW(&sazero, 1, 1, 0);
    processended = CreateEventW(&sazero, 1, 0, 0);
    monitorevent = CreateEventW(&sazero, 1, 0, 0);
    if (IS_INVALID_HANDLE(processended) ||
        IS_INVALID_HANDLE(svcstopended) ||
        IS_INVALID_HANDLE(monitorevent))
        return svcsyserror(__LINE__, ERROR_OUTOFMEMORY, L"CreateEvent");

    InitializeCriticalSection(&servicelock);
    InitializeCriticalSection(&logfilelock);
    atexit(objectscleanup);

    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = 0;
    se[1].lpServiceProc = 0;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "start service");
#endif
    if (StartServiceCtrlDispatcherW(se) == 0)
        return svcsyserror(__LINE__, GetLastError(), L"StartServiceCtrlDispatcher");
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "done");
#endif
    return 0;
}
