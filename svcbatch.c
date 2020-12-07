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

static volatile LONG         servicectrlnum  = 0;
static SERVICE_STATUS_HANDLE svcstathandle   = 0;
static PROCESS_INFORMATION   cmdexeproc;
static SERVICE_STATUS        servicestatus;
static CRITICAL_SECTION      scmservicelock;
static CRITICAL_SECTION      logservicelock;

static wchar_t **dupwenvp         = 0;
static int       dupwenvc         = 0;
static int       hasctrlbreak     = 0;
/**
 * On error call exit instead reporting
 * SERVICE_STOPPED status.
 */
static int       exitonerror      = 0;

/**
 * Full path to the batch file
 */
static wchar_t  *svcbatchfile     = 0;
static wchar_t  *batchdirname     = 0;
static wchar_t  *svcbatchexe      = 0;
static wchar_t  *servicebase      = 0;

static wchar_t  *logfilename      = 0;
static HANDLE    logfhandle       = 0;

static wchar_t  *comspecexe       = 0;
static wchar_t  *childenviron     = 0;
static wchar_t  *servicename      = 0;
static wchar_t  *servicehome      = 0;
static wchar_t  *serviceuuid      = 0;

/**
 * Signaled by service main thread
 * when the service is about to report
 * SERVICE_STOPPED
 */
static HANDLE    serviceended     = 0;
/**
 * Signaled by service stop thread
 * when stop child process is finished.
 * servicemain must wait for that event
 * after main child is finished or killed
 */
static HANDLE    stopsignaled     = 0;
/**
 * Set when SCM sends Custom signal
 */
static HANDLE    monitorevent     = 0;
/**
 * Set when pipe and process threads end
 */
static HANDLE    processended     = 0;

static HANDLE    redirectedpipewr = 0;
static HANDLE    redirectedpiperd = 0;
static HANDLE    redirectedstdinw = 0;
static HANDLE    redirectedstdinr = 0;
static ULONGLONG logstartedtime;

static wchar_t      zerostring[4] = { L'\0', L'\0', L'\0', L'\0' };

static const wchar_t *stdwinpaths = L";"    \
    L"%SystemRoot%\\System32;"              \
    L"%SystemRoot%;"                        \
    L"%SystemRoot%\\System32\\Wbem;"        \
    L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0";

/**
 * The following environment variables
 * are removed from the provided environment.
 * SVCBATCH_ variables are lated added and
 * are unique for each service instance
 */
static const wchar_t *removeenv[] = {
    L"_=",
    L"!::=",
    L"!;=",
    L"SVCBATCH_VERSION_ABI=",
    L"SVCBATCH_SERVICE_BASE=",
    L"SVCBATCH_SERVICE_HOME=",
    L"SVCBATCH_SERVICE_NAME=",
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

#if 0
static void wafree(wchar_t **array)
{
    wchar_t **ptr = array;

    if (array != 0) {
        while (*ptr != 0)
            free(*(ptr++));
        free(array);
    }
}

static char *xstrdup(const char *s)
{
    char   *p;
    size_t  n;

    if (((s) == 0) || (*(s) == '\0'))
        return 0;
    n = strlen(s);
    p = (char *)xmalloc(n + 2);
    memcpy(p, s, n);
    return p;
}
#endif

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

static size_t xwcslen(const wchar_t *s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    else
        return wcslen(s);
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

static wchar_t *xwcsappend(wchar_t *s1, const wchar_t *s2)
{
    wchar_t *rv;

    if (IS_EMPTY_WCS(s2))
        return s1;
    rv = xwcsconcat(s1, s2);
    xfree(s1);
    return rv;
}

static wchar_t *xwcsvarcat(const wchar_t *p, ...)
{
    const wchar_t *ap;
    wchar_t *cp, *rp;
    size_t  sls[32];
    size_t  len = 0;
    int     cnt = 1;
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

static void fs2bs(wchar_t *s)
{
    while (*s != L'\0') {
        if (*s == L'/')
            *s = L'\\';
        s++;
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
static void rmtrailingsep(wchar_t *s)
{
    int i = (int)xwcslen(s);

    while (--i > 0) {
        if ((s[i] == L'\\') || (s[i] == L';'))
            s[i] = L'\0';
        else
            break;
    }
}

/**
 * Check if the path doesn't start
 * with \ (something like \\?\) or C:\
 */
static int isrelativepath(const wchar_t *path)
{
    wchar_t p = path[0];

    if (p == L'\\')
        return 0;

    if ((p < 128) && (isalpha(p) != 0) && (path[1] == L':'))
        return 0;
    else
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
    char    buff[MBUFSIZ];
    char   *bp;
    size_t  blen = MBUFSIZ - 2;
    int     n;
    va_list ap;

    n = _snprintf(buff, blen,
                 "[%.4lu] %s ",
                 GetCurrentThreadId(),
                 funcname);
    bp = buff + n;
    va_start(ap, format);
    _vsnprintf(bp, blen - n, format, ap);
    va_end(ap);
    buff[MBUFSIZ - 1] = '\0';
    OutputDebugStringA(buff);
}

#endif

static void xwinapierror(wchar_t *buf, DWORD bufsize, DWORD statcode)
{
    DWORD len = 0;

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
         * Remove embedded newline (\r\n) if present.
         */
        while (len-- > 0) {
            if ((buf[len] == L'\r') || (buf[len] == L'\n'))
                buf[len] = L' ';
        }
    }
    else
        _snwprintf(buf, bufsize, L"Unknown Win32 error code: %d", statcode);

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

    _snwprintf(buf, SBUFSIZ - 2, L"svcbatch.c(%d) %s", line, err);
    buf[SBUFSIZ - 1] = L'\0';

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
    DWORD  a[BBUFSIZ];
    DWORD  i, x;
    HANDLE h;
    HANDLE p;
    PROCESSENTRY32W e;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " pid: %d", pid);
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
            if (c == BBUFSIZ) {
                /**
                 * Process has more then 512 child processes !?
                 */
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, " overflow");
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

    for (i = 0; i < c; i++) {
        /**
         * Terminate each child and its children
         */
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, " subtree: %d", a[i]);
#endif
        killprocesstree(a[i], err);
    }
    if (c == BBUFSIZ) {
        /**
         * Do recursive call on overflow
         */
        killprocesstree(pid, err);
    }
    if (pid == cmdexeproc.dwProcessId)
        return 0;
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " killing pid: %d", pid);
#endif
    p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, 0, pid);
    if (GetExitCodeProcess(p, &x)) {
        if (x == STILL_ACTIVE) {
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, " STILL_ACTIVE");
#endif
            if (TerminateProcess(p, err) == 0)
                r = GetLastError();
#if defined(_DBGVIEW)
            if (r)
                dbgprintf(__FUNCTION__, " term failed: %d", r);
#endif
        }
#if defined(_DBGVIEW)
        else {
            dbgprintf(__FUNCTION__, " EXIT_CODE %d ", x);
        }
#endif
    }
    else
        r = GetLastError();
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " done pid: %d rv: %d", pid, r);
#endif
    return r;
}

