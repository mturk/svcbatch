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
rem Dummy SvcBatch service manager
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
echo %~nx0: Unknown command %~1
exit /B 1
rem
:doCreate
rem
pushd %~dp0
set "_TESTS_DIR=%cd%"
popd
if not exist "%_TESTS_DIR%\..\.build\dbg" (
    echo.
    echo Cannot find build directory.
    echo Run [n]make tests _DEBUG=1
    exit /B 1
)
pushd ..\.build\dbg
set "_BUILD_DIR=%cd%"
popd
set "SERVICE_LOG_FNAME="
set "SHUTDOWN_ARGS="
set "ROTATE_RULE="
set "SERVICE_BATCH=dummyservice.bat"
set "SERVICE_SHUTDOWN=-s %SERVICE_BATCH%"
rem
rem Uncomment to use separate shutdown file
rem set "SERVICE_SHUTDOWN=-s dummyshutdown.bat"
rem Set arguments for shutdown bat file
set "SHUTDOWN_ARGS=-s shutdown /S@@SystemRoot@@ /s\"argument with spaces\""
rem
rem
set "SERVICE_LOG_DIR=-o \"Logs/%SERVICE_NAME%\""
rem Rotate Log files each 10 minutes or when larger then 100Kbytes
set "ROTATE_RULE=/R 10 -r100K"
rem Rotate Log files at midnight
rem set "ROTATE_RULE=-r0"
rem Rotate Log files every full hour or when larger then 40000 bytes
rem set "ROTATE_RULE=-r60 -r 40000B"
rem Rotate only by signal
rem set "ROTATE_RULE=-rS"
rem
rem Set log file names instead default SvcBatch.log and SvcBatch.shutdown.log
rem Both .log and .shutdown.log extensions are added to the -n parameter
set "SERVICE_LOG_FNAME=-n \"%SERVICE_NAME%\""
rem
rem set "SERVICE_LOG_FNAME=-n \"%SERVICE_NAME%.@Y-@m-@d.@H@M@S\""
rem
rem set "SERVICE_LOG_FNAME=-n \"%SERVICE_NAME%.@Y-@m-@d\""
rem
rem
rem Presuming this is the build tree ...
rem Create a service command line
rem
set "SERVICE_CMDLINE=\"%_BUILD_DIR%\svcbatch.exe\" @%SERVICE_NAME% -pvbL /h \"%_TESTS_DIR%\" %SERVICE_LOG_DIR% %SERVICE_LOG_FNAME% %ROTATE_RULE% %SERVICE_SHUTDOWN% %SHUTDOWN_ARGS% %SERVICE_BATCH% run @SystemDrive@"
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
echo %~nx0: Deleted %SERVICE_NAME%
goto End
rem
:doRemove
rem
rem
sc stop "%SERVICE_NAME%" >NUL 2>&1
sc delete "%SERVICE_NAME%" >NUL 2>&1
rd /S /Q "Logs" >NUL 2>&1
echo %~nx0: Removed %SERVICE_NAME%
goto End
rem
rem
:End
exit /B 0
