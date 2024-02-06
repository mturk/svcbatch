@echo off
rem Licensed to the Apache Software Foundation (ASF) under one or more
rem contributor license agreements.  See the NOTICE file distributed with
rem this work for additional information regarding copyright ownership.
rem The ASF licenses this file to You under the Apache License, Version 2.0
rem (the "License"); you may not use this file except in compliance with
rem the License.  You may obtain a copy of the License at
rem
rem     http://www.apache.org/licenses/LICENSE-2.0
rem
rem Unless required by applicable law or agreed to in writing, software
rem distributed under the License is distributed on an "AS IS" BASIS,
rem WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
rem See the License for the specific language governing permissions and
rem limitations under the License.
rem
rem --------------------------------------------------
rem Dummy SvcBatch service
rem
rem
setlocal
rem
rem
rem goto xsName
rem
set "_SERVICE_NAME=%SVCBATCH_NAME%"
set "_SERVICE_UUID=%SVCBATCH_UUID%"
set "_SERVICE_LOGS=%SVCBATCH_LOGS%"
goto doService
rem
:xsName
rem
set "_SERVICE_NAME=%SERVICE_NAME%"
set "_SERVICE_UUID=%SERVICE_UUID%"
set "_SERVICE_LOGS=%SERVICE_LOGS%"
rem
rem
:doService
rem
if "x%_SERVICE_NAME%" == "x" goto noService
rem
if /i "x%~1" == "xstop"     goto doShutdown
if /i "x%~1" == "xshutdown" goto doShutdown
rem
echo %~nx0: Running %_SERVICE_NAME% Service
echo %~nx0: Arguments [%*]
echo.
echo %~nx0: %CD%
echo.
echo %~nx0: System Information
echo.
rem Display machine specific properties and configuration
systeminfo
chcp
echo.
echo %~nx0: Environment Variables
echo.
rem Display environment variables
set
echo.
echo.
echo %~nx0: [%TIME%] ... started >&2
echo. >&2
rem
:doMain
rem
set /A _qc=0
rem
:doRun
rem Set running counter
set /A _rc=0
rem
:doWork
rem Set working counter
set /A _wc=0
rem
if %_qc% lss 15 (
    echo %~nx0: [%_rc%] [%TIME%] ... running
)
rem
:doLoop
rem
if %_qc% lss 15 (
    echo %~nx0: [%_wc%] [%TIME%] ... working %RANDOM%
)
rem
rem Simulate work by sleeping for 2 seconds
rem ping -n 3 127.0.0.1 >NUL
xsleep.exe 2
rem
rem Check if shutdown batch signaled to stop the service
if exist "%_SERVICE_LOGS%\ss-%_SERVICE_UUID%" (
    goto doCleanup
)
rem Increment counter
set /A _wc+=1
if %_wc% lss 10 (
    goto doLoop
)
rem
set /A _qc+=1
if %_qc% gtr 50 (
    echo %~nx0: [%_rc%] [%TIME%] ... leaving quiet mode >&2
    echo. >&2
    goto doMain
)
rem
if %_qc% gtr 15 (
    goto doRun
)
rem
rem Dump some lorem ipsum
echo.
echo %~nx0: [%_rc%] [%TIME%] ... dumping
echo.
echo Lorem ipsum dolor sit amet, consectetur adipiscing elit,
echo sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.
echo Ut enim ad minim veniam, quis nostrud exercitation ullamco
echo laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure
echo dolor in reprehenderit in voluptate velit esse cillum dolore eu
echo fugiat nulla pariatur. Excepteur sint occaecat cupidatat non
echo proident, sunt in culpa qui officia deserunt mollit anim id est laborum.
echo.
rem
if %_qc% geq 15 (
    echo. >&2
    echo %~nx0: [%_rc%] [%TIME%] ... entering quiet mode >&2
)
rem Increment counter
set /A _rc+=1
rem
if %_rc% lss 10 (
    goto doWork
)
rem
rem Send shutdown signal
rem sc stop %_SERVICE_NAME%
goto doRun
rem Comment above goto to simulate failure
echo %~nx0: [%TIME%] Simulating failure
rem ping -n 6 127.0.0.1 >NUL
xsleep.exe 5
rem SvcBatch will report error if we end without
rem explicit call to sc stop [service name]
goto End
rem
:doCleanup
rem
del /F /Q "%_SERVICE_LOGS%\ss-%_SERVICE_UUID%" 2>NUL
if %_qc% geq 15 (
    xsleep.exe 2
    goto End
)
echo.
echo %~nx0: [%TIME%] Found ss-%_SERVICE_UUID%
echo %~nx0: [%TIME%] Simulating cleanup
rem ping -n 3 127.0.0.1 >NUL
xsleep.exe 3
echo.
echo.
echo %~nx0: [%TIME%] Service done
rem
goto End
rem
rem
:doShutdown
rem
echo %~nx0: Called from %_SERVICE_NAME% Service
echo %~nx0: Arguments [%*]
echo.
echo %~nx0: System Information
echo.
rem Display machine specific properties and configuration
systeminfo
chcp
echo.
echo %~nx0: Environment Variables
echo.
rem Display environment variables
set
echo.
echo.
rem
rem
echo %~nx0: [%TIME%] Shutdown running
rem Simulate some work by sleeping for 2 seconds
rem ping -n 3 127.0.0.1 >NUL
xsleep.exe 2
rem Simple IPC mechanism to signal the service
rem to stop by creating unique file
echo.
echo %~nx0: [%TIME%] Creating ss-%_SERVICE_UUID%
echo.
echo Y> "%_SERVICE_LOGS%\ss-%_SERVICE_UUID%"
rem
:doShutdownWork
rem
echo %~nx0: [%TIME%] Shutdown working
rem ping -n 6 127.0.0.1 >NUL
xsleep.exe 5
rem echo %~nx0: [%TIME%] ... running
rem goto doShutdownWork
echo.
echo %~nx0: [%TIME%] Shutdown done
rem
goto End
rem
rem
:noService
echo %~nx0: _SERVICE_NAME is not defined
exit /B 1
rem
rem
:End
exit /B 0
