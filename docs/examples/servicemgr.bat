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
rem Apache Tomcat Service Tool
rem
rem Usage: servicemgr.bat create/delete/rotate/dump [service_name]
rem
setlocal
rem
if "x%~2" == "x" goto Einval
set "SERVICE_NAME=%~2"
rem
if /i "x%~1" == "xcreate" goto doCreate
if /i "x%~1" == "xdelete" goto doDelete
if /i "x%~1" == "xrotate" goto doRotate
if /i "x%~1" == "xdump"   goto doDumpStacks
rem Unknown option
echo %nx0: Unknown option '%~1'
goto Einval
rem
rem Create service
:doCreate
set "SERVICE_BASE=%cd%"
pushd ..
set "SERVICE_HOME=%cd%"
popd
rem
rem Change to actual Tomcat version
set "TOMCAT_DISPLAY=Apache Tomcat 10.0"
set "TOMCAT_FULLVER=Apache Tomcat 10.0.0"
rem
sc create "%SERVICE_NAME%" binPath= ""%SERVICE_BASE%\svcbatch.exe" /w "%SERVICE_HOME%" /s /c .\bin\winservice.bat"
sc config "%SERVICE_NAME%" DisplayName= "%TOMCAT_DISPLAY% %SERVICE_NAME% Service"
rem Ensure the networking services are running
rem and that service is started on system startup
sc config "%SERVICE_NAME%" depend= Tcpip/Afd start= auto
rem Set required privileges so we can kill process tree
rem even if Tomcat created multiple child processes.
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
sc description "%SERVICE_NAME%" "%TOMCAT_FULLVER% Server - https://tomcat.apache.org/"
goto End

rem
rem Delete service
:doDelete
rem
sc stop "%SERVICE_NAME%" >NUL
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

:Einval
echo Usage: %nx0 create/delete/rotate/dump [service_name]
echo.
exit /b 1

:End
