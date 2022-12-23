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
set "SERVICE_NAME=adummysvc"
rem
if /i "x%~1" == "xcreate"   goto doCreate
if /i "x%~1" == "xdelete"   goto doDelete
if /i "x%~1" == "xremove"   goto doRemove
if /i "x%~1" == "xstart"    goto doStart
if /i "x%~1" == "xstop"     goto doStop
if /i "x%~1" == "xbreak"    goto doBreak
if /i "x%~1" == "xrotate"   goto doRotate
if /i "x%~1" == "xshutdown" goto doShutdown
rem
set "SERVICE_NAME="
if "x%SVCBATCH_SERVICE_NAME%" == "x" goto noService
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
echo %~nx0: Arguments [%*]
echo.
echo.
rem Dump environment variables to log file
set
echo.
echo.
rem
:doRepeat
rem
echo %~nx0: [%TIME%] ... running
rem Simulate work by sleeping for 5 seconds
ping -n 6 127.0.0.1 >NUL
rem Uncomment to write more data to SvcBatch.log
rem echo.
rem set
rem echo.
rem
rem Send shutdown signal
rem sc stop %SVCBATCH_SERVICE_NAME%
goto doRepeat
rem Comment above goto to simulate failure
echo %~nx0: Simulating failure
ping -n 6 127.0.0.1 >NUL
rem SvcBatch will report error if we end without
rem explicit call to sc stop [service name]
goto End
rem
rem
:doShutdown
rem
set "SERVICE_NAME="
if "x%SVCBATCH_SERVICE_NAME%" == "x" goto noService
echo %~nx0: Called from %SVCBATCH_SERVICE_NAME% Service
echo %~nx0: Arguments [%*]
echo.
echo.
rem Dump environment variables to SvcBatch.shutdown.log file
set
echo.
echo.
rem
rem
echo %~nx0: [%TIME%] Shutdown running
rem Simulate some work by sleeping for 10 seconds
ping -n 6 127.0.0.1 >NUL
ping -n 6 127.0.0.1 >NUL
echo %~nx0: [%TIME%] Shutdown done
rem
goto End
rem
rem
:doCreate
rem
pushd %~dp0
set "_TESTS_DIR=%cd%"
popd
if not exist "%_TESTS_DIR%\..\x64" (
    echo.
    echo Cannot find build directory.
    echo Run [n]make tests
    exit /B 1
)
pushd ..\x64
set "_BUILD_DIR=%cd%"
popd
set "SERVICE_LOG_REDIR="
set "SERVICE_LOG_FNAME="
set "SHUTDOWN_ARGS="
set "ROTATE_RULE="
set "SERVICE_BATCH=%~nx0"
rem
rem Uncomment to use separate shutdown file
rem set "SERVICE_SHUTDOWN=-s dummyshutdown.bat"
rem Set arguments for shutdown bat file
set "SHUTDOWN_ARGS=-a shutdown /Aargument /a\"argument with spaces\""
rem
rem
set "SERVICE_LOG_DIR=-o \"Logs/%SERVICE_NAME%\""
rem Rotate Log files each 30 minutes or when larger then 100Kbytes
set "ROTATE_RULE=/R 30 -r100K"
rem Rotate Log files at midnight
rem set "ROTATE_RULE=-r0"
rem Rotate Log files every full hour or when larger then 40000 bytes
rem set "ROTATE_RULE=-r60 -r 40000B"
rem Uncomment to disable log rotation
rem set "ROTATE_RULE=-m 0"
rem
rem Write log to external program instead to log file
rem set "SERVICE_LOG_REDIR=-e \"pipedlog.exe @@logfile@@ some \\\"dummy arguments\\\"\""
rem
rem Use Apache Httpd rotatelogs utility for logging
rem set "SERVICE_LOG_REDIR=-e \"rotatelogs.exe -l @@logfile@@ 120\""
rem
rem Set log file name intead defaut SvcBatch.log
set "SERVICE_LOG_FNAME=-n \"%SERVICE_NAME%.log\""
rem
rem set "SERVICE_LOG_FNAME=-n %SERVICE_NAME%.@Y-@m-@d.@H@M@S.log"
rem
rem Presuming this is the build tree ...
rem Create a service command line
rem
set "SERVICE_CMDLINE=\"%_BUILD_DIR%\svcbatch.exe\" -pvDbL /w \"%_TESTS_DIR%\" %SERVICE_LOG_DIR% %SERVICE_LOG_REDIR% %SERVICE_LOG_FNAME% %ROTATE_RULE% %SERVICE_SHUTDOWN% %SHUTDOWN_ARGS% %SERVICE_BATCH% run test"
rem
sc create "%SERVICE_NAME%" binPath= "%SERVICE_CMDLINE%"
sc config "%SERVICE_NAME%" DisplayName= "A Dummy Service"
sc config "%SERVICE_NAME%" depend= Tcpip/Afd start= demand
sc description "%SERVICE_NAME%" "One dummy SvcBatch service example"
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
goto End
rem
:doStart
rem
rem
sc start "%SERVICE_NAME%"
goto End
rem
:doStop
rem
rem
sc stop "%SERVICE_NAME%"
goto End
rem
:doBreak
rem
rem
sc control "%SERVICE_NAME%" 233
goto End
rem
:doRotate
rem
rem
sc control "%SERVICE_NAME%" 234
goto End
rem
:doDelete
rem
rem
sc stop "%SERVICE_NAME%" >NUL
sc delete "%SERVICE_NAME%"
echo %~nx0: Deleted %SERVICE_NAME%
goto End
rem
:doRemove
rem
rem
sc stop "%SERVICE_NAME%" >NUL 2>&1
sc delete "%SERVICE_NAME%" >NUL 2>&1
rd /S /Q "Logs" >NUL 2>&1
echo %~nx0: Removed %SERVICE_NAME%
goto End
rem
:noService
echo %~nx0: SVCBATCH_SERVICE_NAME not defined
exit /B 1
rem
rem
:End
exit /B 0