static DWORD killprocessmain(UINT err)
{
    DWORD x = 0;
    DWORD r = 0;

    if (IS_INVALID_HANDLE(cmdexeproc.hProcess))
        return 0;
    if (GetExitCodeProcess(cmdexeproc.hProcess, &x)) {
        if (x == STILL_ACTIVE) {
            if (TerminateProcess(cmdexeproc.hProcess, err) == 0)
                r = GetLastError();
        }
    }
    else {
        r = GetLastError();
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " main pid: %d %d:%d ", cmdexeproc.dwProcessId, x, r);
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

static wchar_t *expandenvstr(const wchar_t *str)
{
    wchar_t  *buf = 0;
    int       bsz = MBUFSIZ;
    int       len = 0;

    if (wcschr(str, L'%') == 0)
        buf = xwcsdup(str);

    while (buf == 0) {
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
    fs2bs(buf);
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
    if ((es = expandenvstr(path)) == 0)
        return 0;

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
        int i = (int)xwcslen(svcbatchexe);
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
 * and get its directoy
 *
 * Note that input param must have file extension.
 */
static int resolvebatchname(const wchar_t *batch)
{
    int i;

    svcbatchfile = getrealpathname(batch, 0);
    if (IS_EMPTY_WCS(svcbatchfile))
        return 0;

    i = (int)xwcslen(svcbatchfile);
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
            if (strstartswith(name, L"Service-")) {
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "session %S", name);
#endif
                rv = 1;
            }
        }
        if (GetUserObjectInformationW(ws, UOI_FLAGS, &uf,
                                      DSIZEOF(uf), &len)) {
            if (uf.dwFlags == WSF_VISIBLE) {
                /**
                 * Window station has visible display surfaces
                 * Interactive session can lead to unpredictable
                 * results.
                 */
                rv = 0;
            }
        }
    }
    return rv;
}

/**
 * ServiceStatus support functions.
 */
static void setsvcstatusexit(DWORD e)
{
    EnterCriticalSection(&scmservicelock);
    servicestatus.dwServiceSpecificExitCode = e;
    LeaveCriticalSection(&scmservicelock);
}

static DWORD getservicestate(void)
{
    DWORD cs;

    EnterCriticalSection(&scmservicelock);
    cs = servicestatus.dwCurrentState;
    LeaveCriticalSection(&scmservicelock);
    return cs;
}

