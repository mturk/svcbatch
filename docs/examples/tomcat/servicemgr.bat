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
rem SvcBatch Management Tool for Apache Tomcat
rem
rem Usage: servicemgr.bat create|delete|rotate|dump|start|stop [service_name]
rem
setlocal
rem
rem Set default service name
set "SERVICE_NAME=tomcat10"
rem
if "x%~2" == "x" goto doMain
set "SERVICE_NAME=%~2"
rem
:doMain
rem
if /i "x%~1" == "xcreate" goto doCreate
if /i "x%~1" == "xdelete" goto doDelete
if /i "x%~1" == "xrotate" goto doRotate
if /i "x%~1" == "xdump"   goto doDumpStacks
if /i "x%~1" == "xstart"  goto doStart
if /i "x%~1" == "xstop"   goto doStop
rem Unknown option
echo Error: Unknown option '%~1'
goto Einval
rem
rem
rem Create service
:doCreate
pushd %~dp0
set "SERVICE_BASE=%cd%"
rem
rem
rem Run catalina.bat directly
set "SVCBATCH_FILE=bin\catalina.bat"
set "SVCBATCH_ARGS=run"
rem Use simple wrapper script if you need
rem to customize environment before running catalina.bat
rem set "SVCBATCH_FILE=bin\winservice.bat"
rem
rem Set shutdown file
set "SHUTDOWN_FILE=bin\shutdown.bat"
rem
rem Enable log rotation
rem set "ROTATE_RULE=-r1M"
rem
rem Set log name
set "SERVICE_LOGNAME=-n svcbatch.@Y-@m-@d"
rem
sc create "%SERVICE_NAME%" binPath= "\"%SERVICE_BASE%\svcbatch.exe\" /bl /h ..\ /w work %ROTATE_RULE% %SERVICE_LOGNAME% /s %SHUTDOWN_FILE% %SVCBATCH_FILE% %SVCBATCH_ARGS%"
sc config "%SERVICE_NAME%" DisplayName= "Apache Tomcat 10.0 %SERVICE_NAME% Service"
sc description "%SERVICE_NAME%" "Apache Tomcat 10.0.0 Server - https://tomcat.apache.org/"
rem
rem Ensure the networking services are running
rem and that service is started on system startup
sc config "%SERVICE_NAME%" depend= Tcpip/Afd start= auto
rem
rem Set required privileges so we can kill process tree
rem even if Tomcat created multiple child processes.
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
popd
goto End
rem
rem Delete service
:doDelete
rem
echo Stopping %SERVICE_NAME%
rem
sc stop "%SERVICE_NAME%"
rem
if %ERRORLEVEL% equ 0 (
  echo.
  echo Waiting for %SERVICE_NAME% service to stop ...
  ping -n 6 127.0.0.1 >NUL
  echo.
)
rem
echo Deleting %SERVICE_NAME%
rem
sc delete "%SERVICE_NAME%"
goto End
rem
rem Rotate SvcBatch logs
:doRotate
rem
sc control "%SERVICE_NAME%" 234
goto End
rem
rem Send CTRL_BREAK_EVENT
rem JVM will dump full thread stack to log file
:doDumpStacks
rem
sc control "%SERVICE_NAME%" 233
goto End
rem
rem Start service
:doStart
rem
sc start "%SERVICE_NAME%"
goto End
rem
rem Stop service
:doStop
rem
rem
sc stop "%SERVICE_NAME%"
goto End
rem
:Einval
echo Usage: %~nx0 create^|delete^|rotate^|dump^|start^|stop [service_name]
echo.
exit /b 1
rem
:End
exit /b 0
