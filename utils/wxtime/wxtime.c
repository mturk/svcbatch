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

/**
 * Disable or reduce the frequency of...
 *   C4100: unreferenced formal parameter
 *   C4244: int to char/short - precision loss
 *   C4389: signed/unsigned mismatch
 *   C4702: unreachable code
 *   C4996: function was declared deprecated
 */
#pragma warning(disable: 4100 4244 4389 4702 4996)

#define CPP_INT64_C(_v)         (_v##I64)
#define CPP_UINT64_C(_v)        (_v##UI64)
#define INT64_ZERO              0I64
#define UINT64_ZERO             0UI64

#define IS_INVALID_HANDLE(_h)   (((_h) == NULL) || ((_h) == INVALID_HANDLE_VALUE))
#define IS_VALID_HANDLE(_h)     (((_h) != NULL) && ((_h) != INVALID_HANDLE_VALUE))
#define DSIZEOF(_s)             (DWORD)(sizeof(_s))

static LONGLONG         _nanocrt_counterbase        = INT64_ZERO;
static LONGLONG         _nanocrt_counterfreq        = INT64_ZERO;

/** Intrinsic functions */
void * __cdecl memset(void *, int, size_t);
#pragma intrinsic(memset)
#pragma function(memset)
void * __cdecl memset(void *s, int c, size_t n)
{
    unsigned char *d = (unsigned char *)s;

    while (n-- > 0)
        *d++ = (unsigned char)c;
    return s;
}

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    OutputDebugStringA("\r\n>>>\r\n");
    OutputDebugStringA("Signaled consolehandler\r\n");

    switch (ctrl) {
        case CTRL_C_EVENT:
            OutputDebugStringA("Signaled CTRL_C_EVENT\r\n");
        break;
        case CTRL_BREAK_EVENT:
            OutputDebugStringA("Signaled CTRL_BREAK_EVENT\r\n");
            Sleep(1000);
            OutputDebugStringA("Signaled CTRL_BREAK_EVENT\r\n");
            return FALSE;
        break;
        default:
        break;
    }
    OutputDebugStringA("<<<\r\n\r\n");
    return TRUE;
}

static __inline int xisblank(int ch)
{
    if ((ch > 0) && (ch < 33))
        return 1;
    else
        return 0;
}

static LPWSTR xwcschr(LPCWSTR str, int c)
{
    while (*str) {
        if (*str == c)
            return (LPWSTR)str;
        str++;
    }
    return NULL;
}

static int wprocessRun(LPWSTR cmdblk)
{
    DWORD rc = 0;
    PROCESS_INFORMATION cp;
    STARTUPINFOW si;

    memset(&cp, 0, sizeof(PROCESS_INFORMATION));
    memset(&si, 0, sizeof(STARTUPINFOW));

    si.cb = DSIZEOF(STARTUPINFOW);

    if (!CreateProcessW(NULL, cmdblk,
                        NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        NULL, NULL,
                       &si, &cp)) {
        rc = GetLastError();
    }
    else {
        ResumeThread(cp.hThread);
        CloseHandle(cp.hThread);
        /**
         * Wait for child to finish
         */
        OutputDebugStringA("\r\nRunning ...\r\n");
        OutputDebugStringA("<<<\r\n");
        WaitForSingleObject(cp.hProcess, INFINITE);
        GetExitCodeProcess(cp.hProcess, &rc);
        CloseHandle(cp.hProcess);
    }

    return rc;
}


int WINAPI wxtimeMain(void)
{
    int             rv;
    STARTUPINFOW    si;
    LARGE_INTEGER   q;
    LARGE_INTEGER   c;
    WCHAR           cmd[8192];
    LPWSTR          cmdline;
    LPWSTR          args;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    QueryPerformanceFrequency(&q);
    QueryPerformanceCounter(&c);
    _nanocrt_counterfreq = q.QuadPart;
    _nanocrt_counterbase = c.QuadPart;


    OutputDebugStringA("\r\n>>>\r\n");
    OutputDebugStringA("Hello world!\r\n");

    cmdline = GetCommandLineW();
    OutputDebugStringW(cmdline);
    if (*cmdline == L'"') {
        args = xwcschr(cmdline + 1, '"');
        if (args)
            args++;
    }
    else {
        args = xwcschr(cmdline, ' ');
    }
    if (args == NULL) {
        OutputDebugStringA("\r\n>>>\r\n");
        OutputDebugStringA("Missing exec\r\n");
        OutputDebugStringA("<<<\r\n\r\n");

        return ERROR_INVALID_PARAMETER;
    }
    while (xisblank(*args))
        args++;
    lstrcpyW(cmd, args);
    OutputDebugStringW(cmd);

    memset(&si, 0, sizeof(STARTUPINFOW));
    GetStartupInfoW(&si);

    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);

    if (IS_INVALID_HANDLE(si.hStdInput))
        OutputDebugStringA("Invalid STARTUPINFOW.hStdInput HANDLE\r\n");
    if (IS_INVALID_HANDLE(si.hStdError))
        OutputDebugStringA("Invalid STARTUPINFOW.StdError  HANDLE\r\n");
    if (IS_INVALID_HANDLE(si.hStdOutput))
        OutputDebugStringA("Invalid STARTUPINFOW.StdOutput HANDLE\r\n");

    /** Process CTRL+C */
    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consolehandler, TRUE);

    rv = wprocessRun(cmd);

    SetConsoleCtrlHandler(consolehandler, FALSE);
    OutputDebugStringA("\r\n>>>\r\n");
    OutputDebugStringA("Done\r\n");
    OutputDebugStringA("<<<\r\n\r\n");

    ExitProcess(rv);
    return rv;
}
