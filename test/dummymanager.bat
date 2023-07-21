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
pushd "%~dp0"
set "TEST_DIR=%cd%"
popd
if not exist "%TEST_DIR%\..\.build\dbg" (
    echo.
    echo Cannot find build directory.
    echo Run [n]make tests _DEBUG=1
    exit /B 1
)
pushd "..\.build\dbg"
set "BUILD_DIR=%cd%"
popd
set "SERVICE_LOG_FNAME="
set "SHUTDOWN_ARGS="
set "ROTATE_RULE="
set "SERVICE_BATCH=dummyservice.bat"
set "SERVICE_SHUTDOWN=/s%SERVICE_BATCH%"
rem
rem Uncomment to use separate shutdown file
rem set "SERVICE_SHUTDOWN=-s dummyshutdown.bat"
rem Set arguments for shutdown bat file
set "SHUTDOWN_ARGS=/sshutdown /S@SystemRoot@ "/ssome @@argument @@@@@with spaces""
rem
rem
set "SERVICE_LOG_DIR=-o "Logs/%SERVICE_NAME%""
rem Rotate Log files each 10 minutes or when larger then 100Kbytes
rem set "ROTATE_RULE=/R 10 -r100K"
set "ROTATE_RULE=/R5 -r20K -rS"
rem Rotate Log files at midnight
rem set "ROTATE_RULE=-r0"
rem Rotate Log files every full hour or when larger then 40000 bytes
rem set "ROTATE_RULE=-r60 -r 40000B"
rem Rotate only by signal
rem set "ROTATE_RULE=-rS"
rem
rem Set log file names instead default SvcBatch.log and SvcBatch.shutdown.log
rem Both .log and .shutdown.log extensions are added to the -n parameter
rem set "SERVICE_LOG_FNAME=-n "%SERVICE_NAME%""
rem
rem set "SERVICE_LOG_FNAME=-n "%SERVICE_NAME%.@Y-@m-@d.@H@M@S""
rem
set "SERVICE_LOG_FNAME=/n@N.%%Y-@m-%%d"
rem
rem Set PATH
set "SERVICE_ENVIRONMENT=/ePATH=%BUILD_DIR%;%%PATH%%"
rem
rem Presuming this is the build tree ...
rem Create a service command line
rem
rem
goto allInOne
rem goto doLite
rem
%BUILD_DIR%\svcbatch.exe create "%SERVICE_NAME%" -pbL /h "%TEST_DIR%" "%SERVICE_ENVIRONMENT%" %SERVICE_LOG_DIR%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem Add Additional configuration
%BUILD_DIR%\svcbatch.exe config "%SERVICE_NAME%" /quiet %SERVICE_LOG_FNAME% %ROTATE_RULE% %SERVICE_SHUTDOWN% %SHUTDOWN_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem Add Batch file arguments
%BUILD_DIR%\svcbatch.exe config "%SERVICE_NAME%" /quiet %SERVICE_BATCH% run %%TEMP%%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem Set Display Name and Description
%BUILD_DIR%\svcbatch.exe config "%SERVICE_NAME%" /quiet /display "A Dummy Service" /description "One dummy SvcBatch service example"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem Set Dependencies and Privileges
%BUILD_DIR%\svcbatch.exe config "%SERVICE_NAME%" /quiet /depend=Tcpip/Afd /privs:SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
rem
echo %~nx0: Created %SERVICE_NAME%
goto End
rem
:allInOne
rem
rem
%BUILD_DIR%\svcbatch.exe create "%SERVICE_NAME%" ^
    /displayname "A Dummy Service" /description "One dummy SvcBatch service example" ^
    /depend=Tcpip/Afd /privs:SeShutdownPrivilege ^
    -pbLva /f1 /h "%TEST_DIR%" "%SERVICE_ENVIRONMENT%" %SERVICE_LOG_DIR% ^
    %SERVICE_LOG_FNAME% %ROTATE_RULE% %SERVICE_SHUTDOWN% %SHUTDOWN_ARGS% ^
    %SERVICE_BATCH% run %%TEMP%% %%SOME_RANDOM_VARIABLE%%
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
rem sc failure adummysvc reset= INFINITE actions= restart/10000
rem sc failureflag adummysvc 1
rem
echo %~nx0: Created %SERVICE_NAME%
goto End
rem
:doLite
rem
rem
%BUILD_DIR%\svcbatch.exe create "%SERVICE_NAME%" /quiet ^
    /display "A Dummy Service" /desc "One dummy SvcBatch service example" ^
    /username=1 ^
    /h "%TEST_DIR%" %SERVICE_BATCH% run
rem
rem
echo %~nx0: Created %SERVICE_NAME%
goto End
rem
:doStart
rem
rem
pushd "..\.build\dbg"
set "BUILD_DIR=%cd%"
popd
rem
set "_NX=%~nx0"
echo %_NX%: Starting %SERVICE_NAME%
rem Wait until the service is Running
rem
shift
set START_CMD_ARGS=
:setStartArgs
if "x%~1" == "x" goto doneStartArgs
set "START_CMD_ARGS=%START_CMD_ARGS% "%~1""
shift
goto setStartArgs
:doneStartArgs
rem
%BUILD_DIR%\svcbatch.exe start "%SERVICE_NAME%" /wait %START_CMD_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo %_NX%: Started %SERVICE_NAME%
goto End
rem
:doStop
rem
pushd "..\.build\dbg"
set "BUILD_DIR=%cd%"
popd
rem
set "_NX=%~nx0"
echo %_NX%: Stopping %SERVICE_NAME%
rem Wait up to 30 seconds until the service is Stopped
rem
shift
set STOP_CMD_ARGS=
:setStopArgs
if "x%~1" == "x" goto doneStopArgs
set "STOP_CMD_ARGS=%STOP_CMD_ARGS% "%~1""
shift
goto setStopArgs
:doneStopArgs
rem
%BUILD_DIR%\svcbatch.exe stop "%SERVICE_NAME%" /wait=30 %STOP_CMD_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo %_NX%: Stopped %SERVICE_NAME%
goto End
rem
:doBreak
rem
rem
rem sc control "%SERVICE_NAME%" 233
rem
pushd "..\.build\dbg"
set "BUILD_DIR=%cd%"
popd
%BUILD_DIR%\svcbatch.exe control "%SERVICE_NAME%" 233
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
:doRotate
rem
rem
rem sc control "%SERVICE_NAME%" 234
rem
pushd "..\.build\dbg"
set "BUILD_DIR=%cd%"
popd
%BUILD_DIR%\svcbatch.exe control "%SERVICE_NAME%" 234
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
:doDelete
rem
rem
pushd "..\.build\dbg"
set "BUILD_DIR=%cd%"
popd
rem
echo %~nx0: Deleting %SERVICE_NAME%
rem
%BUILD_DIR%\svcbatch.exe delete "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
echo %~nx0: Deleted %SERVICE_NAME%
goto End
rem
:doRemove
rem
rem
rd /S /Q "Logs" >NUL 2>&1
echo %~nx0: Removed %SERVICE_NAME%
goto End
rem
:Failed
sc delete "%SERVICE_NAME%" >NUL 2>&1
exit /B 1
rem
rem
:End
exit /B 0
