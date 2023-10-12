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
rem Apache Tomcat Service SvcBatch management script
rem --------------------------------------------------
rem
rem
setlocal
rem
rem
set "EXECUTABLE=svcbatch.exe"
rem Set default service name
set "DEFAULT_SERVICE_NAME=Tomcat11"
set "SERVICE_NAME=%DEFAULT_SERVICE_NAME%"
rem
rem Parse the Arguments
rem
if "x%~1x" == "xx" goto displayUsage
rem
set "SERVICE_CMD=%~1"
shift
rem
set CMD_LINE_ARGS=
rem Process additional command arguments
if "x%~1x" == "xx" goto doneSetArgs
rem Set service name
set "SERVICE_NAME=%~1"
shift
rem
:setArgs
if "x%~1x" == "xx" goto doneSetArgs
set "CMD_LINE_ARGS=%CMD_LINE_ARGS% "%~1""
shift
goto setArgs
:doneSetArgs
rem
rem Process the requested command
rem
if /i "%SERVICE_CMD%" == "create"  goto doCreate
if /i "%SERVICE_CMD%" == "delete"  goto doDelete
if /i "%SERVICE_CMD%" == "start"   goto doStart
if /i "%SERVICE_CMD%" == "stop"    goto doStop
rem
rem
echo Unknown command "%SERVICE_CMD%"
:displayUsage
echo.
echo Usage: %~nx0 command [service_name] [arguments ...]
echo commands:
echo   create            Create the service
echo   delete            Delete the service
echo   start             Start the service
echo   stop              Stop the service
rem
exit /B 1
rem
rem
:doCreate
rem
rem
rem Set batch file to execute
set "SVCBATCH_FILE=bin\catalina.bat"
rem
rem Use the service batch file for shutdown
set "SHUTDOWN_FILE=/S:@"
rem
rem Set the log name
set "SERVICE_LOGNAME=/LN:service.@Y-@m-@d.log"
rem
rem set "SERVICE_LOGNAME=/LN:service.@Y-@m-@d.log /SL:service.stop.log /SM:1"
rem
rem
rem
%EXECUTABLE% create "%SERVICE_NAME%" ^
    --displayName "Apache Tomcat 11.0 %SERVICE_NAME%" ^
    --description "Apache Tomcat 11.1.x Server - https://tomcat.apache.org/" ^
    --start=auto ^
    /F:LP -h .. %SERVICE_LOGNAME% ^
    %SHUTDOWN_FILE% %CMD_LINE_ARGS% %SVCBATCH_FILE% run
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStart
rem
rem
%EXECUTABLE% start "%SERVICE_NAME%" -- %CMD_LINE_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStop
rem
rem
%EXECUTABLE% stop "%SERVICE_NAME%" -- %CMD_LINE_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doDelete
rem
rem
%EXECUTABLE% delete "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
rem
rem
:End
exit /B 0