static void reportsvcstatus(DWORD status, DWORD param)
{
    static DWORD cpcnt = 1;

    EnterCriticalSection(&scmservicelock);
    if (servicestatus.dwCurrentState == SERVICE_STOPPED) {
        status = SERVICE_STOPPED;
        goto finished;
    }
    if (status == 0) {
        SetServiceStatus(svcstathandle, &servicestatus);
        goto finished;
    }
    servicestatus.dwControlsAccepted = 0;
    servicestatus.dwCheckPoint       = 0;
    servicestatus.dwWaitHint         = 0;

    if (status == SERVICE_RUNNING) {
        servicestatus.dwControlsAccepted =  SERVICE_ACCEPT_STOP |
                                            SERVICE_ACCEPT_SHUTDOWN;
#if defined(SERVICE_ACCEPT_PRESHUTDOWN)
        servicestatus.dwControlsAccepted |= SERVICE_ACCEPT_PRESHUTDOWN;
#endif
        cpcnt = 1;
    }
    else if (status == SERVICE_STOPPED) {
        if (param != 0)
            servicestatus.dwServiceSpecificExitCode = param;
        if (servicestatus.dwServiceSpecificExitCode == 0 &&
            servicestatus.dwCurrentState != SERVICE_STOP_PENDING)
            servicestatus.dwServiceSpecificExitCode = ERROR_PROCESS_ABORTED;
        if (servicestatus.dwServiceSpecificExitCode != 0) {
            servicestatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
            if (exitonerror) {
                /**
                 * Use exit so that SCM can kick Recovery
                 */
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, " exit(%d)",
                          servicestatus.dwServiceSpecificExitCode);
#endif
                exit(servicestatus.dwServiceSpecificExitCode);
            }
        }
    }
    else {
        servicestatus.dwCheckPoint = cpcnt++;
        servicestatus.dwWaitHint   = param;
    }
    servicestatus.dwCurrentState = status;
    if (SetServiceStatus(svcstathandle, &servicestatus) == 0)
        svcsyserror(__LINE__, GetLastError(), L"SetServiceStatus");

finished:
    LeaveCriticalSection(&scmservicelock);
}

static PSECURITY_ATTRIBUTES getnullacl(BOOL inheritable)
{
    static BYTE sb[SECURITY_DESCRIPTOR_MIN_LENGTH];
    static SECURITY_ATTRIBUTES  sa;
    static PSECURITY_DESCRIPTOR sd = 0;

    if (sd == 0) {
        sd = (PSECURITY_DESCRIPTOR)sb;
        if ((InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) == 0) ||
            (SetSecurityDescriptorDacl(sd, 1, (PACL)0, 0) == 0)) {
            return 0;
        }
    }
    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle       = inheritable;
    return &sa;
}

