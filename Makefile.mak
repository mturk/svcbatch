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
# Originally contributed by Mladen Turk <mturk apache.org>
#
#

CC = cl.exe
LN = link.exe
RC = rc.exe

PROJECT = svcbatch
WORKDIR = x64
OUTPUT  = $(WORKDIR)\$(PROJECT).exe

!IF DEFINED(_DBGVIEW)
EXTRA_CFLAGS = -D_DBGVIEW $(EXTRA_CFLAGS)
!ENDIF

WINVER = 0x0601
CFLAGS = -DNDEBUG -DWIN32 -DWIN64 -D_WIN32_WINNT=$(WINVER) -DWINVER=$(WINVER)
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_DEPRECATE -DUNICODE -D_UNICODE $(EXTRA_CFLAGS)
CLOPTS = /c /nologo -MD -W4 -O2 -Ob2 -EHsc
RFLAGS = /l 0x409 /n /d NDEBUG $(EXTRA_RFLAGS)
LFLAGS = /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:X64 $(EXTRA_LFLAGS)
LDLIBS = kernel32.lib advapi32.lib user32.lib $(EXTRA_LIBS)

OBJECTS = \
	$(WORKDIR)\$(PROJECT).obj \
	$(WORKDIR)\$(PROJECT).res

all : $(WORKDIR) $(OUTPUT)

$(WORKDIR):
	@-md $(WORKDIR)

.c{$(WORKDIR)}.obj:
	$(CC) $(CLOPTS) $(CFLAGS) -Fo$(WORKDIR)\ $<

.rc{$(WORKDIR)}.res:
	$(RC) $(RFLAGS) /fo $@ $<

$(OUTPUT): $(WORKDIR) $(OBJECTS)
	$(LN) $(LFLAGS) $(OBJECTS) $(LDLIBS) /out:$(OUTPUT)

!IF !DEFINED(PREFIX) || "$(PREFIX)" == ""
install:
	@echo PREFIX is not defined
	@echo Use `nmake install PREFIX=directory`
	@echo.
	@exit /B 1
!ELSE
install : all
	@xcopy /I /Y /Q "$(WORKDIR)\*.exe" "$(PREFIX)"
!ENDIF

clean:
	@-rd /S /Q $(WORKDIR) 2>NUL
