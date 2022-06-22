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

_CPU = x64

PROJECT = svcbatch
PPREFIX = $(SRCDIR)\$(PROJECT)
WORKDIR = $(SRCDIR)\$(_CPU)
OUTPUT  = $(WORKDIR)\$(PROJECT).exe

!IF DEFINED(_STATIC_MSVCRT)
CRT_CFLAGS = -MT
EXTRA_LIBS =
!ELSE
CRT_CFLAGS = -MD
!ENDIF

WINVER = 0x0601
EXEVER = 1.2

CFLAGS = -DNDEBUG -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER) -DWIN32_LEAN_AND_MEAN
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_DEPRECATE
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE
CLOPTS = /c /nologo $(CRT_CFLAGS) -W4 -O2 -Ob2
RCOPTS = /l 0x409 /n
RFLAGS = /d NDEBUG /d WINVER=$(WINVER) /d _WIN32_WINNT=$(WINVER) $(EXTRA_RFLAGS)
LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:$(_CPU) /VERSION:$(EXEVER) $(EXTRA_LFLAGS)
LDLIBS = kernel32.lib advapi32.lib $(EXTRA_LIBS)

!IF DEFINED(_DBGVIEW)
CFLAGS  = $(CFLAGS) -D_DBGVIEW
!ENDIF
!IF DEFINED(EXTRA_CFLAGS)
CFLAGS  = $(CFLAGS) $(EXTRA_CFLAGS)
!ENDIF

!IF DEFINED(VSCMD_VER)
RCOPTS = /nologo $(RCOPTS)
!ENDIF

!IF DEFINED(_VENDOR_SFX)
CFLAGS = $(CFLAGS) -D_VENDOR_SFX=$(_VENDOR_SFX)
RFLAGS = $(RFLAGS) /d _VENDOR_SFX=$(_VENDOR_SFX)
!ENDIF
!IF DEFINED(_VENDOR_NUM)
RFLAGS = $(RFLAGS) /d _VENDOR_NUM=$(_VENDOR_NUM)
!ENDIF

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj \
	$(WORKDIR)\$(PROJECT).res

all : $(WORKDIR) $(OUTPUT)

$(WORKDIR):
	@-md $(WORKDIR) 2>NUL

$(WORKDIR)\$(PROJECT).obj: $(PPREFIX).h $(PPREFIX).c
	$(CC) $(CLOPTS) $(CFLAGS) -I$(SRCDIR) -Fo$(WORKDIR)\ $(PPREFIX).c

$(WORKDIR)\$(PROJECT).res: $(PPREFIX).h $(PPREFIX).rc
	$(RC) $(RCOPTS) $(RFLAGS) /i $(SRCDIR) /fo $@ $(PPREFIX).rc

$(OUTPUT): $(WORKDIR) $(OBJECTS)
	$(LN) $(LFLAGS) $(OBJECTS) $(LDLIBS) /out:$(OUTPUT)

clean:
	@-rd /S /Q $(WORKDIR) 2>NUL
