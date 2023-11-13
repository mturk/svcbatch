@echo off
rem
rem ---------------------------------------------------------------
rem JBoss EAP Service Management Tool
rem ---------------------------------------------------------------
rem
setlocal
rem
rem
set "EXECUTABLE=svcbatch.exe"
set "WINSERVICE=%~nx0"
rem
set "DEFAULT_SERVICE_SHELL=cmd"
set "SERVICE_SHELL=%DEFAULT_SERVICE_SHELL%"
rem
rem Set default service name and server mode
rem Change those variables to actual JBoss EAP version
set "DEFAULT_SERVICE_NAME=JBossEAP74"
set "SERVICE_NAME=%DEFAULT_SERVICE_NAME%"
rem
rem
set "SERVICE_DISPLAY=JBoss EAP 7.4.11 Service"
set "SERVICE_DESCIPTION=JBoss EAP continuous delivery - Version 7.4.11"
rem
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
rem
if "x%~1x" == "xx" goto doneSetArgs
if /i "%SERVICE_CMD%" neq "create" goto setSvcName
set "SERVICE_SHELL=%~1"
shift
rem
:setSvcName
if "x%~1x" == "xx" goto doneSetArgs
rem Set service name
set "SERVICE_NAME=%~1"
shift
rem
rem
:setArgs
if "x%~1x" == "xx" goto doneSetArgs
rem Append argument
set "CMD_LINE_ARGS=%CMD_LINE_ARGS% "%~1""
shift
rem
goto setArgs
rem
:doneSetArgs
rem
rem Process the requested command
rem
if /i "%SERVICE_CMD%" == "create"   goto doCreate
if /i "%SERVICE_CMD%" == "delete"   goto doDelete
if /i "%SERVICE_CMD%" == "start"    goto doStart
if /i "%SERVICE_CMD%" == "stop"     goto doStop
rem
rem
echo Unknown command "%SERVICE_CMD%"
rem
:displayUsage
rem
echo.
echo Usage: %WINSERVICE% command [shell] [service_name] [arguments ...]
echo commands:
echo   create            Create the service
echo   delete            Delete the service
echo   start             Start the service
echo   stop              Stop the service
rem
exit /B 1
rem
rem
rem Create Service
rem
:doCreate
rem Set common create command options
rem
rem Set the server mode
set "SERVER_MODE=standalone"
rem
rem Set the log name
set "SERVICE_LOGNAME=/LN:service.log"
rem
rem set "SERVICE_LOGNAME=/LN:service.@Y-@m-@d.log /SN:service.stop.log /SM:1"
rem
if /i "%SERVICE_SHELL%" == "cmd" goto doCreateCmd
if /i "%SERVICE_SHELL%" == "ps"  goto doCreatePs
rem
echo Unknown shell "%SERVICE_SHELL%"
echo.
echo Usage: %WINSERVICE% create cmd^|ps [service_name] [arguments ...]
rem
exit /B 1
rem
rem
rem Create service using cmd.exe
rem
:doCreateCmd
rem
%EXECUTABLE% create "%SERVICE_NAME%" ^
    --displayName "%SERVICE_DISPLAY%" ^
    --description "%SERVICE_DESCIPTION%" ^
    --start=automatic ^
    /F:P /E:NOPAUSE=Y ^
    /L:..\%SERVER_MODE%\log %SERVICE_LOGNAME% ^
    /S:jboss-cli.bat [ --controller=127.0.0.1:9990 --connect --command=:shutdown ] ^
    %SERVER_MODE%.bat %CMD_LINE_ARGS%
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
rem Create service using powershell
rem
:doCreatePs
rem
rem
%EXECUTABLE% create "%SERVICE_NAME%" ^
    --displayName "%SERVICE_DISPLAY%" ^
    --description "%SERVICE_DESCIPTION%" ^
    --start=auto ^
    /F:P ^
    /L:..\%SERVER_MODE%\log %SERVICE_LOGNAME% ^
    /C:powershell [ -NoProfile -ExecutionPolicy Bypass -File ] ^
    /S:jboss-cli.ps1 [ --controller=127.0.0.1:9990 --connect --command=:shutdown ] ^
    %SERVER_MODE%.ps1 %CMD_LINE_ARGS%
rem
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
