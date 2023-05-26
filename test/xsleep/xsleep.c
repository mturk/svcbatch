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

/**
 * Simple program that sleeps
 * from 1 to 86400 seconds (one day)
 *
 * Usage: xsleep.exe [seconds]
 */
int wmain(int argc, const wchar_t **wargv)
{
    int secs = 0;

    if (argc > 1) {
        secs = _wtoi(wargv[1]);
        if (secs > 86400)
            secs = 86400;
    }
    if (secs < 1)
        secs = 1;
    if (secs > 1800) {
        /**
         * Enable CTRL+C
         */
        SetConsoleCtrlHandler(NULL, FALSE);
    }
    /**
     * Do an actual sleep
     */
    Sleep(secs * 1000);
    return 0;
}
