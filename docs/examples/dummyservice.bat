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
if /i "x%~1" == "xcreate" goto doCreate
if /i "x%~1" == "xdelete" goto doDelete
if /i "x%~1" == "xremove" goto doRemove
if /i "x%~1" == "xstart"  goto doStart
if /i "x%~1" == "xstop"   goto doStop
if /i "x%~1" == "xruncmd" goto doRunInteractive
if /i "x%~1" == "xrunme"  goto doRunMe
if /i "x%~1" == "xrotate" goto doRotate
rem
set "SERVICE_NAME="
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
echo.
rem Dump environment variables to log file
set
echo.

:doRepeat
rem
echo [%TIME%] ... running
rem Simulate work by sleeping for 5 seconds
ping -n 6 localhost >NUL
rem Uncomment to write more data to SvcBatch.log
rem echo.
rem set
rem echo.
rem
rem Send shutdown signal
rem sc stop %SVCBATCH_SERVICE_NAME%
goto doRepeat
rem Comment above goto to simulate failure
echo Simulating failure
ping -n 6 localhost >NUL
rem SvcBatch will report error if we end without
rem explicit call to sc stop [service name]
goto End

:doCreate
rem
rem
rem Presuming this is the build tree ...
rem Create a service command line
set "SERVICE_CMDLINE=\"%cd%\..\..\x64\svcbatch.exe\" -w \"%cd%\" -r @30~100K %~nx0"
rem
sc create "%SERVICE_NAME%" binPath= "%SERVICE_CMDLINE%"
sc config "%SERVICE_NAME%" DisplayName= "A Dummy Service"
sc config "%SERVICE_NAME%" depend= Tcpip/Afd start= demand
sc description "%SERVICE_NAME%" "One dummy SvcBatch service example"
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
goto End

:doStart
rem
rem
sc start "%SERVICE_NAME%"
goto End

:doStop
rem
rem
sc stop "%SERVICE_NAME%"
goto End

:doRotate
rem
rem
sc control "%SERVICE_NAME%" 234
goto End

:doDelete
rem
rem
sc stop "%SERVICE_NAME%" >NUL
sc delete "%SERVICE_NAME%"
echo Deleted %SERVICE_NAME%
goto End

:doRemove
rem
rem
sc stop "%SERVICE_NAME%" >NUL 2>&1
sc delete "%SERVICE_NAME%" >NUL 2>&1
rd /S /Q Logs >NUL 2>&1
del /Q %~nx0.Y >NUL 2>&1
echo Removed %SERVICE_NAME%
goto end

:doRunInteractive
rem Suppress Terminate batch job on CTRL+C
rem
echo Y > %~nx0.Y
call %~nx0 runme < %~nx0.Y
rem Use provided errorlevel
exit /B %ERRORLEVEL%
rem
:doRunMe
rem
rem
rem Presuming this is the build tree ...
rem Run batch file
if not exist %~nx0.Y (
echo Cannot find %~nx0,Y answer file
exit /B 1
)
..\..\x64\svcbatch.exe -i -w "%cd%" -r @30~100K %~nx0

:End
