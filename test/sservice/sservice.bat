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
rem Batch script for running sservice.exe
rem
rem
setlocal
rem
if /i "x%~1" == "xcreate" goto doCreate
rem
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
echo %~nx0: Arguments [%*]
echo.
rem
sservice.exe %*
rem
goto End
rem
:doCreate
rem
rem Presume that svcbatch.exe is in this directory
sc create sservice binPath= "%cd%\svcbatch.exe -vb -sNUL sservice.bat run"
rem
:End
exit /B 0
