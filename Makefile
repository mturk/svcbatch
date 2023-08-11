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

PROJECT = svcbatch
BLDARCH = x64
WINVER  = 0x0601

!IF DEFINED(_DEBUG) && "$(_DEBUG)" == ""
!ERROR _DEBUG variable cannot be empty. Use _DEBUG=1, _DEBUG=2, etc...
!ENDIF
!IF DEFINED(VERSION_SFX) && "$(VERSION_SFX)" == ""
!ERROR VERSION_SFX variable cannot be empty. Use VERSION_SFX=value
!ENDIF

WORKTOP = $(SRCDIR)\build
!IF DEFINED(_DEBUG)
WORKDIR = $(WORKTOP)\dbg
!ELSE
WORKDIR = $(WORKTOP)\rel
!ENDIF
PPREFIX = $(SRCDIR)\$(PROJECT)
POUTPUT = $(WORKDIR)\$(PROJECT).exe

SSERVICE = $(WORKDIR)\sservice.exe
XSLEEP   = $(WORKDIR)\xsleep.exe

!IF DEFINED(_STATIC_MSVCRT)
CRT_CFLAGS = -MT
EXTRA_LIBS =
!ELSE
CRT_CFLAGS = -MD
!ENDIF
!IF DEFINED(_DEBUG)
CRT_CFLAGS = $(CRT_CFLAGS)d
!ENDIF

CFLAGS = -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER) -DWIN32_LEAN_AND_MEAN
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_DEPRECATE
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE

CLOPTS = /c /nologo $(CRT_CFLAGS) -W4 -O2 -Ob2 -GF -Gs0
LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:$(BLDARCH) /STACK:0x200000

!IF DEFINED(_DEBUG)
CFLAGS = $(CFLAGS) -D_DEBUG=$(_DEBUG)
CLOPTS = $(CLOPTS) -Zi
RFLAGS = /d _DEBUG=$(_DEBUG)
LFLAGS = $(LFLAGS) /DEBUG
!ELSE
CFLAGS = $(CFLAGS) -DNDEBUG
RFLAGS = /d NDEBUG
!IF DEFINED(_STATIC_MSVCRT) && "$(_STATIC_MSVCRT)" == "Hybrid"
LFLAGS = $(LFLAGS) /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib
!ENDIF
!ENDIF

RCOPTS = /l 0x409 /n
RFLAGS = $(RFLAGS) /d WINVER=$(WINVER) /d _WIN32_WINNT=$(WINVER)

LDLIBS = kernel32.lib advapi32.lib bcrypt.lib $(EXTRA_LIBS)

!IF DEFINED(VSCMD_VER)
RCOPTS = /nologo $(RCOPTS)
!ENDIF

!IF DEFINED(VERSION_SFX)
CFLAGS = $(CFLAGS) -DVERSION_SFX=$(VERSION_SFX)
RFLAGS = $(RFLAGS) /d VERSION_SFX=$(VERSION_SFX)
!ENDIF

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj \
	$(WORKDIR)\$(PROJECT).res


TESTAPPS = \
	$(SRCDIR)\test\sservice \
	$(SRCDIR)\test\xsleep

all : $(WORKDIR) $(POUTPUT)

$(WORKDIR):
	@-md $(WORKDIR) 2>NUL

{$(SRCDIR)}.c{$(WORKDIR)}.obj:
	$(CC) $(CLOPTS) $(CFLAGS) -Fd$(WORKDIR)\$(PROJECT) -Fo$(WORKDIR)\ $<

{$(SRCDIR)}.rc{$(WORKDIR)}.res:
	$(RC) $(RCOPTS) $(RFLAGS) /fo $@ $<

$(POUTPUT): $(WORKDIR) $(OBJECTS)
	$(LN) $(LFLAGS) /pdb:$(WORKDIR)\$(PROJECT).pdb /out:$(POUTPUT) $(OBJECTS) $(LDLIBS)

$(TESTAPPS): $(POUTPUT)
	@cd $@
  @$(MAKE) /L$(MAKEFLAGS)
  @cd $(MAKEDIR)

tests: $(TESTAPPS)

clean:
	@-rd /S /Q $(WORKDIR) 2>NUL

distclean:
	@-rd /S /Q $(WORKTOP) 2>NUL
