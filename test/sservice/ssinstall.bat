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
svcbatch create sservice /F:LBR /C sservice.exe /P "300 some /c parameters " /P "\"and quoted one\"" "fake script" "script argument"
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
rem Presume that svcbatch.exe is in this directory
svcbatch create sservice /F:LB /w work
rem
goto End
rem
:doDelete
rem
svcbatch delete sservice
rem
:End
exit /B 0
