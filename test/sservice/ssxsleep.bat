@echo off
rem
rem Licensed under the Apache License, Version 2.0 (the "License");
rem you may not use this file except in compliance with the License.
rem You may obtain a copy of the License at
rem
rem     http://www.apache.org/licenses/LICENSE-2.0
rem
rem Unless required by applicable law or agreed to in writing, software
rem distributed under the License is distributed on an "AS IS" BASIS,
rem WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
rem See the License for the specific language governing permissions and
rem limitations under the License.
rem
rem Run xsleep.exe
rem
rem
setlocal
rem
rem
echo %~nx0: Running inside %SVCBATCH_SERVICE_NAME% Service
echo %~nx0: Sleep interval [%~1]
echo.
rem
echo %~nx0: [%TIME%] ... Start > %SVCBATCH_SERVICE_WORK%\%SVCBATCH_SERVICE_NAME%.%SVCBATCH_SERVICE_UUID%.%~nx0.log
rem
start /wait xsleep.exe %~1
rem
echo %~nx0: [%TIME%] ... Done >> %SVCBATCH_SERVICE_WORK%\%SVCBATCH_SERVICE_NAME%.%SVCBATCH_SERVICE_UUID%.%~nx0.log
rem
exit /b 0
