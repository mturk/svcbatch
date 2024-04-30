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
rem Batch script for installing sservice
rem
rem
setlocal
rem Set active code page to 65001 (utf-8)
chcp 65001>NUL
rem
pushd "..\..\build\rel"
rem set "BUILD_DIR=%cd%"
set "SERVICE_EXEC=svcbatch.exe"
set "SERVICE_NAME=sservice"
set "DISPLAY_NAME=Simple Service"
rem
if /i "x%~1" == "xdelete" goto doDelete
if /i "x%~1" == "xstart"  goto doStart
if /i "x%~1" == "xstop"   goto doStop
rem
rem goto doStressTest
rem
rem
%SERVICE_EXEC% create "%SERVICE_NAME%" ^
            --displayname "%DISPLAY_NAME%" ^
            --set Export * ^
            --set FailMode 0 ^
            --set Command [ sservice.exe 120 some options ]  ^
            --set Arguments [ "the arguments" ]

rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
echo Created %SERVICE_NAME%
goto End
rem
:doStressTest
rem
rem Create work directory
rem
mkdir work 2>NUL
copy /Y sservice.bat work\ > nul
copy /Y ssxsleep.bat work\ > nul
copy /Y sservice.exe work\ > nul
copy /Y xsleep.exe work\ > nul
rem
rem
%SERVICE_EXEC% create "%SERVICE_NAME%" --set UseLocalTime Yes /W:work
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
echo Created %SERVICE_NAME%
goto End
rem
rem
:doStart
%SERVICE_EXEC% start "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo Started %SERVICE_NAME%
goto End
rem
rem
:doStop
%SERVICE_EXEC% stop "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo Stopped %SERVICE_NAME%
goto End
rem
rem
:doDelete
rem
%SERVICE_EXEC% delete "%SERVICE_NAME%"
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
echo Deleted %SERVICE_NAME%
rem
rem
:End
popd
exit /B 0
