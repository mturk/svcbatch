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
PS = powershell.exe
SRCDIR = .

BLDARCH = x64
PROJECT = svcbatch
!INCLUDE <Version.mk>

VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH).$(VERSION_MICRO)
!IF DEFINED(_DEBUG)
WORKDIR = $(SRCDIR)\.build\msc\dbg
!ELSE
WORKDIR = $(SRCDIR)\.build\msc\rel
!ENDIF
PPREFIX = $(SRCDIR)\$(PROJECT)
OUTPUT  = $(WORKDIR)\$(PROJECT).exe
PIPELOG = $(WORKDIR)\pipedlog.exe

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
EXEVER = $(VERSION_MAJOR).$(VERSION_MINOR)

CFLAGS = -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER) -DWIN32_LEAN_AND_MEAN
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_DEPRECATE
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE

CLOPTS = /c /nologo $(CRT_CFLAGS) -W4 -O2 -Ob2
!IF DEFINED(_DEBUG)
CFLAGS = $(CFLAGS) -D_DEBUG=$(_DEBUG)
CLOPTS = $(CLOPTS) -Zi
RFLAGS = /d _DEBUG=$(_DEBUG)
!ELSE
CFLAGS = $(CFLAGS) -DNDEBUG
RFLAGS = /d NDEBUG
!ENDIF

RCOPTS = /l 0x409 /n
RFLAGS = $(RFLAGS) /d WINVER=$(WINVER) /d _WIN32_WINNT=$(WINVER)

LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:$(BLDARCH) /VERSION:$(EXEVER)
LDLIBS = kernel32.lib advapi32.lib user32.lib shell32.lib $(EXTRA_LIBS)

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

!IF DEFINED(_DEBUG)
CFLAGS = $(CFLAGS) -Fd$(WORKDIR)\$(PROJECT)
LFLAGS = $(LFLAGS) /DEBUG /pdb:$(WORKDIR)\$(PROJECT).pdb
!ENDIF

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj \
	$(WORKDIR)\$(PROJECT).res

PIPEOBJ = \
	$(WORKDIR)\pipedlog.obj


all : $(WORKDIR) $(OUTPUT)

$(WORKDIR):
	@-md $(WORKDIR) 2>NUL

$(WORKDIR)\$(PROJECT).manifest: $(PPREFIX).manifest.in
	$(PS) -NoProfile -ExecutionPolicy Bypass \
	"((Get-Content -Path $(PPREFIX).manifest.in -Raw) | Foreach-Object {$$_ \
	-replace '@@version@@','$(VERSION)' \
	}) |\
	Set-Content -Path $@"

$(WORKDIR)\$(PROJECT).h: $(PPREFIX).h.in
	$(PS) -NoProfile -ExecutionPolicy Bypass \
	"((Get-Content -Path $(PPREFIX).h.in -Raw) | Foreach-Object {$$_ \
	-replace '@@version_major@@','$(VERSION_MAJOR)' \
	-replace '@@version_minor@@','$(VERSION_MINOR)' \
	-replace '@@version_micro@@','$(VERSION_MICRO)' \
	-replace '@@version_patch@@','$(VERSION_PATCH)' \
	-replace '@@version_devel@@','$(VERSION_DEVEL)' \
	}) |\
	Set-Content -Path $@"

{$(SRCDIR)}.c{$(WORKDIR)}.obj:
	$(CC) $(CLOPTS) $(CFLAGS) -I$(SRCDIR) -I$(WORKDIR) -Fo$(WORKDIR)\ $<

{$(SRCDIR)\test\pipedlog}.c{$(WORKDIR)}.obj:
	$(CC) $(CLOPTS) $(CFLAGS) -I$(SRCDIR) -I$(WORKDIR) -Fo$(WORKDIR)\ $<

{$(SRCDIR)}.rc{$(WORKDIR)}.res:
	$(RC) $(RCOPTS) $(RFLAGS) /i $(SRCDIR) /i $(WORKDIR) /fo $@ $<

$(OUTPUT): $(WORKDIR) $(WORKDIR)\$(PROJECT).h $(WORKDIR)\$(PROJECT).manifest $(OBJECTS)
	$(LN) $(LFLAGS) /out:$(OUTPUT) $(OBJECTS) $(LDLIBS)

$(PIPELOG): $(OUTPUT) $(PIPEOBJ)
	$(LN) $(LFLAGS) /out:$(PIPELOG) $(PIPEOBJ) $(LDLIBS)

tests: $(PIPELOG)

clean:
	@-rd /S /Q $(WORKDIR) 2>NUL
