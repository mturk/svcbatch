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
#include <io.h>
#include <fcntl.h>

static HANDLE stopsig = NULL;

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    DWORD pid = GetCurrentProcessId();

    switch (ctrl) {
        case CTRL_CLOSE_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_CLOSE_EVENT signaled\n\n", pid);
            SetEvent(stopsig);
        break;
        case CTRL_SHUTDOWN_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_SHUTDOWN_EVENT signaled\n\n", pid);
            SetEvent(stopsig);
        break;
        case CTRL_C_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_C_EVENT signaled\n\n", pid);
            SetEvent(stopsig);
        break;
        case CTRL_BREAK_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_BREAK_EVENT signaled\n\n", pid);
            SetEvent(stopsig);
        break;
        case CTRL_LOGOFF_EVENT:
            fprintf(stdout, "\n\n[%.4lu] CTRL_LOGOFF_EVENT signaled\n\n", pid);
            SetEvent(stopsig);
        break;
        default:
            fprintf(stdout, "\n\n[%.4lu] Unknown control '%lu' signaled\n\n", pid, ctrl);
        break;
    }
    return TRUE;
}

int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
{
    int i;
    int e     = 0;
    int r     = 0;
    int secs  = 300;
    DWORD  id;


    _setmode(_fileno(stdout),_O_BINARY);
    setvbuf(stdout, (char*)NULL, _IONBF, 0);
    id = GetCurrentProcessId();

    stopsig = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stopsig == NULL) {
        r = GetLastError();
        fprintf(stderr, "\n\n[%.4lu] CreateEvent failed\n", id);
        return r;
    }
    fprintf(stdout, "\n[%.4lu] Program '%S' started\n", id, wargv[0]);
    if (argc > 1) {
        secs = _wtoi(wargv[1]);
        if (secs > 1800)
            secs = 1800;
        fprintf(stdout, "\nArguments\n\n");
        for (i = 1; i < argc; i++) {
            fprintf(stdout, "[%.2d] %S\n", i, wargv[i]);
        }
    }
    if (secs < 60)
        secs = 60;
    fprintf(stdout, "\nEnvironment\n\n");
    while (wenv[e] != NULL) {
        fprintf(stdout, "[%.2d] %S\n", e + 1, wenv[e]);
        e++;
    }
    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consolehandler, TRUE);

    fprintf(stdout, "\n\n[%.4lu] Program running for %d seconds\n\n", id, secs);
    i = 1;
    for(;;) {
        DWORD ws = WaitForSingleObject(stopsig, 1000);

        if (ws == WAIT_OBJECT_0) {
            fprintf(stdout, "\n\n[%.4lu] Stop signaled\n", id);
            fflush(stdout);
            Sleep(1000);
            break;
        }
        fprintf(stdout, "[%.4d] ... running\n", i);
        i++;
        if (i > secs) {
            fprintf(stderr, "\n\n[%.4d] Timeout reached\n", id);
            r = ERROR_PROCESS_ABORTED;
            break;
        }
    }
    fprintf(stdout, "\n\n[%.4lu] Program done\n", id);
    _flushall();
    CloseHandle(stopsig);
    return r;
}
