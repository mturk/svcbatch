@echo off
rem
rem ---------------------------------------------------------------
rem JBoss EAP Service Management Tool
rem ---------------------------------------------------------------
rem
rem
rem
set "EXECUTABLE=svcbatch.exe"
set "WINSERVICE=%~nx0"
rem
rem Set default service name and server mode
rem Change those variables to actual JBoss EAP version
set "DEFAULT_SERVICE_NAME=JBossEAP74"
set "SERVICE_NAME=%DEFAULT_SERVICE_NAME%"
rem
set "DEFAULT_SERVER_MODE=standalone"
set "SERVER_MODE=%DEFAULT_SERVER_MODE%"
rem
set "SERVICE_DISPLAY=JBoss EAP 7.4.11 Service"
set "SERVICE_DESCIPTION=JBoss EAP continuous delivery - Version 7.4.11"
rem
rem Parse the Arguments
rem
if "x%~1x" == "xx" goto displayUsage
rem
set "SERVICE_CMD=%~1"
shift
rem
set CMD_LINE_ARGS=
rem Process additional command arguments
if "x%~1x" == "xx" goto doneSetArgs
rem Set service name
set "SERVICE_NAME=%~1"
shift
rem
if /i "%SERVICE_CMD%" == "start" goto doneSetArgs
if /i "%SERVICE_CMD%" == "stop"  goto doneSetArgs
rem
if "x%~1x" == "xx" goto doneSetArgs
rem Set server mode
set "SERVER_MODE=%~1"
shift
rem
:setArgs
if "x%~1x" == "xx" goto doneSetArgs
set "CMD_LINE_ARGS=%CMD_LINE_ARGS% "%~1""
shift
goto setArgs
:doneSetArgs
rem
rem Process the requested command
rem
if /i "%SERVICE_CMD%" == "create"   goto doCreate
if /i "%SERVICE_CMD%" == "createps" goto doCreatePs
if /i "%SERVICE_CMD%" == "delete"   goto doDelete
if /i "%SERVICE_CMD%" == "dump"     goto doThreadDump
if /i "%SERVICE_CMD%" == "rotate"   goto doRotate
if /i "%SERVICE_CMD%" == "start"    goto doStart
if /i "%SERVICE_CMD%" == "stop"     goto doStop
rem
rem
echo Unknown command "%SERVICE_CMD%"
:displayUsage
echo.
echo Usage: %WINSERVICE% command [service_name] [server_mode] [arguments ...]
echo commands:
echo   create            Create the service
echo   createps          Create the service using powershell
echo   delete            Delete the service
echo   dump              Create Full JDK Thread Dump
echo   rotate            Rotate log files
echo   start             Start the service
echo   stop              Stop the service
rem
exit /B 1
rem
rem
rem
rem Create service
:doCreate
rem
rem
rem Set the log name
set "SERVICE_LOGNAME=-n:service.log"
rem
rem set "SERVICE_LOGNAME=-n:service.@Y-@m-@d.log/service.stop.log -m:.1"
rem
rem
rem
%EXECUTABLE% create "%SERVICE_NAME%" ^
    --display "%SERVICE_DISPLAY%" ^
    --description "%SERVICE_DESCIPTION%" ^
    --start:automatic ^
    -f:PCR -e:NOPAUSE=Y ^
    -o:..\%SERVER_MODE%\log %SERVICE_LOGNAME% ^
    -s:jboss-cli.bat [ --controller=127.0.0.1:9990 --connect --command=:shutdown ] ^
    %SERVER_MODE%.bat %CMD_LINE_ARGS%
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
rem Run service using powershell
:doCreatePs
rem
rem
rem Set the log name
set "SERVICE_LOGNAME=-n:service.log"
rem
rem set "SERVICE_LOGNAME=-n:service.@Y-@m-@d.log/service.stop.log -m:.1"
rem
%EXECUTABLE% create "%SERVICE_NAME%" ^
    --display "%SERVICE_DISPLAY%" ^
    --description "%SERVICE_DESCIPTION%" ^
    --start:auto ^
    -f:PCR ^
    -o:..\%SERVER_MODE%\log %SERVICE_LOGNAME% ^
    -c:powershell [ -NoProfile -ExecutionPolicy Bypass -File ] ^
    -s:jboss-cli.ps1 [ --controller=127.0.0.1:9990 --connect --command=:shutdown ] ^
    %SERVER_MODE%.ps1 %CMD_LINE_ARGS%

rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
:doThreadDump
rem
rem
%EXECUTABLE% control "%SERVICE_NAME%" 233
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doRotate
rem
rem
%EXECUTABLE% control "%SERVICE_NAME%" 234
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStart
rem
rem
%EXECUTABLE% start "%SERVICE_NAME%" -- %CMD_LINE_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doStop
rem
rem
%EXECUTABLE% stop "%SERVICE_NAME%" -- %CMD_LINE_ARGS%
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
:doDelete
rem
rem
%EXECUTABLE% delete "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
rem
rem
:End
exit /B 0
