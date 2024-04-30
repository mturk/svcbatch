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

#if defined(_MSC_VER)
/**
 * Disable or reduce the frequency of...
 *   C4201:  nonstandard extension used
 */
# pragma warning(disable: 4201)
#endif

#include <windows.h>
#include <olectl.h>
#include <string.h>
#include <wchar.h>

#include "svcbatch.h"
#include "svcevent.h"

#define BBUFSIZ                512

static HANDLE    dllinstance    = NULL;
static LPCWSTR   eventsource    = SVCEVENT_SOURCE;
static WCHAR     zerostring[]   = {  0,  0,  0,  0 };

static __inline void xunxpathsep(LPWSTR str)
{
    for (; *str != WNUL; str++) {
        if (*str == L'\\')
            *str = L'/';
    }
}

static __inline void xpprefix(LPWSTR str)
{
    str[0] = L'\\';
    str[1] = L'\\';
    str[2] = L'?';
    str[3] = L'\\';
}

static __inline int xispprefix(LPCWSTR str)
{
    if (IS_EMPTY_WCS(str))
        return 0;
    if ((str[0] == L'\\') &&
        (str[1] == L'\\') &&
        (str[2] == L'?')  &&
        (str[3] == L'\\'))
        return 1;
    else
        return 0;
}

static __inline LPWSTR xnopprefix(LPCWSTR str)
{
    if (IS_EMPTY_WCS(str))
        return zerostring;
    if ((str[0] == L'\\') &&
        (str[1] == L'\\') &&
        (str[2] == L'?')  &&
        (str[3] == L'\\'))
        return (LPWSTR)(str + 4);
    else
        return (LPWSTR)str;
}

static __inline int xisalpha(int ch)
{
    if (((ch > 64) && (ch < 91)) || ((ch > 96) && (ch < 123)))
        return 1;
    else
        return 0;
}

static __inline int xwcslen(LPCWSTR s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    else
        return (int)wcslen(s);
}

static int xwcslcat(LPWSTR dst, int siz, int pos, LPCWSTR src)
{
    LPCWSTR s = src;
    LPWSTR  d = dst + pos;
    int     c = pos;
    int     n;

    ASSERT_NULL(dst, 0);
    ASSERT_WSTR(src, pos);

    n = siz - pos;
    if (n < 2)
        return siz;
    while ((n-- != 1) && (*s != WNUL)) {
        *d++ = *s++;
         c++;
    }
    *d = WNUL;
    if (*s != WNUL)
        c++;
    return c;
}

static DWORD xfixmaxpath(LPWSTR buf, DWORD len, int isdir)
{
    if (len > 5) {
        DWORD siz = isdir ? 248 : MAX_PATH;
        if (len < siz) {
            /**
             * Strip leading \\?\ for short paths
             * but not \\?\UNC\* paths
             */
            if (xispprefix(buf) && (buf[5] == L':')) {
                wmemmove(buf, buf + 4, len - 3);
                len -= 4;
            }
        }
        else {
            /**
             * Prepend \\?\ to long paths
             * if starts with X:\
             */
            if (xisalpha(buf[0])  &&
                (buf[1] == L':' ) &&
                (buf[2] == L'\\')) {
                wmemmove(buf + 4, buf, len + 1);
                xpprefix(buf);
                len += 4;
            }
        }
    }
    return len;
}

static DWORD xgetfilepath(LPCWSTR path, LPWSTR dst, DWORD siz)
{
    HANDLE fh;
    DWORD  len;

    fh = CreateFileW(path, GENERIC_READ,
                     FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (IS_INVALID_HANDLE(fh))
        return 0;
    len = GetFinalPathNameByHandleW(fh, dst, siz, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= siz)) {
        SetLastError(ERROR_BAD_PATHNAME);
        return 0;
    }
    return xfixmaxpath(dst, len, 0);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        dllinstance = instance;
    return TRUE;
}

STDAPI DllRegisterServer(void)
{
    WCHAR  bb[SVCBATCH_PATH_MAX];
    WCHAR  nb[BBUFSIZ];
    DWORD  nn;
    DWORD  cc;
    HKEY   hk;
    int    i;

    nn = GetModuleFileNameW(dllinstance, bb, SVCBATCH_PATH_SIZ);
    if (nn == 0)
        return SELFREG_E_TYPELIB;
    if (nn >= SVCBATCH_PATH_SIZ)
        return SELFREG_E_TYPELIB;
    nn = xgetfilepath(bb, bb, SVCBATCH_PATH_SIZ);
    if (nn < 9)
        return SELFREG_E_TYPELIB;
    bb[nn++] = WNUL;

    i = xwcslcat(nb, BBUFSIZ, 0, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\");
    i = xwcslcat(nb, BBUFSIZ, i, eventsource);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        nb, 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_READ | KEY_WRITE,
                        NULL, &hk, &cc) != ERROR_SUCCESS)
        return SELFREG_E_TYPELIB;
    if (cc == REG_CREATED_NEW_KEY) {
        DWORD dw = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
                   EVENTLOG_INFORMATION_TYPE;
        if (RegSetValueExW(hk, L"EventMessageFile", 0, REG_EXPAND_SZ,
                          (const BYTE *)bb, nn  * 2) != ERROR_SUCCESS)
            return SELFREG_E_TYPELIB;
        if (RegSetValueExW(hk, L"TypesSupported", 0, REG_DWORD,
                          (const BYTE *)&dw, 4) != ERROR_SUCCESS)
            return SELFREG_E_TYPELIB;
    }
    else {
        RegCloseKey(hk);
        return SELFREG_E_CLASS;
    }
    RegCloseKey(hk);
    return S_OK;
}

STDAPI DllUnregisterServer(void)
{
    WCHAR  nb[BBUFSIZ];
    int    i;

    i = xwcslcat(nb, BBUFSIZ, 0, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\");
    i = xwcslcat(nb, BBUFSIZ, i, eventsource);

    if (RegDeleteKeyExW(HKEY_LOCAL_MACHINE, nb, KEY_WOW64_64KEY, 0))
        return SELFREG_E_TYPELIB;
    return S_OK;
}

HRESULT DllInstall(BOOL bInstall, LPCWSTR pszCmdLine)
{
    if (IS_VALID_WCS(pszCmdLine))
        eventsource = pszCmdLine;
    if (bInstall)
        return DllRegisterServer();
    else
        return S_OK;
}
