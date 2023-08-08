@echo off
rem
rem ---------------------------------------------------------------
rem JBoss EAP Service Management Tool
rem
rem Usage: servicemgr.bat create/createps/delete/rotate/dump/start/stop
rem
rem ---------------------------------------------------------------
rem
setlocal
rem
rem Change to actual JBoss EAP version
set "SERVICE_NAME=jbosseap74"
set "SERVICE_DISPLAY=JBoss EAP 7.4.4 Service"
set "SERVICE_DESCIPTION=JBoss EAP continuous delivery - Version 7.4.4"
set "JBOSSEAP_SERVER_MODE=standalone"
rem
rem
if /i "x%~1" == "xcreate"   goto doCreate
if /i "x%~1" == "xcreateps" goto doCreatePs
if /i "x%~1" == "xdelete"   goto doDelete
if /i "x%~1" == "xdump"     goto doDumpStacks
if /i "x%~1" == "xrotate"   goto doRotate
if /i "x%~1" == "xstart"    goto doStart
if /i "x%~1" == "xstop"     goto doStop
rem
echo Unknown command '%~1'
echo.
echo Usage: %~nx0 ( commands ... ) [service_name]
echo commands:
echo   create            Create the service
echo   createps          Create the service using powershell
echo   delete            Delete the service
echo   dump              Dump JVM full thread stack to the log file
echo   rotate            Rotate log files
echo   start             Start the service
echo   stop              Stop the service
rem
exit /B 1
rem
rem
rem Create service
:doCreate
rem
shift
set CMD_LINE_ARGS=
:setArgs
if "x%~1" == "x" goto doneArgs
set "CMD_LINE_ARGS=%CMD_LINE_ARGS% %~1"
shift
goto setArgs
rem
:doneArgs
rem
set "SERVICE_BATCH_FILE=%JBOSSEAP_SERVER_MODE%.bat"
rem
rem set "SERVICE_BATCH_FILE=winservice.bat"
rem
svcbatch create "%SERVICE_NAME%" /display "%SERVICE_DISPLAY%" ^
    /description "%SERVICE_DESCIPTION%" ^
    /start:automatic /nservice /eNOPAUSE=Y ^
    /sjboss-cli.bat "/s'--controller=127.0.0.1:9990 --connect --command=:shutdown'" ^
    /vLpB -rS /o..\%JBOSSEAP_SERVER_MODE%\log %SERVICE_BATCH_FILE% ^
    %CMD_LINE_ARGS%
rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem Run service using powershell
:doCreatePs
rem
shift
set CMD_LINE_ARGS=
:setPsArgs
if "x%~1" == "x" goto donePsArgs
set "CMD_LINE_ARGS=%CMD_LINE_ARGS% %~1"
shift
goto setPsArgs
rem
:donePsArgs
rem
rem
svcbatch create "%SERVICE_NAME%" /display "%SERVICE_DISPLAY%" ^
    /description "%SERVICE_DESCIPTION%" ^
    /start:automatic ^
    /vLpB -rS /o..\%JBOSSEAP_SERVER_MODE%\log /nservice ^
    /sjboss-cli.ps1 "/s'--controller=127.0.0.1:9990 --connect --command=:shutdown'" ^
    /cpowershell "/c-NoProfile -ExecutionPolicy Bypass -File" ^
    %JBOSSEAP_SERVER_MODE%.ps1 %CMD_LINE_ARGS%

rem
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem Start service
:doStart
rem
svcbatch start "%SERVICE_NAME%" /wait
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
rem Stop service
:doStop
rem
svcbatch stop "%SERVICE_NAME%" /wait
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
rem Delete service
:doDelete
rem
svcbatch delete "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
rem Rotate SvcBatch logs
:doRotate
rem
svcbatch control "%SERVICE_NAME%" 234
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
goto End
rem
rem
rem Send CTRL_BREAK_EVENT
rem Jvm will dump full thread stack to log file
:doDumpStacks
rem
svcbatch control "%SERVICE_NAME%" 233
if %ERRORLEVEL% neq 0 exit /B %ERRORLEVEL%
rem
rem
rem
:End
exit /B 0
