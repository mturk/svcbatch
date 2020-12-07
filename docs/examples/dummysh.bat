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
rem Dummy SvcBatch service starting bash
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
rem Modify to your Msys2/Cygwin
rem set "PATH=C:\msys64\usr\bin;%PATH%"
set "PATH=C:\cygwin64\bin;%PATH%"
bash.exe --noprofile --norc "%cd%\dummysh.sh"
goto End

:doCreate
rem
rem
set "SERVICE_NAME=dummysh"
rem Presuming this is the build tree ...
rem
sc create "%SERVICE_NAME%" binPath= ""%cd%\..\..\x64\svcbatch.exe" -w "%cd%" %~nx0"
sc config "%SERVICE_NAME%" DisplayName= "A Dummy Bash Service"
sc description "%SERVICE_NAME%" "Run shell script as service using Msys2/Cygwin bash"
sc privs "%SERVICE_NAME%" SeDebugPrivilege
goto End

:doDelete
rem
rem
set "SERVICE_NAME=dummysh"
sc stop "%SERVICE_NAME%" >NUL
sc delete "%SERVICE_NAME%"
rem
rem rd /S /Q Logs 2>NUL

:End
