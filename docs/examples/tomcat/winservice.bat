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
rem
setlocal
rem
echo %~nx0: Running %SVCBATCH_SERVICE_NAME% Service
echo.
rem Set here any environment variables
rem that are missing from LOCAL_SERVICE account
rem
rem eg. set "JAVA_HOME=C:\Your\JDK\location"
rem     set "JRE_HOME=C:\Your\JRE\location"
rem
rem If you have created a separate copy using
rem     makebase.bat ..\nodes\01 -w
rem
rem     Set CATALINA_HOME and CATALINA_BASE variables
rem
rem     set "CATALINA_HOME=%SVCBATCH_SERVICE_HOME%"
rem     set "CATALINA_BASE=%SVCBATCH_SERVICE_HOME%\nodes\01"
rem
rem     When creating service add '-o nodes\01\logs'
rem     so that SvcBatch.log is created where
rem     the Tomcat logs will be created.
rem
rem Run Apache Tomcat
call "%SVCBATCH_SERVICE_BASE%\catalina.bat" run %*
rem

