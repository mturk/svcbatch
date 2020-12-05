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
rem
if /i "x%~1" == "xcreate" goto doCreate
if /i "x%~1" == "xdelete" goto doDelete
rem
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
rem
rem Send shutdown signal
rem %SVCBATCH_SERVICE_SELF% /k S %SVCBATCH_SERVICE_UUID%
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
set "SERVICE_NAME=adummysvc"
rem Presuming this is the build tree ...
rem
sc create "%SERVICE_NAME%" binPath= ""%cd%\..\..\x64\svcbatch.exe" -w "%cd%" %~nx0"
sc config "%SERVICE_NAME%" DisplayName= "A Dummy Service"
sc description "%SERVICE_NAME%" "One dummy SvcBatch service example"
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
goto End

:doDelete
rem
rem
set "SERVICE_NAME=adummysvc"
sc stop "%SERVICE_NAME%" >NUL
sc delete "%SERVICE_NAME%"
rem
rem rd /S /Q Logs 2>NUL

:End
