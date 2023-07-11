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
rem Set JAVA_HOME and JRE_HOME to your JDK installation
rem
set "JAVA_HOME=%JDK_8_HOME%"
set "JRE_HOME=%JRE_8_HOME%"
rem
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
echo.
rem
rem echo %~nx0: System Information
rem echo.
rem Display machine specific properties and configuration
rem systeminfo
rem chcp
rem echo.
rem echo %~nx0: Environment Variables
rem echo.
rem Display environment variables
rem set
rem echo.
rem echo.
rem Run JBoss EAP
call standalone.bat %*
rem
