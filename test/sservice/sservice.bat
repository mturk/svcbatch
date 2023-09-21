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
goto doStressTest
rem
echo %~nx0: Running inside %SVCBATCH_NAME%
echo.
echo %~nx0: [%TIME%] ... Start
echo.
rem
sservice.exe %*
rem
echo.
echo %~nx0: [%TIME%] ... Done
rem
exit /B 0
rem
:doStressTest
rem
set "_SS_LOG=%SVCBATCH_NAME%.%~nx0.log"
rem
echo %~nx0: Running inside %SVCBATCH_NAME% > %_SS_LOG%
echo %~nx0: Arguments [%*] >> %_SS_LOG%
echo.
echo %~nx0: [%TIME%] ... Start >> %_SS_LOG%
echo. >> %_SS_LOG%
set   >> %_SS_LOG%
echo. >> %_SS_LOG%
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
call :callBack 1.
call :callBack 2.
rem
echo. >> %_SS_LOG%
echo %~nx0: [%TIME%] ... Starting sservice.exe >> %_SS_LOG%
rem
sservice.exe %*
rem
goto End
rem
:callBack
rem
start cmd /C ssxsleep.bat 20 %~1
rem
exit /B 0
rem
:End
echo. >> %_SS_LOG%
echo %~nx0: [%TIME%] ... Done >> %_SS_LOG%
rem
exit /B 0
