@echo off
rem
rem Licensed under the Apache License, Version 2.0 (the "License");
rem you may not use this file except in compliance with the License.
rem You may obtain a copy of the License at
rem
rem     http://www.apache.org/licenses/LICENSE-2.0
rem
rem Unless required by applicable law or agreed to in writing, software
rem distributed under the License is distributed on an "AS IS" BASIS,
rem WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
rem See the License for the specific language governing permissions and
rem limitations under the License.
rem
rem Batch script for building sservice.c file
rem
rem
setlocal
set "PROJECT=sservice"
rem Setup
set "WINVER=0x0601"
set "MACHINE=X64"
set "CC=cl.exe"
set "LN=link.exe"
set "LDLIBS=kernel32.lib"
set "LFLAGS=/nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:%MACHINE%"
set "CFLAGS=-c -nologo -W4 -O2 -Ob2 -MD"
set "CFLAGS=%CFLAGS% -DWIN32 -DWIN64 -DUNICODE -D_UNICODE"
set "CFLAGS=%CFLAGS% -D_WIN32_WINNT=%WINVER% -DWINVER=%WINVER% -D_CRT_SECURE_NO_DEPRECATE"
rem
rem Build
echo.
echo %CC% %CFLAGS% -Fo%PROJECT%.obj %PROJECT%.c
%CC% %CFLAGS% -Fo%PROJECT%.obj %PROJECT%.c
echo.
echo %LN% %LFLAGS% /out:%PROJECT%.exe %PROJECT%.obj %LDLIBS%
%LN% %LFLAGS% /out:%PROJECT%.exe %PROJECT%.obj %LDLIBS%
echo.
