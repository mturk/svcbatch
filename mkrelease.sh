#!/bin/sh
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
# --------------------------------------------------
# SvcBatch release helper script
#
# Usage: mkrelease.sh [options] version [make arguments]
#    eg: mkrelease.sh 1.2.3
#        mkrelease.sh 1.2.3.45_1.acme VERSION_SFX=_1.acme VERSION_MICRO=45
#

eexit()
{
    e=$1; shift;
    echo "$@" 1>&2
    echo 1>&2
    echo "Usage: mkrelease.sh [options] version [make arguments]" 1>&2
    echo "   eg: mkrelease.sh -d 1.2.3_1 VERSION_MICRO=45" 1>&2
    exit $e
}

case "`uname -s`" in
  CYGWIN*)
    BuildHost=cygwin
  ;;
  MINGW*)
    BuildHost=mingw
  ;;
  *)
    echo "Unknown `uname`"
    echo "This scrip can run only inside cygwin or mingw"
    exit 1
  ;;

esac
#
ProjectName=svcbatch
ReleaseArch=win-x64-mingw
MakefileFlags="-f Makefile.gmk"
#
if [ "x$1" = "x-d" ]
then
  ReleaseArch=debug-$ReleaseArch
  BuildDir=.build/dbg
  MakefileFlags="$MakefileFlags _DEBUG=1"
  shift
else
  BuildDir=.build/rel
fi

ReleaseVersion=$1
test "x$ReleaseVersion" = "x" && eexit 1 "Missing version argument"
shift
ReleaseName=$ProjectName-$ReleaseVersion-$ReleaseArch
ReleaseLog=$ReleaseName.txt
ReleaseZip=$ReleaseName.zip
#
#
MakefileFlags="$MakefileFlags $*"
make $MakefileFlags clean
test "x$BuildHost" = "xcygwin" && MakefileFlags="$MakefileFlags USE_MINGW_PACKAGE_PREFIX=1"
make $MakefileFlags
#
pushd $BuildDir >/dev/null
echo "## Binary release v$ReleaseVersion" > $ReleaseLog
echo >> $ReleaseLog
echo '```no-highlight' >> $ReleaseLog
echo "Compiled on $BuildHost host using:" >> $ReleaseLog
echo "make $MakefileFlags" >> $ReleaseLog
echo "`make --version | head -1`" >> $ReleaseLog
echo "`gcc --version | head -1`" >> $ReleaseLog
echo >> $ReleaseLog
echo >> $ReleaseLog
#
7za a -bd $ReleaseZip $ProjectName.exe
echo "SHA256 hash of $ReleaseZip:" >> $ReleaseLog
sha256sum $ReleaseZip | sed 's;\ .*;;' >> $ReleaseLog
echo >> $ReleaseLog
echo '```' >> $ReleaseLog
#
popd >/dev/null

