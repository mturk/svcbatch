# SvcBatch: Overview

SvcBatch is a program that allows to run batch file
as Windows service.

Program's main goal is to run any application as Windows
service by using a batch file wrapper as application launcher.
This is particularly useful when running Java applications or
scripts written in Perl, Python, Ruby, etc... without the need
for a specialized service wrapper.

SvcBatch was designed to be simple to use and lightweight with small
memory footprint. It is written in around 2K lines of pure **C** code,
and depends only on win32 API. There are no configuration
files or installation requirements, so it can be easily distributed
alongside any application that requires Windows service functionality.

Read the rest of the documentation and check [examples](docs/examples/)
for some overview and ideas how to use and deploy SvcBatch
with your application.

* Github: [https://github.com/mturk/svcbatch](https://github.com/mturk/svcbatch)
* Docs: [Documentation](docs/index.md)

# Table of Contents

- [SvcBatch: Overview](#svcbatch-overview)
- [Table of Contents](#table-of-contents)
- [Getting Started](#getting-started)
  - [Supported Windows Versions](#supported-windows-versions)
  - [Building](#building)
  - [Creating Services](#creating-services)
- [Examples](#examples)
- [Main Features](#main-features)
  - [Log Rotation](#log-rotation)
  - [Command Line Options](#command-line-options)
  - [Private Environment Variables](#private-environment-variables)
  - [Custom Control Codes](#custom-control-codes)
  - [Stop and Shutdown](#stop-and-shutdown)
- [Contributing](#contributing)
- [License](#license)

# Getting Started

## Supported Windows Versions

The minimum supported Windows Version is `NT 6.1 64-bit`.
This means that you will need at least `Windows 7 SP1` or `Windows Server 2008 R2`
to run this application.


## Building

To build the SvcBatch from source code follow
directions explained in [Building](docs/building.md) document.
SvcBatch is targetet for Windows 64-bit versions, so make sure
to use 64-bit compiler.

## Creating Services

SvcBatch does not contain any code for service management.
Users should use Microsoft's `sc.exe` utility to
create, configure, manage and delete services.
Sc is integral part of every Windows distribution and
there is no need to replicate that functionality internally.
Check [Microsoft's SC documentation](https://docs.microsoft.com/en-us/windows-server/administration/windows-commands/sc-config)
for detailed description how to use the SC utility to create
and manage services.

SvcBatch uses System's `cmd.exe` as a shell to run a batch file.
Thus the batch file is an *actual* service application from
concept point of view.

The batch file should behave like a *service* and must never
exit. Any exit is treated as error because from SCM
(Service Control Manager) point of view it is the same
as service failure. On Stop or Shutdown events signaled
by SCM, SvcBatch will send CTRL_C signal to cmd.exe, as
if user hit Ctrl+C keys in interactive console session.

The simplest way to create a service for your batch file
is to put `svcbatch.exe` in the same directory where your
`myservice.bat` file is located. Open command prompt and
type something like this...

```cnd
> sc create myservice binPath= ""%cd%\svcbatch.exe" myservice.bat"

```

Check [Examples](docs/examples/) section for more
detailed usage.


**Notice!**
If the program started from service batch file creates
its own child processes ensure to setup the following
privileges to the service

```cmd
> sc privs myservice SeDebugPrivilege
```

This will allow SvcBatch to terminate the entire
descendant process tree on shutdown.

# Examples

To get an overview how the SvcBatch is used with real
application check the documentation [Examples](docs/examples/)
section for some basic deployment guideline.


# Main Features

## Log Rotation

By default SvcBatch on startup creates a `Logs` directory inside its
working directory and creates a SvcBatch.log file that is used both
for internal logging and capturing output from `cmd.exe`

It also rotates (renames) previous log files if the files are
present inside `Logs` directory using the following procedure:

* If present move `SvcBatch.log` to `SvcBatch.log.0`
* If present move `SvcBatch.log.3` to `SvcBatch.log.4`

  This means that `SvcBatch.log.4` will be overwritten, so make sure
  to backup `SvcBatch.log.4` before log rotation occurs if needed

* If present move `SvcBatch.log.2` to `SvcBatch.log.3`
* If present move `SvcBatch.log.1` to `SvcBatch.log.2`
* If present move `SvcBatch.log.0` to `SvcBatch.log.1`
* Create new `SvcBatch.log` and use it as current logfile.


User can use `sc.exe control [service name] 234` to initiate
log rotation at any time while the service is running.
Note that **234** is our custom service control code.


## Command Line Options

SvcBatch command line options allow user to customize
service deployments. Options are case insensitive and both `-` and `/` can be
used as switches. This means that `/o /O -o and -O` can be used for the same option..

After handling switches SvcBatch will use the last argument
as batch file to execute.

Command line options are defined at service install time, so
make sure to get familiar with `sc.exe` utility.

### -b
**Enable sending CTRL_BREAK_EVENT**

This option enables our custom service control
code to send `CTRL_BREAK_EVENT` to the child processes.

Check *Custom control codes* section below for more details

### -c
**Use clean PATH**

This option will replace **PATH** environment variable with minimal
set of paths that are needed to run the batch file.

The path of svcbatch.exe is used as first path element.
For example if you have installed a service with svcbatch.exe from
`C:\Program Files\SvcBatch\svcbatch.exe` then the **PATH** environment
variable will be set to:


```no-highlight
    C:\Program Files\SvcBatch;
    C:\Working\Directory;
    %SystemRoot%\System32;
    %SystemRoot%;
    %SystemRoot%\System32\Wbem;
    %SystemRoot%\System32\WindowsPowerShell\v1.0"
```

This option is useful to separate the service from
other services running with the same account.
The batch file can set **PATH** to desired value
and then call the actual application.

### -o [path]
**Output directory**

This option allows to set the output directory where SvcBatch
will create any runtime data files.

If set the **path** parameter will be used as
location where SvcBatch.log files will be created.
SvcBatch will create **path** directory if it doesn't exist.

If not set, SvcBatch will create and use **SVCBTCH_SERVICE_HOME\Logs**
directory as a location for log files and any runtime data
that has to be created.

This directory has to be unique for each service instance. Otherwise
service will fail if another service already opened SvacBatch.log
in that location.

### -s
**Use safe environment**

Remove all environment variables for child
processes, except the system ones.

This option allows every batch file to have a clean
environment, regardless of how many variables are
defined for `LOCAL_SERVICE` account.

Check [svcbatch.c](svcbatch.c) **safewinenv[]**
string array for a complete list of environment variables
that are passed to child process. If this option is set
all other environment variables not belonging to that set
will be omitted from child process environment.


### -w [path]

Set service working directory to **path**

This option allows to explicitly set the working
directory. This allows to have relative path
for batch file parameter and common location for
svcbatch.exe.

If not specified, the working directory is set
to the path of the batch file if it was defined
as absolute path. Otherwise directory of svcbatch.exe
will be used as working directory.

## Private Environment Variables

SvcBatch sets few private environment variables that
provide more info about running environment to batch file.


Here is the list of environment variables that
SvcBatch sets for each instance.

#### SVCBATCH_VERSION_ABI

This environment variable is set to the value of
current svcbatch.exe ABI. This can be used by batch
file to determine the SvcBatch functionality.

The ABI version is defined in svcbatch.h file and
it's current value is **20201209**

#### SVCBATCH_SERVICE_BASE

This variable is set to the directory of the SvcBatch
executable.

#### SVCBATCH_SERVICE_SELF

This variable is set to SvcBatch executable name.

#### SVCBATCH_SERVICE_HOME

This variable is set to the service working directory.

#### SVCBATCH_SERVICE_NAME

This variable is set to the actual service name
defined with `sc create [service name] ...`

```batchfile
@echo off
rem
rem Simple example
rem

echo Running service %SVCBATCH_SERVICE_NAME%


```

#### SVCBATCH_SERVICE_UUID

This is service's unique identifier in UUID hex format
`01234567-89ab-cdef-0123-456789abcdef` and it is
randomly generated on service startup.

`SVCBATCH_SERVICE_UUID` can be used inside batch file
when unique identifier is needed.

```batchfile
rem
rem Create unique temp directoy
rem
md "%TEMP%\%SVCBATCH_SERVICE_UUID%"
...
... do some work using that directory
...
rd /S /Q "%TEMP%\%SVCBATCH_SERVICE_UUID%"

```

## Custom Control Codes

SvcBatch can send `CTRL_BREAK_EVENT` signal to its child processes.

This allows programs like **java.exe** to act upon that signal.
For example JVM will dump full thread stack in the same way
as if user hit the `CTRL` and `Break` keys in interactive console.

Use `sc.exe control [service name] 233` to send
`CTRL_BREAK_EVENT` to all child processes.
Again as with log rotate, the **233** is our custom control code.

**Important!**

This option is enabled at service install time with `/b` command
switch option.

Do not send `CTRL_BREAK_EVENT` if the batch file runs a process
that does not have custom `CTRL_BREAK_EVENT` console handler.
By default the process will exit and service will either fail or hang.

## Stop and Shutdown

When you type `sc.exe stop myservice` or when machine gets into
shutdown state SvcBatch running as a service will receive
stop or shutdown event. SvcBatch will send `CTRL_C_EVENT` to its
child processes.

It is up to the application started from batch file to
handle this event and do any cleanup needed and then exit.
After sending `CTRL_C_EVENT` service will wait for one second
and write 'Y' to `cmd.exe` stdin, to handle that obnoxious
`Terminate batch job (Y/N)?` prompt. If batch file or any of
downstream processes do not exit within that timeout,
SvcBatch will give another 20 seconds for all processes to exit.
After that timeout it will simply kill each descendant process
by calling `TerminateProcess` for every process that originated
from svcbatch.exe.

# Contributing

SvcBatch is an open source project, and is thus built with
your contributions.

# License

The code in this repository is licensed under the [Apache-2.0 License](LICENSE.txt).

