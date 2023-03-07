## Simple service application for SvcBatch

This is basic example application that
simulates some typical application running
as service.

To build the application open Visual Studio
x64 native command prompt in this directory.

Inside command prompt type:

```cmd
> build.bat

cl.exe -c -nologo -W4 -O2 -Ob2 -MD -DWIN32 -DWIN64 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -D_CRT_SECURE_NO_DEPRECATE -Fosservice.obj sservice.c
sservice.c

link.exe /nologo /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE /MACHINE:X64 /out:sservice.exe sservice.obj kernel32.lib

```

Copy debug version of svacbatch.exe in this directory.

To install this as a service type

```cmd
>sservice.bat create
[SC] CreateService SUCCESS

```

Run the service by typing

```cmd
>sc start sservice

SERVICE_NAME: sservice
        TYPE               : 10  WIN32_OWN_PROCESS
        STATE              : 2  START_PENDING
                                (NOT_STOPPABLE, NOT_PAUSABLE, IGNORES_SHUTDOWN)
        WIN32_EXIT_CODE    : 0  (0x0)
        SERVICE_EXIT_CODE  : 0  (0x0)
        CHECKPOINT         : 0x0
        WAIT_HINT          : 0x7d0
        PID                : 6268
        FLAGS              :

```

Check for **Logs\SvcBatch.log** file.

To stop the service type

```cmd
>sc stop sservice

SERVICE_NAME: sservice
        TYPE               : 10  WIN32_OWN_PROCESS
        STATE              : 3  STOP_PENDING
                                (NOT_STOPPABLE, NOT_PAUSABLE, IGNORES_SHUTDOWN)
        WIN32_EXIT_CODE    : 0  (0x0)
        SERVICE_EXIT_CODE  : 0  (0x0)
        CHECKPOINT         : 0x1
        WAIT_HINT          : 0x7530

```

When done experimenting, delete the service
by typing

```cmd
>sc delete sservice
[SC] DeleteService SUCCESS

```




