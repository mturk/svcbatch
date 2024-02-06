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
rem Dummy SvcBatch service manager
rem
rem
setlocal
set "SERVICE_NAME=adummysvc"
set "DISPLAY_NAME=A Dummy Service"
rem
rem
set "SERVICE_EXEC=svcbatch.exe"
rem
rem set "SERVICE_EXEC=service.exe"
rem
pushd "..\build\rel"
set "BUILD_DIR=%cd%"
popd
rem
set "SERVICE_MAIN=%BUILD_DIR%\%SERVICE_EXEC%"
rem
if /i "x%~1" == "xcreate"   goto doCreate
if /i "x%~1" == "xdelete"   goto doDelete
if /i "x%~1" == "xremove"   goto doRemove
if /i "x%~1" == "xrotate"   goto doRotate
if /i "x%~1" == "xstart"    goto doStart
if /i "x%~1" == "xstop"     goto doStop
rem
echo %~nx0: Unknown command %~1
exit /B 1
rem
:doCreate
rem
pushd "%~dp0"
set "TEST_DIR=%cd%"
popd
if not exist "%BUILD_DIR%" (
    echo.
    echo Cannot find build directory.
    echo Run [n]make tests _DEBUG=1
    exit /B 1
)
rem Check long paths
set "LONG_STRING=0123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123456790123"
rem
set "SERVICE_BATCH=.\dummyservice.bat"
rem
rem Presuming this is the build tree ...
rem Create a service command line
rem
rem
%SERVICE_MAIN% create "%SERVICE_NAME%" ^
    --DisplayName "%DISPLAY_NAME%" ^
    --Description "One dummy SvcBatch service example" ^
    --depend Tcpip/Afd ^
    --privs SeShutdownPrivilege ^
    --start manual ^
    --Preshutdown 22000 ^
    --set FailMode 1 ^
    --set UseLocalTime Yes ^
    --set AcceptPreshutdown Yes ^
    --set Home ..\..\test ^
    --set Work ..\build\dbg ^
    --set Logs Logs\$NAME ^
    --set Temp $LOGS\temp ^
    --set StdInput 79,0d,0a ^
    --set StopTimeout 12000 ^
    --set LogName $NAME.@Y-@m-@d.log ^
    --set LogRotate On ^
    --set LogRotateSize 20000 ^
    --set LogRotateInterval 5 ^
    --set EnvironmentPrefix $BASENAME ^
    --set Environment [ ^
            ADUMMYSVC_PID=$ProcessId ^
            ADUMMYSVC_VER=$RELEASE ^
            ADUMMYSVC_ARG=$1.$2.$-PROCESSOR_ARCHITECTURE ^
            ADUMMYSVC_VER=$VERSION-$ADUMMYSVC_VER ^
            ${+NAME}_${VERSION}_VER=$ADUMMYSVC_VER ^
            PATH=$HOME;$PATH ^
            ] ^
    --set Export +BCD ^
    --set Arguments [ %SERVICE_BATCH% run ] ^
    --set Stop $0 ^
    --set StopLogName $NAME.stop.log ^
    --set StopMaxLogs 1 ^
    /V:2

rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
rem sc failure adummysvc reset= INFINITE actions= restart/10000
rem sc failureflag adummysvc 1
rem
echo Created %SERVICE_NAME%
goto End
rem
:doLite
rem
rem
%SERVICE_MAIN% create "%SERVICE_NAME%" --quiet ^
    --displayName "A Dummy Service" ^
    --description "One dummy SvcBatch service example" ^
    --username=1 ^
    -h "%TEST_DIR%" %SERVICE_BATCH% run
rem
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo Created %SERVICE_NAME%
goto End
rem
rem
:doStart
rem
rem
echo Starting %SERVICE_NAME%
rem Wait until the service is Running
rem
shift
set START_CMD_ARGS=
:setStartArgs
if "x%~1" == "x" goto doneStartArgs
set "START_CMD_ARGS=%START_CMD_ARGS% "%~1""
shift
goto setStartArgs
:doneStartArgs
rem
%SERVICE_MAIN% start "%SERVICE_NAME%" --wait=10 %START_CMD_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo Started %SERVICE_NAME%
goto End
rem
rem
:doStop
rem
rem
echo Stopping %SERVICE_NAME%
rem Wait up to 30 seconds until the service is Stopped
rem
shift
set STOP_CMD_ARGS=
:setStopArgs
if "x%~1" == "x" goto doneStopArgs
set "STOP_CMD_ARGS=%STOP_CMD_ARGS% "%~1""
shift
goto setStopArgs
:doneStopArgs
rem
%SERVICE_MAIN% stop "%SERVICE_NAME%" --wait %STOP_CMD_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo Stopped %SERVICE_NAME%
goto End
rem
rem
:doRotate
rem
rem
%SERVICE_MAIN% control "%SERVICE_NAME%" 234
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doDelete
rem
rem
%SERVICE_MAIN% delete "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo Deleted %SERVICE_NAME%
goto End
rem
rem
:doRemove
rem
rem
pushd "%BUILD_DIR%"
rd /S /Q "Logs" >NUL 2>&1
echo Removed %SERVICE_NAME%
popd
rem
rem
:End
exit /B 0
