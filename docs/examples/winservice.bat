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
rem Apache Tomcat Service script
rem
rem Usage: winservice.bat create/delete [service_name]
rem
setlocal
if /i "x%~1" == "xcreate" goto doCreate
if /i "x%~1" == "xdelete" goto doDelete
if not "x%~1" == "x" goto Einval
rem
rem This section is run by SvcBatch
rem
rem Set JAVA_HOME or JRE_HOME to desired JDK/JRE
set "JAVA_HOME=c:\Tools\jdk-15.0.1"
echo %~nx0: running %SVCBATCH_SERVICE_NAME% service
echo.
rem Call catalina.bat
call catalina.bat run
rem
goto End
rem
rem Create service using sc.exe
:doCreate
if "x%~2" == "x" goto Einval
set "SERVICE_NAME=%~2"
set "SERVICE_BASE=%cd%"
pushd ..
set "SERVICE_HOME=%cd%"
popd
rem
rem Change to actual version
set "TOMCAT_DISPLAY=Apache Tomcat 10.0"
set "TOMCAT_FULLVER=Apache Tomcat 10.0.0"
rem
sc create "%SERVICE_NAME%" binPath= ""%SERVICE_BASE%\svcbatch.exe" /w "%SERVICE_HOME%" /s /c .\bin\%~nx0"
sc config "%SERVICE_NAME%" DisplayName= "%TOMCAT_DISPLAY% %SERVICE_NAME% Service"
sc config "%SERVICE_NAME%" depend= Tcpip/Afd
sc privs "%SERVICE_NAME%" SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
sc description "%SERVICE_NAME%" "%TOMCAT_FULLVER% Server - https://tomcat.apache.org/"
goto End
rem
rem Delete service
:doDelete
if "x%~2" == "x" goto Einval
set "SERVICE_NAME=%~2"
rem
sc stop "%SERVICE_NAME%" >NUL
sc delete "%SERVICE_NAME%"
goto End

:Einval
echo %nx0: Invalid parameter
echo.
exit /b 1

:End
