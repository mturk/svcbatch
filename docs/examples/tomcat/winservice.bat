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
rem Usage: winservice.bat command [service_name]
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
pushd %~dp0
set "SERVICE_BASE=%cd%"
popd
rem
rem Set the path to the svcbatch.exe
set "SVCBATCH=%SERVICE_BASE%\svcbatch.exe"
rem
if not exist "%SVCBATCH%" (
  echo The svcbatch.exe file does not exist
  echo Set SVCBATCH environment variable
  echo to the correct location of the svcbatch executable
  exit /B 1
)
rem
rem Set Home directory
set "SERVICE_HOME=/h .."
rem
rem Set Work directory
rem set "SERVICE_WORK=/w nodes\01"
rem
rem Set batch file to execute
set "SVCBATCH_FILE=bin\runservice.bat"
rem
rem Set Arguments to the SVCBATCH_FILE
set "SVCBATCH_ARGS=run"
rem
rem Set shutdown file
set "SHUTDOWN_FILE=/s %SVCBATCH_FILE%"
rem
rem Set Arguments to the SHUTDOWN_FILE
set "SHUTDOWN_ARGS=/s stop"
rem
rem Rotate log each day at midnight or if larger then 1 megabyte
rem set "ROTATE_RULE=-r0 -r1M"
rem
rem Enable manual log rotation by using 'winservice.bat rotate'
rem set "ROTATE_RULE=%ROTATE_RULE% -rS"
rem
rem Set the log name
set "SERVICE_LOGNAME=/n svcbatch.@Y-@m-@d"
rem
rem
rem
sc create "%SERVICE_NAME%" binPath= "\"%SVCBATCH%\" /blv %SERVICE_HOME% %SERVICE_WORK% %ROTATE_RULE% %SERVICE_LOGNAME% %SHUTDOWN_FILE% %SHUTDOWN_ARGS% %SVCBATCH_FILE% %SVCBATCH_ARGS%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem Set Display Name and Description
sc config "%SERVICE_NAME%" DisplayName= "Apache Tomcat 10.0 %SERVICE_NAME% Service" >NUL
if %ERRORLEVEL% neq 0 goto Failed
sc description "%SERVICE_NAME%" "Apache Tomcat 10.0.0 Server - https://tomcat.apache.org/"  >NUL
if %ERRORLEVEL% neq 0 goto Failed
rem
rem Ensure the networking services are running
rem and that service is started on system startup
rem
sc config "%SERVICE_NAME%" depend= Tcpip/Afd start= auto >NUL
if %ERRORLEVEL% neq 0 goto Failed
rem
rem Set required privileges so we can kill the process tree
rem in case the service created multiple child processes.
rem
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege >NUL
if %ERRORLEVEL% neq 0 goto Failed
rem
echo %~nx0: Created %SERVICE_NAME%
goto End
rem
rem Send CTRL_BREAK_EVENT
rem The JVM will dump the full thread stack to the log file
:doDumpStacks
rem
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
:doStart
rem
rem
sc start "%SERVICE_NAME%"
goto End
rem
rem
:doStop
rem
rem
sc stop "%SERVICE_NAME%"
goto End
rem
rem
:doDelete
rem
rem
sc stop "%SERVICE_NAME%" >NUL
rem
if %ERRORLEVEL% equ 0 (
  echo.
  echo %~nx0: Waiting for %SERVICE_NAME% to stop ...
  ping -n 6 127.0.0.1 >NUL
  echo.
)
rem
sc delete "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo %~nx0: Deleted %SERVICE_NAME%
goto End
rem
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
