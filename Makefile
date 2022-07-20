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
!INCLUDE <Version.mk>

VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH).$(VERSION_MICRO)

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
EXEVER = $(VERSION_MAJOR).$(VERSION_MINOR)

CFLAGS = -DNDEBUG -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER) -DWIN32_LEAN_AND_MEAN
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_DEPRECATE
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE
CFLAGS = $(CFLAGS) -DVERSION_MAJOR=$(VERSION_MAJOR) -DVERSION_MINOR=$(VERSION_MINOR) -DVERSION_PATCH=$(VERSION_PATCH)
CFLAGS = $(CFLAGS) -DVERSION_MICRO=$(VERSION_MICRO) -DVERSION_DEVEL=$(VERSION_DEVEL)

CLOPTS = /c /nologo $(CRT_CFLAGS) -W4 -O2 -Ob2
RCOPTS = /l 0x409 /n
RFLAGS = /d NDEBUG /d WINVER=$(WINVER) /d _WIN32_WINNT=$(WINVER)
RFLAGS = $(RFLAGS) /d VERSION_MAJOR=$(VERSION_MAJOR) /d VERSION_MINOR=$(VERSION_MINOR) /d VERSION_PATCH=$(VERSION_PATCH)
RFLAGS = $(RFLAGS) /d VERSION_MICRO=$(VERSION_MICRO) /d VERSION_DEVEL=$(VERSION_DEVEL)

LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:$(_CPU) /VERSION:$(EXEVER)
LDLIBS = kernel32.lib advapi32.lib user32.lib $(EXTRA_LIBS)

!IF DEFINED(VSCMD_VER)
RCOPTS = /nologo $(RCOPTS)
!ENDIF

!IF DEFINED(VERSION_SFX)
CFLAGS = $(CFLAGS) -DVERSION_SFX=$(VERSION_SFX)
RFLAGS = $(RFLAGS) /d VERSION_SFX=$(VERSION_SFX)
!ENDIF

!IF DEFINED(_BUILD_TIMESTAMP)
CFLAGS = $(CFLAGS) -D_BUILD_TIMESTAMP=$(_BUILD_TIMESTAMP)
RFLAGS = $(RFLAGS) /d _BUILD_TIMESTAMP=$(_BUILD_TIMESTAMP)
!ENDIF

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj \
	$(WORKDIR)\$(PROJECT).res

all : $(WORKDIR) $(OUTPUT)

$(WORKDIR):
	@-md $(WORKDIR) 2>NUL

$(WORKDIR)\$(PROJECT).manifest: $(PPREFIX).manifest.in
	<<ssed.bat
	@echo off
	setlocal
	(for /f "delims=" %%i in ($(PPREFIX).manifest.in) do (
	set "line=%%i"
	setlocal enabledelayedexpansion
	set "line=!line:@@version@@=$(VERSION)!"
	echo(!line!
	endlocal
	))>$@
<<

$(WORKDIR)\$(PROJECT).obj: $(PPREFIX).h $(PPREFIX).c
	$(CC) $(CLOPTS) $(CFLAGS) -I$(SRCDIR) -Fo$(WORKDIR)\ $(PPREFIX).c

$(WORKDIR)\$(PROJECT).res: $(PPREFIX).h $(PPREFIX).rc $(WORKDIR)\$(PROJECT).manifest
	$(RC) $(RCOPTS) $(RFLAGS) /i $(SRCDIR) /i $(WORKDIR) /fo $@ $(PPREFIX).rc

$(OUTPUT): $(WORKDIR) $(OBJECTS)
	$(LN) $(LFLAGS) $(OBJECTS) $(LDLIBS) /out:$(OUTPUT)

clean:
	@-rd /S /Q $(WORKDIR) 2>NUL
