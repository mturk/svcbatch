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
rem Batch file that never returns
rem
rem
setlocal
rem Set active code page to 65001 (utf-8)
chcp 65001 1>NUL
rem
echo %~nx0: Started
echo %~nx0: Arguments [%*]
rem
echo.
echo %~nx0: System Information
echo.
rem Display machine specific properties and configuration
systeminfo
chcp
echo.
echo %~nx0: Environment Variables
echo.
rem Display environment variables
set
echo.
echo.
rem
:doMain
rem Set running counter
set /A _xx=0
rem
:doRun
rem Set working counter
set /A _cc=0
echo.
echo.
echo %~nx0: [%_xx%] [%TIME%] ... running
echo.
rem
:doWork
rem
echo %~nx0: [%_cc%] [%TIME%] ... working %RANDOM%
ping -n 3 127.0.0.1 >NUL
rem
set /A _cc+=1
if %_cc% lss 10 (
    goto doWork
)
rem
rem Dump some lorem ipsum
echo.
echo %~nx0: [%_xx%] [%TIME%] ... dumping
echo.
echo Lorem ipsum dolor sit amet, consectetur adipiscing elit,
echo sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.
echo Ut enim ad minim veniam, quis nostrud exercitation ullamco
echo laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure
echo dolor in reprehenderit in voluptate velit esse cillum dolore eu
echo fugiat nulla pariatur. Excepteur sint occaecat cupidatat non
echo proident, sunt in culpa qui officia deserunt mollit anim id est laborum.
rem
ping -n 6 127.0.0.1 >NUL
rem Increment counter
set /A _xx+=1
if %_xx% lss 10 (
    goto doRun
)
rem
goto doMain
rem
rem Never returns
rem
exit /B 0
