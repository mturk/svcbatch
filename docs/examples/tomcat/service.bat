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
rem Apache Tomcat Service management script
rem
rem Usage: service.bat command [service_name]
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
if "x%~1x" == "xx" goto doneSetArgs
rem Set service name
set "SERVICE_NAME=%DEFAULT_SERVICE_NAME%"
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
if /i "%SERVICE_CMD%" == "dump"    goto doDumpStacks
if /i "%SERVICE_CMD%" == "rotate"  goto doRotate
if /i "%SERVICE_CMD%" == "start"   goto doStart
if /i "%SERVICE_CMD%" == "stop"    goto doStop
rem
rem
echo Unknown command "%SERVICE_CMD%"
:displayUsage
echo.
echo Usage: %~nx0 ( commands ... ) [service_name] [arguments ...]
echo commands:
echo   create            Create the service
echo   delete            Delete the service
echo   dump              Dump JVM full thread stack to the log file
echo   rotate            Rotate log files
echo   start             Start the service
echo   stop              Stop the service
rem
exit /B 1
rem
rem
:doCreate
rem
rem
rem Set Home directory
set "SERVICE_HOME=/h.."
rem
rem Set Work directory
rem set "SERVICE_WORK=/w nodes\01"
rem
rem Set batch file to execute
set "SVCBATCH_FILE=bin\catalina.bat"
rem
rem Use the service batch file for shutdown
set "SHUTDOWN_ARGS=/s?stop"
rem
rem Rotate log each day at midnight or if larger then 1 megabyte
rem set "ROTATE_RULE=-r0 -r1M"
rem
rem Enable manual log rotation by using 'service.bat rotate'
rem set "ROTATE_RULE=%ROTATE_RULE% -rS"
rem
rem Set the log name
set "SERVICE_LOGNAME=/nservice.@Y-@m-@d"
rem set "SERVICE_LOGNAME=/n%SERVICE_NAME%service"
rem
rem
rem
%EXECUTABLE% create "%SERVICE_NAME%" ^
    /displayName "Apache Tomcat 11.0 %SERVICE_NAME%" ^
    /description "Apache Tomcat 11.1.x Server - https://tomcat.apache.org/" ^
    /start:auto ^
    /blv %SERVICE_HOME% %SERVICE_LOGNAME% ^
    %SHUTDOWN_ARGS% %CMD_LINE_ARGS% %SVCBATCH_FILE% run
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem Send CTRL_BREAK_EVENT
rem The JVM will dump the full thread stack to the log file
:doDumpStacks
rem
rem
%EXECUTABLE% control "%SERVICE_NAME%" 233
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doRotate
rem
rem
%EXECUTABLE% control "%SERVICE_NAME%" 234
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStart
rem
rem
%EXECUTABLE% start "%SERVICE_NAME%" /wait %CMD_LINE_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStop
rem
rem
%EXECUTABLE% stop "%SERVICE_NAME%" /wait %CMD_LINE_ARGS%
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
