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

BLDARCH = x64
PROJECT = xsleep

WORKTOP = $(SRCDIR)\..\..\.build
!IF DEFINED(_DEBUG)
WORKDIR = $(WORKTOP)\dbg
!ELSE
WORKDIR = $(WORKTOP)\rel
!ENDIF

POUTPUT = $(WORKDIR)\$(PROJECT).exe

!IF DEFINED(_STATIC_MSVCRT)
CRT_CFLAGS = -MT
EXTRA_LIBS =
!ELSE
CRT_CFLAGS = -MD
!ENDIF
!IF DEFINED(_DEBUG)
CRT_CFLAGS = $(CRT_CFLAGS)d
!ENDIF

WINVER = 0x0601
EXEVER = 2.0

CFLAGS = -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER) -DWIN32_LEAN_AND_MEAN
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_DEPRECATE
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE

CLOPTS = /c /nologo $(CRT_CFLAGS) -W4 -O2 -Ob2
LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:$(BLDARCH) /VERSION:$(EXEVER)

!IF DEFINED(_DEBUG)
CFLAGS = $(CFLAGS) -D_DEBUG=$(_DEBUG)
CLOPTS = $(CLOPTS) -Zi
LFLAGS = $(LFLAGS) /DEBUG
!ELSE
CFLAGS = $(CFLAGS) -DNDEBUG
!ENDIF

LDLIBS = kernel32.lib

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj

all : $(POUTPUT)

{$(SRCDIR)}.c{$(WORKDIR)}.obj:
	$(CC) $(CLOPTS) $(CFLAGS) -Fd$(WORKDIR)\$(PROJECT) -Fo$(WORKDIR)\ $<

$(POUTPUT): $(OBJECTS)
	$(LN) $(LFLAGS) /pdb:$(WORKDIR)\$(PROJECT).pdb /out:$(POUTPUT) $(OBJECTS) $(LDLIBS)
