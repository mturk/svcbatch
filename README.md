# SvcBatch: Overview

SvcBatch is a program that allows to run batch file
as Windows service.

Program's main goal is to run any application as Windows
service by using a batch file wrapper as application launcher.
This is particularly useful when running Java applications or
scripts written in Perl, Python, Ruby, etc... without the need
for a specialized service wrapper.


## Supported Windows Versions

The minimum supported Windows Version is `NT 6.1 64-bit`.
This means that you will need at least `Windows 7 SP1` or `Windows Server 2008 R2`
to run this application.


## Building

To build the SvcBatch from source code follow
directions explained in [Building](docs/building.md) document.
SvcBatch is targetet for Windows 64-bit versions, so make sure
to use 64-bit compiler.

## Installing services

SvcBatch does not contain any code for service management.
Users should use Microsoft's `sc.exe` utility to
install, configure, manage and delete services.
Sc is integral part of every Windows distribution and
there is no need to replicate that functionality internally.

If the program started from service batch file can create
its own child processes ensure to setup the following
privileges to the service

```no-highlight
> sc privs svcname SeCreateSymbolicLinkPrivilege/SeDebugPrivilege
```

This should allow SvcBatch to terminate the entire
descendant process tree on shutdown.

## Examples

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
* If present move `SvcBatch.log.2` to `SvcBatch.log.3`

  This means that `SvcBatch.log.3` will be overwritten, so make sure
  to backup `SvcBatch.log.3` before log rotation occurs if needed

* If present move `SvcBatch.log.1` to `SvcBatch.log.2`
* If present move `SvcBatch.log.0` to `SvcBatch.log.1`
* Create new `SvcBatch.log` and use it as current logfile.


User can use `sc.exe control [service name] 234` to initiate
log rotation at any time while the service is running.
Note that **234** is our custom service control code.


## Command line options

SvcBatch command line options allow user to customize
service deployments. Options are case insensitive and both `-` and `/` can be
used as switches. This means that `/o /O -o and -O` can be used for the same option..

After handling switches SvcBatch will use the last argument
as batch file to execute.

Command line options are defined at service install time, so
make sure to get familiar with `sc.exe` utility.

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


## Private environment variables

SvcBatch sets few private environment variables that
provide more info about running environment to batch file.


Here is the list of environment variables that
SvcBatch sets for each instance.

#### SVCBATCH_VERSION_ABI

This environment variable is set to the value of
current svcbatch.exe ABI. This can be used by batch
file to determine the SvcBatch functionality.

The ABI version is defined in svcbatch.h file and
it current value is **20201201**

#### SVCBATCH_SERVICE_BASE

This variable is set to the directory of the SvcBatch
executable.

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

In future releases this variable will be also used
for inter-process communication between SvcBatch and
batch file's child process.


## Custom control codes

SvcBatch can send `CTRL_BREAK_EVENT` signal to its child processes.

This allows programs like **java.exe** to act upon that signal.
For example JVM will dump full thread stack in the same way
as if user hit the `CTRL` and `Break` keys in interactive console.

Use `sc.exe control [service name] 233` to send
`CTRL_BREAK_EVENT` to all child processes.
Again as with log rotate, the **233** is our custom control code.

### Important!

Do not send `CTRL_BREAK_EVANT` if the batch file runs a process
that does not have custom `CTRL_BREAK_EVENT` console handler.
By default the process will exit and service will either fail or hang.



# License

The code in this repository is licensed under the [Apache-2.0 License](LICENSE.txt).

