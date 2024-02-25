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

PROJECT = xsleep
BLDARCH = x64
WINVER  = 0x0601

WORKTOP = $(SRCDIR)\..\..\build
!IF DEFINED(DEBUG_BUILD)
WORKDIR = $(WORKTOP)\dbg
STATIC_MSVCRT = 1
!ELSE
WORKDIR = $(WORKTOP)\rel
!ENDIF

POUTPUT = $(WORKDIR)\$(PROJECT).exe

!IF DEFINED(STATIC_MSVCRT) || DEFINED(HYBRID_MSVCRT)
CRT_CFLAGS = -MT
!ELSE
CRT_CFLAGS = -MD
!ENDIF
!IF DEFINED(DEBUG_BUILD)
CRT_CFLAGS = $(CRT_CFLAGS)d
!ENDIF

CFLAGS = -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER) -DWIN32_LEAN_AND_MEAN
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_DEPRECATE
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE

CLOPTS = /c /nologo $(CRT_CFLAGS) -W4 -O2 -Ob2 -GF -Gs0
LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:$(BLDARCH)

!IF DEFINED(DEBUG_BUILD)
CFLAGS = $(CFLAGS) -DDEBUG -D_DEBUG
!ELSE
CFLAGS = $(CFLAGS) -DNDEBUG
!IF DEFINED(HYBRID_MSVCRT)
LFLAGS = $(LFLAGS) /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib
!ENDIF
!ENDIF

LDLIBS = kernel32.lib

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj

all : $(POUTPUT)

{$(SRCDIR)}.c{$(WORKDIR)}.obj:
	$(CC) $(CLOPTS) $(CFLAGS) -Fo$(WORKDIR)\ $<

$(POUTPUT): $(OBJECTS)
	$(LN) $(LFLAGS) /out:$(POUTPUT) $(OBJECTS) $(LDLIBS)