static DWORD createiopipes(void)
{
    LPSECURITY_ATTRIBUTES sa;
    DWORD  rc = 0;
    HANDLE sh = 0;
    HANDLE cp = GetCurrentProcess();

    if ((sa = getnullacl(1)) == 0)
        return svcsyserror(__LINE__, ERROR_ACCESS_DENIED, L"SetSecurityDescriptorDacl");
    /**
     * Create stdin pipe
     */
    if (CreatePipe(&redirectedstdinr, &sh, sa, 0) == 0) {
        rc = svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
        goto finished;
    }
    /**
     * Create write side of the pipe as non
     * inheritable
     */
    if (DuplicateHandle(cp, sh, cp,
                        &redirectedstdinw, 0, 0,
                        DUPLICATE_SAME_ACCESS) == 0) {
        rc = svcsyserror(__LINE__, GetLastError(), L"DuplicateHandle");
        goto finished;
    }
    /**
     * Close the inheritable write handle
     */
    SAFE_CLOSE_HANDLE(sh);

    /**
     * Create stdout/stderr pipe
     */
    if (CreatePipe(&sh, &redirectedpipewr, sa, 0) == 0) {
        rc = svcsyserror(__LINE__, GetLastError(), L"CreatePipe");
        goto finished;
    }
    /**
     * Create read side of the pipe as non
     * inheritable
     */
    if (DuplicateHandle(cp, sh, cp,
                        &redirectedpiperd, 0, 0,
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

    if (SetFilePointerEx(logfhandle, ee, 0, FILE_END) == 0)
        return GetLastError();
    if (WriteFile(logfhandle, buf, len, &w, 0) == 0)
        return GetLastError();
    else
        return 0;
}

/**
 * Set file pointer to the end and write CRLF
 * Always use logfflush when start
 * logging so that pipe thread data is separated
 * from our log messages
 */
static void logfflush(void)
{
    char  b[2] = {'\r', '\n'};

    FlushFileBuffers(logfhandle);
    logappend(b, 2);
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
    ct = GetTickCount64() - logstartedtime;
    ms = (int)((ct % MS_IN_SECOND));
    ss = (int)((ct / MS_IN_SECOND) % 60);
    mm = (int)((ct / MS_IN_MINUTE) % 60);
    hh = (int)((ct / MS_IN_HOUR)   % 24);
    dd = (int)((ct / MS_IN_DAY));

    _snprintf(buf, BBUFSIZ - 2,
              "[%.2d:%.2d:%.2d:%.2d.%.3d] [%.4lu:%.4lu] ",
              dd, hh, mm, ss, ms,
              GetCurrentProcessId(),
              GetCurrentThreadId());
    buf[BBUFSIZ - 1] = '\0';
    WriteFile(logfhandle, buf, (DWORD)strlen(buf), &w, 0);
    WriteFile(logfhandle, str, (DWORD)strlen(str), &w, 0);

    buf[0] = '\r';
    buf[1] = '\n';

    WriteFile(logfhandle, buf, 2, &w, 0);
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

/**
 *
 * Create service log file and rotate any previous
 * files in the Logs directory.
 */
static DWORD opensvclog(int ssp)
{
    HANDLE   lh = 0;
    wchar_t *logfn;
    wchar_t  sfx[4] = { L'.', L'\0', L'\0', L'\0' };
    int i;
    SECURITY_ATTRIBUTES sa;
    SYSTEMTIME tt;

    logstartedtime = GetTickCount64();

    if ((logfn = logfilename) == 0) {
        wchar_t *ld = xwcsconcat(servicehome, L"\\Logs");

        if ((CreateDirectoryW(ld, 0) == 0) &&
            (GetLastError() != ERROR_ALREADY_EXISTS))
            return GetLastError();
        logfn = xwcsappend(ld, L"\\SvcBatch.log");
    }
    if (GetFileAttributesW(logfn) != INVALID_FILE_ATTRIBUTES) {
        wchar_t *lognn;

        sfx[1] = L'0';
        lognn = xwcsconcat(logfn, sfx);
        if (MoveFileExW(logfn, lognn, 0) == 0)
            return GetLastError();
        xfree(lognn);
    }
    GetLocalTime(&tt);
    sa.nLength = DSIZEOF(sa);
    sa.lpSecurityDescriptor = 0;
    sa.bInheritHandle       = 0;

    if (ssp)
        reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    lh = CreateFileW(logfn,
                     GENERIC_WRITE,
                     FILE_SHARE_READ,
                     &sa, CREATE_NEW,
                     FILE_ATTRIBUTE_NORMAL, 0);

    if (IS_INVALID_HANDLE(lh))
        return GetLastError();

    logfilename = logfn;
    logfhandle  = lh;
    /**
     * Rotate previous log files
     */
    for (i = SVCBATCH_MAX_LOGS; i > 0; i--) {
        wchar_t *logpn;

        sfx[1] = L'0' + i - 1;
        logpn  = xwcsconcat(logfn, sfx);

        if (GetFileAttributesW(logpn) != INVALID_FILE_ATTRIBUTES) {
            wchar_t *lognn;

            sfx[1] = L'0' + i;
            lognn = xwcsconcat(logfn, sfx);
            if (MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING) == 0)
                return GetLastError();
            xfree(lognn);
            if (ssp)
                reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
        }
        xfree(logpn);
    }

    logwrline(SVCBATCH_NAME " " SVCBATCH_VERSION_STR " " SVCBATCH_BUILD_STAMP);
    logprintf("Log opened       : %d-%.2d-%.2d %.2d:%.2d:%.2d",
               tt.wYear, tt.wMonth, tt.wDay,
               tt.wHour, tt.wMinute, tt.wSecond);

    return 0;
}
/**
 * Write basic configuration when new logfile is created.
 */
static void logconfig(void)
{
    logprintf("Service name     : %S", servicename);
    logprintf("Service id       : %S", serviceuuid);
    logprintf("Batch file       : %S", svcbatchfile);
    logprintf("Base directory   : %S", servicebase);
    logprintf("Working directory: %S", servicehome);
    logfflush();
}

/**
 * Simple log rotation
 */
static DWORD rotatesvclog(void)
{
    static int rotatecount = 1;
    DWORD rv;

    EnterCriticalSection(&logservicelock);
    if (IS_VALID_HANDLE(logfhandle)) {
        SYSTEMTIME tt;
        GetLocalTime(&tt);
        logfflush();
        logprintf("Log rotated %d-%.2d-%.2d %.2d:%.2d:%.2d",
                  tt.wYear, tt.wMonth, tt.wDay,
                  tt.wHour, tt.wMinute, tt.wSecond);
        FlushFileBuffers(logfhandle);
        SAFE_CLOSE_HANDLE(logfhandle);
    }
    if ((rv = opensvclog(0)) == 0) {
        logprintf("Log generation   : %d", rotatecount++);
        logconfig();
    }
    LeaveCriticalSection(&logservicelock);
    if (rv) {
        setsvcstatusexit(rv);
        svcsyserror(__LINE__, rv, L"rotatesvclog");
    }
    return rv;
}

static void closesvclog(void)
{
    EnterCriticalSection(&logservicelock);
    if (IS_VALID_HANDLE(logfhandle)) {
        SYSTEMTIME tt;
        GetLocalTime(&tt);

        logfflush();
        logprintf("Log closed %d-%.2d-%.2d %.2d:%.2d:%.2d",
                   tt.wYear, tt.wMonth, tt.wDay,
                   tt.wHour, tt.wMinute, tt.wSecond);
        FlushFileBuffers(logfhandle);
        SAFE_CLOSE_HANDLE(logfhandle);
    }
    LeaveCriticalSection(&logservicelock);
}

static DWORD WINAPI svcstopthread(LPVOID unused)
{
    static LONG volatile sstarted = 0;
    const char yn[2] = { 'Y', '\n'};
    DWORD wr;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "   started");
#endif

    if (getservicestate() != SERVICE_RUNNING) {
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "   not running");
#endif
        XENDTHREAD(0);
    }
    if (InterlockedIncrement(&sstarted) > 1) {
#if defined(_DBGVIEW)
        dbgprintf(__FUNCTION__, "   already started");
#endif
        XENDTHREAD(0);
    }
    /**
     * Set stop event to non signaled.
     * This ensures that main thread will wait until we finish
     */
    ResetEvent(stopsignaled);
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT);

    EnterCriticalSection(&logservicelock);
    if (IS_VALID_HANDLE(logfhandle)) {
        logfflush();
        logwrline("Service STOP signaled\r\n");
    }
    LeaveCriticalSection(&logservicelock);

    /**
     * Calling SetConsoleCtrlHandler with the NULL and TRUE arguments
     * causes the calling process to ignore CTRL+C signals.
     * This attribute is inherited by child processes, but it can be
     * enabled or disabled by any process without affecting existing processes.
     */
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "   CTRL_C_EVENT raised");
#endif
    SetConsoleCtrlHandler(0, 1);
    GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    SetConsoleCtrlHandler(0, 0);
    /**
     * Wait some time for process to finish.
     *
     * If still active write Y to stdin pipe
     * to handle case when cmd.exe waits for
     * user reply to "Terminate batch job (Y/N)?"
     */
    if (WaitForSingleObject(processended,
                            SVCBATCH_PENDING_WAIT) == WAIT_TIMEOUT)
        WriteFile(redirectedstdinw, yn, 2, &wr, 0);

    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_STOP_HINT + SVCBATCH_PENDING_WAIT);
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
         * cmdexeproc tree by brute force
         */
        reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_PENDING_WAIT);
        rc = killprocesstree(cmdexeproc.dwProcessId, ERROR_INVALID_FUNCTION);
        if (rc != 0)
            svcsyserror(__LINE__, rc, L"killprocesstree");
        rc = killprocessmain(ERROR_INVALID_FUNCTION);
        if (rc != 0)
            svcsyserror(__LINE__, rc, L"killprocessmain");
    }
    reportsvcstatus(SERVICE_STOP_PENDING, SVCBATCH_PENDING_WAIT);

