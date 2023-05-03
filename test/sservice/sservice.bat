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
rem
echo %~nx0: Running inside %SVCBATCH_SERVICE_NAME% Service
echo %~nx0: Arguments [%*]
echo.
echo %~nx0: [%TIME%] ... Start > %SVCBATCH_SERVICE_NAME%.%SVCBATCH_SERVICE_UUID%.%~nx0.log
echo. >> %SVCBATCH_SERVICE_NAME%.%SVCBATCH_SERVICE_UUID%.%~nx0.log
set >> %SVCBATCH_SERVICE_NAME%.%SVCBATCH_SERVICE_UUID%.%~nx0.log
pushd %SVCBATCH_SERVICE_HOME%
rem
rem Sleep for one hour
rem Note that since started via 'start' the xsleep will
rem not receive CTRL+C signal. The svcbatch.exe will kill
rem that process on service stop.
rem
start xsleep.exe 3600
rem
rem Call batch file that calls xsleep.exe
rem This process will end in 10 seconds
start cmd /C ssxsleep.bat 10
rem
rem
rem Call this script again to test the
rem killprocess tree
rem
rem start cmd /C %~nx0 xsleep 1
rem cmd /C %~nx0 xsleep 2
rem cmd /C %~nx0 xsleep 3
rem cmd /C %~nx0 xsleep 4
rem
sservice.exe %*
rem
popd
rem
echo. >> %SVCBATCH_SERVICE_NAME%.%SVCBATCH_SERVICE_UUID%.%~nx0.log
echo %~nx0: [%TIME%] ... Done >> %SVCBATCH_SERVICE_NAME%.%SVCBATCH_SERVICE_UUID%.%~nx0.log
rem
exit /B 0
