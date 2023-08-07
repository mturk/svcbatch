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
rem Usage: servicemgr.bat command [service_name]
rem
setlocal
rem
rem
rem Set default service name
set "SERVICE_NAME=tomcat10"
if not "x%~2" == "x" (
  set "SERVICE_NAME=%~2"
)
rem
rem
rem
if /i "x%~1" == "xcreate"  goto doCreate
if /i "x%~1" == "xdelete"  goto doDelete
if /i "x%~1" == "xdump"    goto doDumpStacks
if /i "x%~1" == "xrotate"  goto doRotate
if /i "x%~1" == "xstart"   goto doStart
if /i "x%~1" == "xstop"    goto doStop
rem
rem
echo Unknown command %~1
echo.
echo Usage: winservice ( commands ... ) [service_name]
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
set "SERVICE_HOME=/h .."
rem
rem Set Work directory
rem set "SERVICE_WORK=/w nodes\01"
rem
rem Set batch file to execute
set "SVCBATCH_FILE=bin\winservice.bat"
rem Call catalina.bat directly
rem set "SVCBATCH_FILE=bin\catalina.bat"
rem set "SHUTDOWN_FILE=bin\shutdown.bat"
rem
rem Set Arguments to the SVCBATCH_FILE
set "SVCBATCH_ARGS=run"
rem
rem Set shutdown file
rem set "SHUTDOWN_FILE=-s%SVCBATCH_FILE%"
set "SHUTDOWN_FILE=/s?stop"
rem
rem Set Arguments to the SHUTDOWN_FILE
rem set "SHUTDOWN_ARGS=-sstop"
rem
rem Rotate log each day at midnight or if larger then 1 megabyte
rem set "ROTATE_RULE=-r0 -r1M"
rem
rem Enable manual log rotation by using 'servicemgr.bat rotate'
set "ROTATE_RULE=%ROTATE_RULE% -rS"
rem
rem Set the log name
set "SERVICE_LOGNAME=/nservice.@Y-@m-@d"
rem
rem
rem
svcbatch create "%SERVICE_NAME%" ^
    /displayName "Apache Tomcat 10.1 %SERVICE_NAME% Service" ^
    /description "Apache Tomcat 10.1.x Server - https://tomcat.apache.org/" ^
    /depend=Tcpip/Afd /privs:SeCreateSymbolicLinkPrivilege/SeDebugPrivilege ^
    /start auto ^
    /blv %SERVICE_HOME% %SERVICE_WORK% %ROTATE_RULE% %SERVICE_LOGNAME% ^
    %SHUTDOWN_FILE% %SHUTDOWN_ARGS% %SVCBATCH_FILE% %SVCBATCH_ARGS%
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
echo %~nx0: Created %SERVICE_NAME%
goto End
rem
rem Send CTRL_BREAK_EVENT
rem The JVM will dump the full thread stack to the log file
:doDumpStacks
rem
rem
svcbatch control "%SERVICE_NAME%" 233
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doRotate
rem
rem
svcbatch control "%SERVICE_NAME%" 234
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStart
rem
rem
svcbatch start "%SERVICE_NAME%" /wait
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStop
rem
rem
svcbatch stop "%SERVICE_NAME%" /wait
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doDelete
rem
rem
svcbatch delete "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
rem
rem
:End
exit /B 0