#if defined(_DBGVIEW)
    {
        DWORD rc;
        if (GetExitCodeProcess(cmdexeproc.hProcess, &rc) == 0)
            dbgprintf(__FUNCTION__, "   GetExitCodeProcess failed");
        else
            dbgprintf(__FUNCTION__, "   GetExitCodeProcess %d", rc);
        dbgprintf(__FUNCTION__, "   done");
    }
#endif

    SetEvent(stopsignaled);
    XENDTHREAD(0);
}

static DWORD WINAPI svcpipethread(LPVOID unused)
{
    DWORD  rc = 0;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "   started");
#endif
    for (;;) {
        BYTE  rb[HBUFSIZ];
        DWORD rd = 0;

        if ((ReadFile(redirectedpiperd, rb, HBUFSIZ, &rd, 0) == 0) || (rd == 0)) {
            /**
             * Read from child failed.
             * ERROR_BROKEN_PIPE or ERROR_NO_DATA means that
             * child process closed its side of the pipe.
             */
            rc = GetLastError();
            break;
        }

        EnterCriticalSection(&logservicelock);
        if (IS_VALID_HANDLE(logfhandle))
            rc = logappend(rb, rd);
        else
            rc = ERROR_NO_MORE_FILES;
        LeaveCriticalSection(&logservicelock);

        if (rc != 0)
            break;
    }
#if defined(_DBGVIEW)
    if (rc == ERROR_BROKEN_PIPE)
        dbgprintf(__FUNCTION__, "   done ERROR_BROKEN_PIPE");
    else if (rc == ERROR_NO_DATA)
        dbgprintf(__FUNCTION__, "   done ERROR_NO_DATA");
    else
        dbgprintf(__FUNCTION__, "   done %d", rc);
#endif

    XENDTHREAD(0);
}

