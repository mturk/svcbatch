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

static int ctrlcc = 0;

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    DWORD pid = GetCurrentProcessId();

    switch (ctrl) {
        case CTRL_CLOSE_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_CLOSE_EVENT signaled\n\n", pid);
            ctrlcc++;
        break;
        case CTRL_SHUTDOWN_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_SHUTDOWN_EVENT signaled\n\n", pid);
            ctrlcc++;
        break;
        case CTRL_C_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_C_EVENT signaled\n\n", pid);
            ctrlcc++;
        break;
        case CTRL_BREAK_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_BREAK_EVENT signaled\n\n", pid);
        break;
        case CTRL_LOGOFF_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_LOGOFF_EVENT signaled\n\n", pid);
            ctrlcc++;
        break;
        default:
            fprintf(stdout, "\n\n[%.4lu] Unknown control '%lu' signaled\n\n", pid, ctrl);
            ctrlcc++;
        break;
    }
    return TRUE;
}

int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
{
    int i;
    int e = 0;
    int r = 0;

    DWORD pid = GetCurrentProcessId();
    fwprintf(stdout, L"\n[%.4lu] Program '%s' started\n", pid, wargv[0]);
    if (argc > 1) {
        fwprintf(stdout, L"\n[%.4lu] Arguments\n\n", pid);
        for (i = 1; i < argc; i++) {
            fwprintf(stdout, L"[%.4lu] [%.2d] %s\n", pid, i, wargv[i]);
        }
    }
    fwprintf(stdout, L"\n[%.4lu] Environment\n\n", pid);
    while (wenv[e] != NULL) {
        fwprintf(stdout, L"[%.4lu] [%.2d] %s\n", pid, e + 1, wenv[e]);
        e++;
    }
    SetConsoleCtrlHandler(consolehandler, TRUE);
    fwprintf(stdout, L"\n\n[%.4lu] Program running\n", pid);
    i = 1;
    for(;;) {
        Sleep(1000);
        fwprintf(stdout, L"[%.4lu] [%.4d] ... running\n", pid, i++);
        if (i > 3600) {
            fwprintf(stderr, L"\n\n[%.4lu] Timeout reached\n", pid);
            r = ERROR_PROCESS_ABORTED;
            break;
        }
        if (ctrlcc) {
            fwprintf(stdout, L"\n\n[%.4lu] Stop signaled\n", pid);
            Sleep(1000);
            break;
        }
    }
    fwprintf(stdout, L"\n\n[%.4lu] Program done\n", pid);
    return r;
}
