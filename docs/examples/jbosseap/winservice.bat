@echo off
rem
rem --------------------------------------------------
rem JBoss EAP Service script
rem
rem This script is executed by SvcBatch
rem
rem --------------------------------------------------
rem
setlocal
rem
rem Set JAVA_HOME to your JDK installation
set "JAVA_HOME=C:\Java\java-11-openjdk-11.0.9.11-3.windows.redhat.x86_64"
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
rem Run JBoss EAP
if exist "%~dp0\winservice.conf.bat" (
   echo %~nx0: Found configuration file -- winservice.conf.bat
   call "%~dp0\winservice.conf.bat"
) else (
   echo %~nx0: No winservice.conf.bat file -- using standalone
   set "JBOSSEAP_SERVER_MODE=standalone"
)

echo.
rem
rem Call actual batch script
call %JBOSSEAP_SERVER_MODE%.bat
rem

