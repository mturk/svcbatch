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
rem Apache Tomcat Service script
rem
rem Usage: winservice.bat create|rotate|dump [service_name]
rem
setlocal
rem
rem
if /i "x%~1" == "xcreate" goto doCreate
if /i "x%~1" == "xrotate" goto doRotate
if /i "x%~1" == "xdump"   goto doDumpStacks
rem
rem Presume we are running as service
rem
echo %~nx0: [%TIME%] Running %SVCBATCH_SERVICE_NAME% Service
echo.
rem Set here any environment variables
rem that are missing from LOCAL_SERVICE account
rem
rem eg. set "JAVA_HOME=C:\Your\JDK\location"
rem     set "JRE_HOME=C:\Your\JRE\location"
rem
rem If you have created a separate copy using
rem     makebase.bat ..\nodes\01 -w
rem
rem     Set CATALINA_HOME and CATALINA_BASE variables
rem
rem     set "CATALINA_HOME=%SVCBATCH_SERVICE_HOME%"
rem     set "CATALINA_BASE=%SVCBATCH_SERVICE_HOME%\nodes\01"
rem
rem     When creating service add '-o nodes\01\logs'
rem     so that SvcBatch.log is created where
rem     the Tomcat logs will be created.
rem
rem Run Apache Tomcat
call "%SVCBATCH_SERVICE_BASE%\catalina.bat" %*
rem
exit /B %ERRORLEVEL%
rem
rem
:doCreate
rem
rem Set default service name
set "SERVICE_NAME=tomcat10"
if "x%~2" neq "x" (
  set "SERVICE_NAME=%~2"
)
rem
pushd %~dp0
set "SERVICE_BASE=%cd%"
popd
rem
rem Set Home directory
set "SERVICE_HOME=/h ..\\"
rem
rem Run catalina.bat directly
rem set "SVCBATCH_FILE=bin\catalina.bat"
rem
set "SVCBATCH_FILE=bin\%~nx0"
rem
rem Set Arguments to the SVCBATCH_FILE
set "SVCBATCH_ARGS=run"
rem
rem Set shutdown file
set "SHUTDOWN_FILE=/s bin\shutdown.bat"
rem
rem Rotate log each day at midnight or if larger the 1 megabyte
rem set "ROTATE_RULE=-r0 -r1M"
rem
rem Set log name
set "SERVICE_LOGNAME=/n svcbatch.@Y-@m-@d"
rem
sc create "%SERVICE_NAME%" binPath= "\"%SERVICE_BASE%\svcbatch.exe\" /bl %SERVICE_HOME% %ROTATE_RULE% %SERVICE_LOGNAME% %SHUTDOWN_FILE% %SVCBATCH_FILE% %SVCBATCH_ARGS%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem Set Display Name and Description
sc config "%SERVICE_NAME%" DisplayName= "Apache Tomcat 10.0 %SERVICE_NAME% Service" >NUL
if %ERRORLEVEL% neq 0 goto Failed
sc description "%SERVICE_NAME%" "Apache Tomcat 10.0.0 Server - https://tomcat.apache.org/"  >NUL
if %ERRORLEVEL% neq 0 goto Failed
rem
rem Ensure the networking services are running
rem and that service is started on system startup
sc config "%SERVICE_NAME%" depend= Tcpip/Afd start= auto >NUL
if %ERRORLEVEL% neq 0 goto Failed
rem
rem Set required privileges so we can kill process tree
rem even if Tomcat created multiple child processes.
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege >NUL
if %ERRORLEVEL% neq 0 goto Failed
rem
echo %~nx0: Created %SERVICE_NAME%
goto End
rem
rem Send CTRL_BREAK_EVENT
rem JVM will dump full thread stack to log file
:doDumpStacks
rem
sc control "%SERVICE_NAME%" 233
goto End
rem
rem
:doRotate
rem
rem
sc control "%SERVICE_NAME%" 234
goto End
rem
rem
:Failed
rem
rem Service installation Failed
rem
sc delete "%SERVICE_NAME%" >NUL 2>&1
exit /B 1
rem
rem
:End
exit /B 0
