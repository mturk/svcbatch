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

#define RDBUFSIZE 8192
static char CRLFA[2] = { '\r', '\n'};
int wmain(int argc, const wchar_t **wargv)
{
    int i;
    DWORD    e = 0;
    HANDLE   w, r;
    BYTE     b[RDBUFSIZE];
    SECURITY_ATTRIBUTES sa;

    r = GetStdHandle(STD_INPUT_HANDLE);
    if (r == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        OutputDebugStringA("GetStdHandle failed");
        return e;
    }
    OutputDebugStringW(wargv[0]);
    if (argc < 2) {
        OutputDebugStringA("Missing logfile argument");
        return ERROR_INVALID_PARAMETER;
    }
    if (argc > 2) {
        OutputDebugStringA(">>> extra arguments");
        for (i = 2; i < argc; i++) {
            OutputDebugStringW(wargv[i]);
        }
        OutputDebugStringA("<<<");
    }

    memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
    sa.nLength = (DWORD)sizeof(SECURITY_ATTRIBUTES);

    w = CreateFileW(wargv[1], GENERIC_WRITE,
                    FILE_SHARE_READ, &sa, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (w == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        OutputDebugStringA("CreateFileW failed");
        return e;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD wr = 0;
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (!SetFilePointerEx(w, ee, NULL, FILE_END)) {
            e = GetLastError();
            OutputDebugStringA("SetFilePointerEx failed");
            return e;
        }
        if (WriteFile(w, CRLFA, 2, &wr, NULL) && (wr != 0)) {
            OutputDebugStringA("reusing");
        }
        else {
            e = GetLastError();
            OutputDebugStringA("cannot write to existing file");
            return e;
        }
    }
    else {
        OutputDebugStringA("created");
    }
    OutputDebugStringW(wargv[1]);

    while (e == 0) {
        DWORD rd = 0;
        DWORD wr = 0;

        if (ReadFile(r, b, RDBUFSIZE, &rd, NULL)) {
            if (rd == 0) {
                e = GetLastError();
                OutputDebugStringA("read 0 bytes");
            }
            else {
                if (WriteFile(w, b, rd, &wr, NULL)) {
                    if (wr == 0) {
                        e = GetLastError();
                        OutputDebugStringA("wrote 0 bytes");
                    }
                }
                else {
                    e = GetLastError();
                    OutputDebugStringA("WriteFile failed");
                }
            }
        }
        else {
            e = GetLastError();
        }
    }
    CloseHandle(w);
    if (e) {
        if ((e == ERROR_BROKEN_PIPE) || (e == ERROR_NO_DATA))
            OutputDebugStringA("iopipe closed");
        else
            OutputDebugStringA("iopipe error");
    }
    else {
        OutputDebugStringA("done");
    }
    return e;
}
