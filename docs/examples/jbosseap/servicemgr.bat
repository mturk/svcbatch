@echo off
rem
rem ---------------------------------------------------------------
rem JBoss EAP Service Management Tool
rem
rem Usage: servicemgr.bat create/delete/rotate/dump/start/stop
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
if /i "x%~1" == "xcreate" goto doCreate
if /i "x%~1" == "xdelete" goto doDelete
if /i "x%~1" == "xdump"   goto doDumpStacks
if /i "x%~1" == "xrotate" goto doRotate
if /i "x%~1" == "xstart"  goto doStart
if /i "x%~1" == "xstop"   goto doStop
rem Unknown option
echo %~nx0: Unknown option '%~1'
goto Einval

rem
rem Create service
:doCreate
set "SERVICE_BASE=%cd%"
pushd ..
rem Location for Logs\SvcBatch.log[.n] files
set "SERVICE_RUNTIME_DIR=%cd%\%JBOSSEAP_SERVER_MODE%"
popd
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
sc create "%SERVICE_NAME%" binPath= "\"%SERVICE_BASE%\svcbatch.exe\" -o \"%SERVICE_RUNTIME_DIR%\log\" -bdp %JBOSSEAP_SERVER_MODE%.bat %CMD_LINE_ARGS%"
sc config "%SERVICE_NAME%" DisplayName= "%SERVICE_DISPLAY%"

rem Ensure the networking services are running
rem and that service is started on system startup
sc config "%SERVICE_NAME%" depend= Tcpip/Afd start= auto

rem Set required privileges so we can kill process tree
rem even if Jvm created multiple child processes.
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
rem Description is from JBoss EAP distribution version.txt file
sc description "%SERVICE_NAME%" "%SERVICE_DESCIPTION%"
rem
goto End

rem
rem Start service
:doStart
rem
sc start "%SERVICE_NAME%"
goto End

rem
rem Stop service
:doStop
rem
sc stop "%SERVICE_NAME%"
goto End

rem
rem Delete service
:doDelete
rem
sc stop "%SERVICE_NAME%" >NUL
ping -n 6 localhost >NUL
sc delete "%SERVICE_NAME%"
goto End

rem
rem Rotate SvcBatch logs
:doRotate
rem
sc control "%SERVICE_NAME%" 234
goto End

rem
rem Send CTRL_BREAK_EVENT
rem Jvm will dump full thread stack to log file
:doDumpStacks
rem
sc control "%SERVICE_NAME%" 233
goto End

:Einval
echo Usage: %~nx0 create/delete/rotate/dump/start/stop
echo.
exit /b 1

:End
