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
rem Set Apache Tomcat Service environment
rem --------------------------------------------------
rem
rem
rem Set here any environment variables
rem that are missing from the LOCAL_SERVICE account
rem
rem eg. set "JAVA_HOME=C:\Your\JDK\location"
rem     set "JRE_HOME=C:\Your\JRE\location"
rem
rem     set "JAVA_HOME=%JDK_17_HOME%"
rem     set "JRE_HOME=%JRE_17_HOME%"
rem
rem If you have created a separate copy using
rem     makebase.bat ..\nodes\01 -w
rem
rem
rem     When creating service add '/wnodes\01'
rem     as argument to service.bat
rem     so that SVCBATCH_WORK points to the
rem     correct location where the log files will be created.
rem
rem     For example:
rem     service.bat create Tomcat11Node1 /wnodes\01
rem
rem Set CATALINA_HOME and CATALINA_BASE variables
rem
set "CATALINA_HOME=%SVCBATCH_HOME%"
set "CATALINA_BASE=%SVCBATCH_WORK%"
rem
