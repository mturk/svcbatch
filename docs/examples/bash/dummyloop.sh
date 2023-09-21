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
# Simple SvcBatch script
#
#
if [ "x$1" = "xstop" ]
#
# Running as stop script
#
then
  echo "Running $SVCBATCH_NAME Stop"
  echo
  echo "[`date +%H:%M:%S`] Creating stop file"
  echo
  echo Y > logs/ss-$SVCBATCH_UUID
  sleep 1
  echo "[`date +%H:%M:%S`] Done"
  exit 0
fi
#
#
echo "Running $SVCBATCH_NAME Service"
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
echo "[`date +%H:%M:%S`] Starting infinite loop"
echo
#
while :
do
    echo "[`date +%H:%M:%S`] ... running"
    sleep 2
    if [ -f logs/ss-$SVCBATCH_UUID ]
    then
        echo
        echo "[`date +%H:%M:%S`] Stop file detected"
        sleep 1
        echo
        rm -v logs/ss-$SVCBATCH_UUID
        echo
        echo "[`date +%H:%M:%S`] Terminating"
        exit 0
    fi
done
