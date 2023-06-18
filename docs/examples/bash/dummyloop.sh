#!/bin/bash
#
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
#
#
# sc create adummybash binPath= "\"%cd%\svcbatch.exe\" -vgl /c bash.exe /c \"--norc --noprofile\" -e \"PATH=@SystemDrive@\msys64\usr\bin;@PATH@\" dummyloop.sh"
#
echo "Running $SVCBATCH_SERVICE_NAME Service"
echo
#
# Print the current environment
echo "Environment variables:"
env
#
echo
echo
#
# Dumb infinite loop
while :
do
    echo "[`date +%Y%m%d%H%M%S`] ... running an infinite loop"
    sleep 2
done