static DWORD WINAPI svcmonitorthread(LPVOID unused)
{
    DWORD  wr;

#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "started");
#endif

    while ((wr = WaitForSingleObject(monitorevent, INFINITE)) == WAIT_OBJECT_0) {
        LONG cc = InterlockedExchange(&servicectrlnum, 0);

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
            EnterCriticalSection(&logservicelock);
            if (IS_VALID_HANDLE(logfhandle)) {
                logfflush();
                logwrline("CTRL_BREAK_EVENT signaled\r\n");
            }
            LeaveCriticalSection(&logservicelock);
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
            dbgprintf(__FUNCTION__, "rotate signaled");
#endif
            if (rotatesvclog() != 0) {
#if defined(_DBGVIEW)
                dbgprintf(__FUNCTION__, "rotate log failed");
#endif
                /**
                 * Logfile rotation failed.
                 * Create stop thread which will stop the service.
                 */
                xcreatethread(1, &svcstopthread, 0);
                break;
            }
        }
        ResetEvent(monitorevent);
    }
#if defined(_DBGVIEW)
    if (wr != WAIT_OBJECT_0)
        dbgprintf(__FUNCTION__, "wait failed: %d", wr);
    dbgprintf(__FUNCTION__, "done");
#endif
    XENDTHREAD(0);
}

static DWORD WINAPI svcworkerthread(LPVOID unused)
{
    STARTUPINFOW si;
    wchar_t *cmdline;
    wchar_t *arg0;
    wchar_t *arg1;
    HANDLE   wh[2] = {0, 0};

    memset(&si, 0, sizeof(STARTUPINFOW));
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " started");
#endif
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    /**
     * Create a command line
     */
    if (wcshavespace(comspecexe))
        arg0 = xwcsvarcat(L"\"", comspecexe,   L"\"", 0);
    else
        arg0 = comspecexe;
    if (wcshavespace(svcbatchfile))
        arg1 = xwcsvarcat(L"\"", svcbatchfile, L"\"", 0);
    else
        arg1 = svcbatchfile;
    /**
     * Everything after /C has to be one argument
     */
    cmdline = xwcsvarcat(arg0, L" /D /C \"", arg1, L"\"", 0);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " program %S", comspecexe);
    dbgprintf(__FUNCTION__, " cmdline %S", cmdline);
#endif

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    si.cb         = DSIZEOF(STARTUPINFOW);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = redirectedstdinr;
    si.hStdOutput = redirectedpipewr;
    si.hStdError  = redirectedpipewr;

    if (CreateProcessW(comspecexe, cmdline, 0, 0, 1,
                       CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                       childenviron,
                       servicehome,
                       &si, &cmdexeproc) == 0) {
        /**
         * CreateProcess failed ... nothing we can do.
         */
        setsvcstatusexit(GetLastError());
        svcsyserror(__LINE__, GetLastError(), L"CreateProcess");
        goto finished;
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " childid %d", cmdexeproc.dwProcessId);
#endif

    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(redirectedpipewr);
    SAFE_CLOSE_HANDLE(redirectedstdinr);

    wh[0] = cmdexeproc.hProcess;
    wh[1] = xcreatethread(0, &svcpipethread, 0);
    if (IS_INVALID_HANDLE(wh[1])) {
        svcsyserror(__LINE__, ERROR_TOO_MANY_TCBS, L"svcpipethread");
        CloseHandle(cmdexeproc.hThread);
        TerminateProcess(cmdexeproc.hProcess, ERROR_OUTOFMEMORY);
        setsvcstatusexit(ERROR_TOO_MANY_TCBS);
        goto finished;
    }

    ResumeThread(cmdexeproc.hThread);
    reportsvcstatus(SERVICE_RUNNING, 0);
    CloseHandle(cmdexeproc.hThread);

    WaitForMultipleObjects(2, wh, 1, INFINITE);
    CloseHandle(wh[1]);

finished:
    SetEvent(processended);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, " done rv %d", servicestatus.dwServiceSpecificExitCode);
#endif
    InterlockedExchange(&servicectrlnum, 0);
    SetEvent(monitorevent);

    XENDTHREAD(0);
}

BOOL WINAPI consolehandler(DWORD ctrl)
{
    switch(ctrl) {
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            /**
             * Wait for SvcBatch to terminate, but respond
             * after a reasonable time to tell the system
             * that we did attempt to shut ourselves down.
             *
             * Use 30 seconds as timeout value
             * which is the time according to MSDN for
             * services to react on system shutdown
             */
            EnterCriticalSection(&logservicelock);
            if (IS_VALID_HANDLE(logfhandle)) {
                logfflush();
                logwrline("CTRL_SHUTDOWN_EVENT signaled");
            }
            LeaveCriticalSection(&logservicelock);
            WaitForSingleObject(serviceended, SVCBATCH_STOP_WAIT);
        break;
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_LOGOFF_EVENT:
            /**
             * TRUE means that we handled the signal
             */
        break;
        default:
#if defined(_DBGVIEW)
            dbgprintf(__FUNCTION__, "  unknown ctrl %d", ctrl);
#endif
            return FALSE;
        break;
    }
    return TRUE;
}

