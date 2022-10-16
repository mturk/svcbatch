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
rem
set "SERVICE_NAME="
if "x%SVCBATCH_SERVICE_NAME%" == "x" goto noService
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
echo.
rem Dump environment variables to log file
set
echo.
rem
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
rem
rem
:doCreate
rem
rem Rotate Log files each 30 minutes or when larger then 100Kbytes
set "ROTATE_RULE=-r @30~100K"
rem Uncomment to disable log rotation
rem set "ROTATE_RULE=-r 0"
rem
rem Presuming this is the build tree ...
rem Create a service command line
set "SERVICE_CMDLINE=\"%cd%\..\..\x64\svcbatch.exe\" -pDbw \"%cd%\" -o \"Logs\%SERVICE_NAME%\" %ROTATE_RULE% -s dummyshutdown.bat %~nx0"
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
echo Deleted %SERVICE_NAME%
goto End
rem
:doRemove
rem
rem
sc stop "%SERVICE_NAME%" >NUL 2>&1
sc delete "%SERVICE_NAME%" >NUL 2>&1
rd /S /Q "Logs" >NUL 2>&1
echo Removed %SERVICE_NAME%
goto End
rem
:noService
echo SVCBATCH_SERVICE_NAME not defined
exit /B 1
rem
rem
:End
exit /B 0
