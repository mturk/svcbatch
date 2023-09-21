@echo off
rem
rem --------------------------------------------------
rem JBoss EAP Service script
rem
rem This script can be executed by SvcBatch
rem instead default %SERVER_MODE%.bat file
rem
rem --------------------------------------------------
rem
rem
rem Set JAVA_HOME and JRE_HOME to your JDK installation
rem
rem set "JAVA_HOME=%JDK_11_HOME%"
rem set "JRE_HOME=%JRE_11_HOME%"
rem
echo %~nx0: Running %SVCBATCH_NAME% Service
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
rem
rem Run JBoss EAP in Standalone mode
call standalone.bat %*
rem
