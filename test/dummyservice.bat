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
if "x%SVCBATCH_SERVICE_NAME%" == "x" goto noService
rem
if /i "x%~1" == "xshutdown" goto doShutdown
rem
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
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
:runService
rem
echo %~nx0: [%TIME%] ... running %RANDOM%
rem Simulate work by sleeping for 2 seconds
ping -n 3 127.0.0.1 >NUL
rem Uncomment to write more data to SvcBatch.log
rem echo.
rem set
rem echo.
rem
rem Check if shutdown batch signaled to stop the service
if exist "%SVCBATCH_SERVICE_LOGS%\shutdown-%SVCBATCH_SERVICE_UUID%" (
    echo.
    echo %~nx0: [%TIME%] found shutdown-%SVCBATCH_SERVICE_UUID%
    goto doCleanup
)
rem
rem Send shutdown signal
rem sc stop %SVCBATCH_SERVICE_NAME%
goto runService
rem Comment above goto to simulate failure
echo %~nx0: Simulating failure
ping -n 6 127.0.0.1 >NUL
rem SvcBatch will report error if we end without
rem explicit call to sc stop [service name]
goto End
rem
:doCleanup
rem
echo.
echo %~nx0: [%TIME%] Simulating cleanup
ping -n 3 127.0.0.1 >NUL
echo.
echo.
echo %~nx0: [%TIME%] Service done
del /F /Q "%SVCBATCH_SERVICE_LOGS%\shutdown-%SVCBATCH_SERVICE_UUID%" 2>NUL
rem
goto End
rem
rem
:doShutdown
rem
echo %~nx0: Called from %SVCBATCH_SERVICE_NAME% Service
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
ping -n 3 127.0.0.1 >NUL
rem Simple IPC mechanism to signal the service
rem to stop by creating unique file
echo.
echo %~nx0: [%TIME%] Creating shutdown-%SVCBATCH_SERVICE_UUID%
echo.
echo Y> "%SVCBATCH_SERVICE_LOGS%\shutdown-%SVCBATCH_SERVICE_UUID%"
rem
:runShutdown
ping -n 6 127.0.0.1 >NUL
rem echo %~nx0: [%TIME%] ... running
rem goto runShutdown
echo.
echo %~nx0: [%TIME%] Shutdown done
rem
goto End
rem
rem
:noService
echo %~nx0: SVCBATCH_SERVICE_NAME is not defined
exit /B 1
rem
rem
:End
exit /B 0
