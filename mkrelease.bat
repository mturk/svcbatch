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
rem Usage: mkrelease.bat [version]
rem
setlocal
if not "x%~1" == "x" goto haveVersion
rem
echo Error: Missing release version
echo Usage: %~nx0 [version]
exit /b 1

:haveVersion
set "SvcBatchVer=%~1"
rem
rem Set path for ClamAV and 7za
rem
set "PATH=C:\Tools\clamav;C:\Utils;%PATH%"
rem
freshclam.exe --quiet
pushd x64
echo ## Binary release v%SvcBatchVer% > svcbatch-%SvcBatchVer%.txt
echo. >> svcbatch-%SvcBatchVer%.txt
echo. >> svcbatch-%SvcBatchVer%.txt
echo ```no-highlight >> svcbatch-%SvcBatchVer%.txt
clamscan.exe --version >> svcbatch-%SvcBatchVer%.txt
clamscan.exe --bytecode=no svcbatch.exe >> svcbatch-%SvcBatchVer%.txt
echo ``` >> svcbatch-%SvcBatchVer%.txt
7za.exe a -bd svcbatch-%SvcBatchVer%-win-x64.zip svcbatch.exe ..\LICENSE.txt
sigtool.exe --sha256 svcbatch.exe > svcbatch-%SvcBatchVer%-sha256.txt
sigtool.exe --sha256 svcbatch-%SvcBatchVer%-win-x64.zip >> svcbatch-%SvcBatchVer%-sha256.txt
popd
rem
