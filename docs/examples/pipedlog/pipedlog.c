/*
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
# pragma warning(disable: 4100 4244 4702)
#endif
#define SDBUFSIZE 2048
#define RDBUFSIZE 8192

/**
 * Set to 1 to simulate failure
 */
#define SIMULATE_FAILURE 0

static const char *progname = "pipedlog";
static char CRLFA[2] = { '\r', '\n'};
static char SMODE[2] = { 'x',  '\0'};

static void dbgprints(const char *funcname, const char *string)
{
    char buf[SDBUFSIZE];
    _snprintf(buf, SDBUFSIZE - 1,
              "[%.4lu] %s %-16s %s",
              GetCurrentThreadId(),
              SMODE, funcname, string);
     buf[SDBUFSIZE - 1] = '\0';
     OutputDebugStringA(buf);
}

static void dbgprintf(const char *funcname, const char *format, ...)
{
    char    buf[SDBUFSIZE];
    va_list ap;

    va_start(ap, format);
    _vsnprintf(buf, SDBUFSIZE - 1, format, ap);
    va_end(ap);
    buf[SDBUFSIZE - 1] = '\0';
    dbgprints(funcname, buf);
}

#if SIMULATE_FAILURE
DWORD WINAPI failurethread(void *unused)
{
    dbgprints(progname, "simulating failure");
    Sleep(10000);
    dbgprints(progname, "calling ExitProcess(ERROR_WRITE_FAULT)");
    ExitProcess(ERROR_WRITE_FAULT);
}
#endif

int wmain(int argc, const wchar_t **wargv)
{
    int i;
    DWORD    e = 0;
    DWORD    c = 0;
    HANDLE   w, r;
    BYTE     b[RDBUFSIZE];
    SECURITY_ATTRIBUTES sa;

    GetEnvironmentVariableA("SVCBATCH_SERVICE_MODE", SMODE, 2);
    dbgprints(progname, "started");
    r = GetStdHandle(STD_INPUT_HANDLE);
    if (r == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        dbgprints(progname, "Missing stdin handle");
        return e;
    }
    if (argc < 2) {
        dbgprints(progname, "Missing logfile argument");
        return ERROR_INVALID_PARAMETER;
    }
    if (argc > 2) {
        dbgprints(progname, "extra arguments [[");
        for (i = 2; i < argc; i++) {
            dbgprintf(progname, "  %S", wargv[i]);
        }
        dbgprints(progname, "]]");
    }

    memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
    sa.nLength = (DWORD)sizeof(SECURITY_ATTRIBUTES);

    w = CreateFileW(wargv[1], GENERIC_WRITE,
                    FILE_SHARE_READ, &sa, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    e = GetLastError();
    if (w == INVALID_HANDLE_VALUE) {
        dbgprintf(progname, "cannot create: %S", wargv[1]);
        return e;
    }
    if (e == ERROR_ALREADY_EXISTS) {
        DWORD wr = 0;
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (!SetFilePointerEx(w, ee, NULL, FILE_END)) {
            e = GetLastError();
            dbgprintf(progname, "cannot set file pointer for: %S", wargv[1]);
            CloseHandle(w);
            return e;
        }
        if (WriteFile(w, CRLFA, 2, &wr, NULL) && (wr != 0)) {
            dbgprintf(progname, "reusing: %S", wargv[1]);
        }
        else {
            e = GetLastError();
            dbgprintf(progname, "cannot write to: %S", wargv[1]);
            CloseHandle(w);
            return e;
        }
    }
    else {
        dbgprintf(progname, "created: %S", wargv[1]);
    }
#if SIMULATE_FAILURE
    CloseHandle(CreateThread(NULL, 0, failurethread, NULL, 0, &e));
#endif
    e = 0;
    while (e == 0) {
        DWORD rd = 0;
        DWORD wr = 0;

        if (ReadFile(r, b, RDBUFSIZE, &rd, NULL)) {
            if (rd == 0) {
                e = GetLastError();
                dbgprints(progname, "read 0 bytes");
            }
            else {
                if (WriteFile(w, b, rd, &wr, NULL)) {
                    if (wr == 0) {
                        e = GetLastError();
                        dbgprints(progname, "wrote 0 bytes");
                    }
                    else {
                        c += wr;
                        if (c > 16384) {
                            FlushFileBuffers(w);
                            c = 0;
                        }
                    }
                }
                else {
                    e = GetLastError();
                    dbgprints(progname, "WriteFile failed");
                }
            }
        }
        else {
            e = GetLastError();
        }
    }
    CloseHandle(w);
    if (e) {
        if ((e == ERROR_BROKEN_PIPE) || (e == ERROR_NO_DATA)) {
            dbgprints(progname, "iopipe closed");
            e = 0;
#if SIMULATE_FAILURE
            /* SvcBatch will kill this process after 5 seconds */
            Sleep(6000);
#endif
        }
        else
            dbgprintf(progname, "iopipe error: %lu", e);
    }
    else {
        dbgprints(progname, "done");
    }
    return e;
}
