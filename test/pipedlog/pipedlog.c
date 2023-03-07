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
#define RDBUFSIZE 8192

/**
 * Set to 1 to simulate failure
 */
#define SIMULATE_FAILURE 0

static char CRLFA[2] = { '\r', '\n'};

#if SIMULATE_FAILURE
DWORD WINAPI failurethread(void *unused)
{
    fputs("simulating failure\n", stdout);
    Sleep(10000);
    fputs("calling ExitProcess(ERROR_WRITE_FAULT)\n", stdout);
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

    r = GetStdHandle(STD_INPUT_HANDLE);
    if (r == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        fprintf(stderr, "Missing stdin handle %lu", e);
        return e;
    }
    setvbuf(stdout, (char*)NULL, _IONBF, 0);
    if (argc < 2) {
        fputs("Missing logfile argument\n", stderr);
        return ERROR_INVALID_PARAMETER;
    }
    fputs("started\n", stdout);
    if (argc > 2) {
        fputs("extra arguments [[\n", stdout);
        for (i = 2; i < argc; i++) {
            fprintf(stdout, "  %S\n", wargv[i]);
        }
        fputs("]]\n", stdout);
    }
    memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
    sa.nLength = (DWORD)sizeof(SECURITY_ATTRIBUTES);

    w = CreateFileW(wargv[1], GENERIC_WRITE,
                    FILE_SHARE_READ, &sa, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    e = GetLastError();
    if (w == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot create: %S\n", wargv[1]);
        return e;
    }
    if (e == ERROR_ALREADY_EXISTS) {
        DWORD wr = 0;
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (!SetFilePointerEx(w, ee, NULL, FILE_END)) {
            e = GetLastError();
            fprintf(stderr, "cannot set file pointer for: %S\n", wargv[1]);
            CloseHandle(w);
            return e;
        }
        if (WriteFile(w, CRLFA, 2, &wr, NULL) && (wr != 0)) {
            fprintf(stdout, "reusing: %S\n", wargv[1]);
        }
        else {
            e = GetLastError();
            fprintf(stderr, "cannot write to: %S\n", wargv[1]);
            CloseHandle(w);
            return e;
        }
    }
    else {
        fprintf(stdout, "created: %S\n", wargv[1]);
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
            }
            else {
                if (WriteFile(w, b, rd, &wr, NULL)) {
                    if (wr == 0) {
                        e = GetLastError();
                    }
                    else {
                        c += wr;
                        if (c > 16384) {
                            fputs("flushing...\n", stdout);
                            FlushFileBuffers(w);
                            c = 0;
                        }
                    }
                }
                else {
                    e = GetLastError();
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
            fputs("iopipe closed\n", stdout);
            e = 0;
#if SIMULATE_FAILURE
            /* SvcBatch will kill this process after 5 seconds */
            Sleep(6000);
#endif
        }
        else
            fprintf(stderr, "iopipe error: %lu\n", e);
    }
    else {
        fputs("done\n", stdout);
    }
    return e;
}
