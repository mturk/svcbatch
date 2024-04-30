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

CC = cl.exe
LN = link.exe
RC = rc.exe
SRCDIR = .

PROJECT = wxtime
BLDARCH = x64
WINVER  = 0x0601

WORKTOP = $(SRCDIR)\..\..\build
!IF DEFINED(DEBUG_BUILD)
WORKDIR = $(WORKTOP)\dbg
!ELSE
WORKDIR = $(WORKTOP)\rel
!ENDIF

POUTPUT = $(WORKDIR)\$(PROJECT).exe

CFLAGS = -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER) -DWIN32_LEAN_AND_MEAN
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE

CLOPTS = /c /nologo /W4 /O2 /Ob2 /Oi /GS- /Gs2097152
LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:$(BLDARCH)
LFLAGS = $(LFLAGS) /ENTRY:wxtimeMain /NODEFAULTLIB

LDLIBS = kernel32.lib


OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj

all : $(POUTPUT)

{$(SRCDIR)}.c{$(WORKDIR)}.obj:
	$(CC) $(CLOPTS) $(CFLAGS) -Fo$(WORKDIR)\ $<

$(POUTPUT): $(OBJECTS)
	$(LN) $(LFLAGS) /out:$(POUTPUT) $(OBJECTS) $(LDLIBS)

