# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Infinite loop

Write-Host "Running from $env:SVCBATCH_SERVICE_NAME service"

Write-Host ""
Write-Host "Arguments:"
foreach ($arg in $args) {
    Write-Host $arg
}

Write-Host ""
Write-Host "Starting infinite loop ..."
while($true)
{
    Start-Sleep -Seconds 5
    $i++
    Write-Host "Counter is $i"
}
