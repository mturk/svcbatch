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
rem Usage: mkrelease.bat version [options]
rem    eg: mkrelease 1.2.0_1 "_VENDOR_SFX=_1"
rem
setlocal
if "x%~1" == "x" goto Einval
rem
set "ProjectName=svcbatch"
set "ReleaseArch=x64"
set "ReleaseVersion=%~1"
set "MakefileFlags=_STATIC_MSVCRT=1"
shift
:setArgs
if "x%~1" == "x" goto doneArgs
set "MakefileFlags=%MakefileFlags% %~1"
shift
goto setArgs
rem
:doneArgs
rem
set "ReleaseName=%ProjectName%-%ReleaseVersion%-win-%ReleaseArch%"
set "ReleaseLog=%ReleaseName%.txt
pushd %~dp0
set "BuildDir=%cd%"
popd
rem
rem Create builds
nmake /nologo /A %MakefileFlags%
if not %ERRORLEVEL% == 0 goto Failed
rem
pushd "%ReleaseArch%"
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
echo nmake %MakefileFlags% >> %ReleaseLog%
findstr /B /C:"Microsoft (R) " %ProjectName%.p >> %ReleaseLog%
echo. >> %ReleaseLog%
rem
del /F /Q %ProjectName%.i 2>NUL
del /F /Q %ProjectName%.p 2>NUL
rem
rem Set path for ClamAV and 7za if needed
rem
rem set "PATH=C:\Tools\clamav;C:\Utils;%PATH%"
rem
freshclam.exe --quiet
clamscan.exe --version >> %ReleaseLog%
clamscan.exe --bytecode=no %ProjectName%.exe >> %ReleaseLog%
echo ``` >> %ReleaseLog%
del /F /Q %ReleaseName%.zip 2>NUL
7za.exe a -bd %ReleaseName%.zip %ProjectName%.exe
sigtool.exe --sha256 %ReleaseName%.zip > %ReleaseName%-sha256.txt
popd
goto End
rem
:Einval
echo Error: Invalid parameter
echo Usage: %~nx0 version [options]
exit /b 1

:Failed
echo.
echo Error: Cannot build %ProjectName%.exe
exit /b 1

:End
exit /b 0
