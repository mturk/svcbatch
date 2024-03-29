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
rem SvcBatch release helper script
rem
rem Usage: mkrelease.bat [options] version [make arguments]
rem    eg: mkrelease 1.2.3
rem        mkrelease 1.2.3.45 "VERSION_SFX=_1.acme"
rem        mkrelease /d ...   create debug release
rem        mkrelease /s ...   compile with static msvcrt
rem        mkrelease /h ...   compile with hybrid crt
rem        mkrelease /t ...   enable debug trace
rem
setlocal
rem
set "ProjectName=svcbatch"
set "ReleaseArch=win-x64"
set "BuildDir=build\rel"
set "ProjectFiles=%ProjectName%.exe"
set "DebugPrefix="
set "TracePrefix="
set "MakefileArgs="
rem
:getOpts
rem
if /i "x%~1" == "x/d" goto setDebug
if /i "x%~1" == "x/h" goto setHybrid
if /i "x%~1" == "x/s" goto setStatic
if /i "x%~1" == "x/t" goto setTrace
rem
goto doneOpts
rem
:setDebug
set "BuildDir=build\dbg"
set "MakefileArgs=%MakefileArgs% DEBUG_BUILD=1 PROGRAM_EEXT=.debug"
set "DebugPrefix=debug-"
set "ProjectFiles=%ProjectName%.exe.debug"
set "ProjectFiles=%ProjectFiles% %ProjectName%.pdb"
shift
goto getOpts
rem
:setHybrid
set "MakefileArgs=%MakefileArgs% HYBRID_MSVCRT=1"
shift
goto getOpts
rem
:setStatic
set "MakefileArgs=%MakefileArgs% STATIC_MSVCRT=1"
shift
goto getOpts
rem
:setTrace
set "MakefileArgs=%MakefileArgs% DEBUG_TRACE=1 PROGRAM_EEXT=.trace"
set "TracePrefix=trace-"
set "ProjectFiles=%ProjectName%.exe.trace"
shift
goto getOpts
rem
rem
:doneOpts
rem
if "x%~1" == "x" goto Einval
rem
set "ReleaseVersion=%~1"
shift
rem
:setArgs
if "x%~1" == "x" goto doneArgs
set "MakefileArgs=%MakefileArgs% %~1"
shift
goto setArgs
rem
:doneArgs
rem
if "x%DebugPrefix%x" == "xx" goto donePrefix
if "x%TracePrefix%x" == "xx" goto donePrefix
echo Error: Command options /d and /t are mutually exclusive
exit /B 1
rem
:donePrefix
rem
set "ReleaseArch=%DebugPrefix%%TracePrefix%%LitePrefix%%ReleaseArch%"
rem nmake /nologo %MakefileArgs% clean
set "ReleaseName=%ProjectName%-%ReleaseVersion%-%ReleaseArch%"
set "ReleaseLog=%ReleaseName%.txt
set "ReleaseZip=%ReleaseName%.zip
rem
rem Create builds
nmake /a %MakefileArgs%
if not %ERRORLEVEL% == 0 goto Failed
rem
pushd "%BuildDir%"
rem
rem Get nmake and cl versions
rem
echo _MSC_FULL_VER > %ProjectName%.i
nmake /? 2>%ProjectName%.p 1>NUL
cl.exe /EP %ProjectName%.i >>%ProjectName%.p 2>&1
rem
echo ## Binary release v%ReleaseVersion% > %ReleaseLog%
echo. >> %ReleaseLog%
echo ```no-highlight >> %ReleaseLog%
echo Compiled using: >> %ReleaseLog%
echo nmake %MakefileArgs% >> %ReleaseLog%
findstr /B /C:"Microsoft (R) " %ProjectName%.p >> %ReleaseLog%
rem
del /F /Q %ProjectName%.i 2>NUL
del /F /Q %ProjectName%.p 2>NUL
echo. >> %ReleaseLog%
echo. >> %ReleaseLog%
rem
del /F /Q %ReleaseZip% 2>NUL
7za.exe a -bd %ReleaseZip% %ProjectFiles%"
certutil -hashfile %ReleaseZip% SHA256 | findstr /v "CertUtil" >> %ReleaseLog%
echo. >> %ReleaseLog%
certutil -hashfile %ReleaseZip% SHA512 | findstr /v "CertUtil" >> %ReleaseLog%
echo. >> %ReleaseLog%
echo ``` >> %ReleaseLog%
popd
goto End
rem
:Einval
echo Error: Invalid parameter
echo Usage: %~nx0 [options] version [arguments]
exit /b 1
rem
:Failed
echo.
echo Error: Cannot build %ProjectName%
exit /b 1
rem
:End
exit /b 0