DWORD WINAPI servicehandler(DWORD ctrl, DWORD _xe, LPVOID _xd, LPVOID _xc)
{
    switch(ctrl) {
        case SERVICE_CONTROL_SHUTDOWN:
#if defined(SERVICE_CONTROL_PRESHUTDOWN)
        case SERVICE_CONTROL_PRESHUTDOWN:
#endif
        case SERVICE_CONTROL_STOP:
            xcreatethread(1, &svcstopthread, 0);
        break;
        case SVCBATCH_CTRL_BREAK:
            if (hasctrlbreak == 0)
                return ERROR_CALL_NOT_IMPLEMENTED;
        case SVCBATCH_CTRL_ROTATE:
            /**
             * Signal to svcmonitorthread that
             * user send custom service control
             */
            InterlockedExchange(&servicectrlnum, ctrl);
            SetEvent(monitorevent);
        case SERVICE_CONTROL_INTERROGATE:
            reportsvcstatus(0, 0);
        break;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
        break;
    }
    return 0;
}

/**
 * Main Service function
 * This thread is created by SCM. SCM should provide
 * the service name as first argument.
 */
void WINAPI servicemain(DWORD argc, wchar_t **argv)
{
    int          i;
    DWORD        rv    = 0;
    size_t       eblen = 0;
    wchar_t     *ep;
    HANDLE       wh[2] = { 0, 0};

    servicestatus.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    servicestatus.dwCurrentState = SERVICE_START_PENDING;

    if (argc == 0) {
        svcsyserror(__LINE__, ERROR_INVALID_PARAMETER, L"Missing servicename");
        exit(ERROR_INVALID_PARAMETER);
        return;
    }

    servicename   = xwcsdup(argv[0]);
    svcstathandle = RegisterServiceCtrlHandlerExW(servicename, servicehandler, 0);
    if (IS_INVALID_HANDLE(svcstathandle)) {
        svcsyserror(__LINE__, GetLastError(), L"RegisterServiceCtrlHandlerEx");
        exit(ERROR_INVALID_HANDLE);
        return;
    }
    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "     started %S", servicename);
#endif

    if ((rv = opensvclog(1)) != 0) {
        svcsyserror(__LINE__, rv, L"OpenLog");
        exit(rv);
        return;
    }
    logconfig();

    reportsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    /**
     * Add additional environment variables
     * They are unique to this service instance
     */
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_VERSION_ABI=",  CPP_WIDEN(SVCBATCH_VERSION_ABI));
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_BASE=", servicebase);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_HOME=", servicehome);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_NAME=", servicename);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_SELF=", svcbatchexe);
    dupwenvp[dupwenvc++] = xwcsconcat(L"SVCBATCH_SERVICE_UUID=", serviceuuid);

    qsort((void *)dupwenvp, dupwenvc, sizeof(wchar_t *), envsort);
    for (i = 0; i < dupwenvc; i++) {
        eblen += xwcslen(dupwenvp[i]) + 1;
    }
    childenviron = xwalloc(eblen + 2);
    for (i = 0, ep = childenviron; i < dupwenvc; i++) {
        eblen = xwcslen(dupwenvp[i]);
        wmemcpy(ep, dupwenvp[i], eblen);
        ep += eblen + 1;
    }

    if ((rv = createiopipes()) != 0)
        goto finished;
    wh[0] = xcreatethread(0, &svcmonitorthread, 0);
    if (IS_INVALID_HANDLE(wh[0])) {
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"svcmonitorthread");
        goto finished;
    }
    wh[1] = xcreatethread(0, &svcworkerthread,  0);
    if (IS_INVALID_HANDLE(wh[1])) {
        rv = ERROR_TOO_MANY_TCBS;
        svcsyserror(__LINE__, rv, L"svcworkerthread");
        SignalObjectAndWait(monitorevent, wh[0], INFINITE, 0);
        goto finished;
    }
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "     running");
#endif
    /**
     * This is main wait loop that waits
     * for worker and monitor threads to finish.
     */
    WaitForMultipleObjects(2, wh, 1, INFINITE);
    /**
     * Wait for stopthread signal
     *
     * stopsignaled event is created as signaled
     * so we only wait if stop thread is still running
     */
    WaitForSingleObject(stopsignaled, SVCBATCH_STOP_WAIT);

finished:

    SAFE_CLOSE_HANDLE(wh[0]);
    SAFE_CLOSE_HANDLE(wh[1]);

    SAFE_CLOSE_HANDLE(redirectedpipewr);
    SAFE_CLOSE_HANDLE(redirectedpiperd);
    SAFE_CLOSE_HANDLE(redirectedstdinw);
    SAFE_CLOSE_HANDLE(redirectedstdinr);
    SAFE_CLOSE_HANDLE(cmdexeproc.hProcess);

    closesvclog();
