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
SRCDIR  = .
PROJECT = svcbatch
PPREFIX = $(SRCDIR)\$(PROJECT)
WORKDIR = $(SRCDIR)\x64
OUTPUT  = $(WORKDIR)\$(PROJECT).exe

!IF DEFINED(_DBGVIEW)
EXTRA_CFLAGS = -D_DBGVIEW $(EXTRA_CFLAGS)
!ENDIF
!IF DEFINED(_DBGSAVE)
EXTRA_CFLAGS = -D_DBGSAVE $(EXTRA_CFLAGS)
!ENDIF
!IF DEFINED(_STATIC_MSVCRT)
CRT_CFLAGS = -MT
!ELSE
CRT_CFLAGS = -MD
!ENDIF

WINVER = 0x0601
CFLAGS = -DNDEBUG -DWIN32 -DWIN64 -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER)
CFLAGS = $(CFLAGS) -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_DEPRECATE -DUNICODE -D_UNICODE $(EXTRA_CFLAGS)
CLOPTS = /c /nologo $(CRT_CFLAGS) -W4 -O2 -Ob2
RFLAGS = /l 0x409 /n /d NDEBUG /d WIN32 /d WIN64 /d WINVER=$(WINVER)
RFLAGS = $(RFLAGS) /d _WIN32_WINNT=$(WINVER) $(EXTRA_RFLAGS)
LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:X64 $(EXTRA_LFLAGS)
LDLIBS = kernel32.lib advapi32.lib user32.lib

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj \
	$(WORKDIR)\$(PROJECT).res

all : $(WORKDIR) $(OUTPUT)

$(WORKDIR):
	@-md $(WORKDIR)

$(WORKDIR)\$(PROJECT).obj: $(PPREFIX).h
	$(CC) $(CLOPTS) $(CFLAGS) -I$(SRCDIR) -Fo$(WORKDIR)\ $(PPREFIX).c

$(WORKDIR)\$(PROJECT).res: $(PPREFIX).h
	$(RC) $(RFLAGS) /i $(SRCDIR) /fo $@ $(PPREFIX).rc

$(OUTPUT): $(WORKDIR) $(OBJECTS)
	$(LN) $(LFLAGS) $(OBJECTS) $(LDLIBS) /out:$(OUTPUT)

clean:
	@-rd /S /Q $(WORKDIR) 2>NUL
