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
rem Dummy file executed on Service Stop
rem
rem
setlocal
rem
echo %~nx0: Called from %SVCBATCH_SERVICE_NAME% Service
echo.
rem Dump environment variables to SvcBatch.shutdown.log file
set
echo.
rem
:doRepeat
rem
echo %~nx0: [%TIME%] ... running
rem Simulate some work by sleeping for 5 seconds
ping -n 6 localhost >NUL
rem Stalled Shutdown simulation
goto doRepeat
rem
:End
echo %~nx0: [%TIME%] ... done
exit /B 0