#if defined(_DBGVIEW)
    dbgprintf(__FUNCTION__, "     done");
#endif
    SetEvent(serviceended);
    reportsvcstatus(SERVICE_STOPPED, rv);
}

/**
 * Needed to release conhost.exe
 */
static void __cdecl svcconsolecleanup(void)
{
    SetConsoleCtrlHandler(consolehandler, 0);
    FreeConsole();
}

/**
 * Cleanup created resources ...
 */
static void __cdecl svcobjectscleanup(void)
{
    DeleteCriticalSection(&logservicelock);
    DeleteCriticalSection(&scmservicelock);

    SAFE_CLOSE_HANDLE(processended);
    SAFE_CLOSE_HANDLE(stopsignaled);
    SAFE_CLOSE_HANDLE(serviceended);
    SAFE_CLOSE_HANDLE(monitorevent);
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
    SECURITY_ATTRIBUTES  sa;
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
            return svcsyserror(__LINE__, 0, L"Empty cmdline argument");
        if (hasopts) {
            if (servicehome == zerostring) {
                servicehome = getrealpathname(p, 1);
                if (servicehome == 0)
                    return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, p);
                continue;
            }
            hasopts = 0;
            if ((p[0] == L'-') || (p[0] == L'/')) {
                if (p[1] == L'\0')
                    return svcsyserror(__LINE__, 0, L"Invalid cmdline option");
                if (p[2] == L'\0')
                    hasopts = 1;
                else if (p[0] == L'-')
                    return svcsyserror(__LINE__, 0, L"Invalid cmdline option");
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
#if defined(_DBGVIEW)
                    case L'r':
                    case L'R':
                        exitonerror  = 1;
                    break;
#endif
                    case L's':
                    case L'S':
                        usesafeenv   = 1;
                    break;
                    case L'w':
                    case L'W':
                        servicehome = zerostring;
                    break;
                    default:
                        return svcsyserror(__LINE__, 0, L"Unknown cmdline option");
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
            if ((bname = expandenvstr(p)) == 0)
                return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, p);
        }
        else {
            /**
             * extra parameters
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
        atexit(svcconsolecleanup);
        hstdin = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (IS_INVALID_HANDLE(hstdin))
        return svcsyserror(__LINE__, GetLastError(), L"GetStdHandle");
    if (SetConsoleCtrlHandler(consolehandler, 1) == 0)
        return svcsyserror(__LINE__, GetLastError(), L"SetConsoleCtrlHandler");

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
    if ((comspecexe = getrealpathname(cpath, 0)) == 0)
        return svcsyserror(__LINE__, ERROR_FILE_NOT_FOUND, cpath);
    xfree(cpath);

    if (servicehome == 0 && isrelativepath(bname)) {
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

    if (servicehome == 0) {
        /**
         * Use batch file directory as new cwd
         */
        servicehome = batchdirname;
        if (SetCurrentDirectoryW(servicehome) == 0)
            return svcsyserror(__LINE__, GetLastError(), servicehome);
    }

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

        if ((opath = expandenvstr(cp)) == 0)
            return svcsyserror(__LINE__, ERROR_PATH_NOT_FOUND, cp);
        xfree(cp);
    }
    else {
        fs2bs(opath);
        rmtrailingsep(opath);
    }
    dupwenvp[dupwenvc++] = xwcsconcat(L"PATH=", opath);
    xfree(opath);

    memset(&cmdexeproc,    0, sizeof(PROCESS_INFORMATION));
    memset(&servicestatus, 0, sizeof(SERVICE_STATUS));

    sa.nLength              = DSIZEOF(sa);
    sa.bInheritHandle       = 0;
    sa.lpSecurityDescriptor = 0;
    /**
     * Create logic state events
     */
    stopsignaled = CreateEventW(&sa, 1, 1, 0);
    processended = CreateEventW(&sa, 1, 0, 0);
    serviceended = CreateEventW(&sa, 1, 0, 0);
    monitorevent = CreateEventW(&sa, 1, 0, 0);
    if (IS_INVALID_HANDLE(serviceended) ||
        IS_INVALID_HANDLE(stopsignaled) ||
        IS_INVALID_HANDLE(processended) ||
        IS_INVALID_HANDLE(monitorevent))
        return svcsyserror(__LINE__, ERROR_OUTOFMEMORY, L"CreateEvent");

    InitializeCriticalSection(&scmservicelock);
    InitializeCriticalSection(&logservicelock);
    atexit(svcobjectscleanup);

    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = 0;
    se[1].lpServiceProc = 0;

    if (StartServiceCtrlDispatcherW(se) == 0)
        return svcsyserror(__LINE__, GetLastError(), L"StartServiceCtrlDispatcher");
#if defined(_DBGVIEW)
    OutputDebugStringA("Game over");
#endif
    return 0;
}
