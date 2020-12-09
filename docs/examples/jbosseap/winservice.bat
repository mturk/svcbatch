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
echo.
rem Run JBoss EAP standalone
call standalone.bat
rem Instead calling standalone.bat you can call
rem call domain.bat
rem

