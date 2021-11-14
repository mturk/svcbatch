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
rem Compile SvcBatch and run api test suite
rem
rem Make sure to set build environment before
rem executing this script
rem
setlocal
pushd %~dp0
set "BaseDir=%cd%"
popd
rem
pushd ..
echo.
echo Compiling SvcBatch
rd /S /Q x64 2>NUL
gcc -v 2>NUL
if not %ERRORLEVEL% == 0 goto Msvc
make -f Makefile.gmk _RUN_API_TESTS=1 1>NUL
if not %ERRORLEVEL% == 0 goto Failed
goto Compiled
:Msvc
nmake /nologo _RUN_API_TESTS=1 _STATIC_MSVCRT=1 1>NUL
if not %ERRORLEVEL% == 0 goto Failed
rem
:Compiled
echo.
popd
pushd ..\x64
set "BuildDir=%cd%"
popd
rem
rd /S /Q Logs 2>NUL
echo Runnig tests in: %BaseDir%
echo Using SvcBatch : %BuildDir%\svcbatch.exe
echo.
%BuildDir%\svcbatch.exe -c -w %BaseDir% /S -b -r @30~100k noservice.bat noservice %*
if not %ERRORLEVEL% == 0 goto Failed

echo.
echo Done!
exit /B 0

:Failed
echo.
echo Test failed!
exit /B 1
