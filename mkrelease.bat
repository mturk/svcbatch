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
rem    eg: mkrelease 1.0.6 "_VENDOR_SFX=_1"
rem
setlocal
if "x%~1" == "x" goto Einval
rem
set "ProjectName=svcbatch"
set "ReleaseArch=x64"
set "ReleaseVersion=%~1"
rem
set "ReleaseName=%ProjectName%-%ReleaseVersion%-win-%ReleaseArch%"
pushd %~dp0
set "BuildDir=%cd%"
popd
rem
rem Create builds
rd /S /Q "%ReleaseArch%" 2>NUL
nmake /nologo _STATIC_MSVCRT=1 %~2 %~3 %~4 >NUL
rem Set path for ClamAV and 7za
rem
set "PATH=C:\Tools\clamav;C:\Utils;%PATH%"
rem
freshclam.exe --quiet
pushd "%ReleaseArch%"
echo ## Binary release v%ReleaseVersion% > %ReleaseName%.txt
echo. >> %ReleaseName%.txt
echo. >> %ReleaseName%.txt
echo ```no-highlight >> %ReleaseName%.txt
clamscan.exe --version >> %ReleaseName%.txt
clamscan.exe --bytecode=no %ProjectName%.exe >> %ReleaseName%.txt
echo ``` >> %ReleaseName%.txt
7za.exe a -bd %ReleaseName%.zip %ProjectName%.exe
sigtool.exe --sha256 %ReleaseName%.zip >> %ReleaseName%-sha256.txt
popd
goto End
rem
:Einval
echo Error: Invalid parameter
echo Usage: %~nx0 version
exit /b 1

:End
