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
rem
if /i "x%~1" == "xdelete" goto doDelete
rem
rem goto doStressTest
rem
rem
svcbatch create sservice /F:L0 /C:sservice.exe /C:[ "200 some parameters" ]
rem
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
svcbatch create sservice /F:L -w work
rem
goto End
rem
:doDelete
rem
svcbatch delete sservice
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%

rem
:End
exit /B 0
