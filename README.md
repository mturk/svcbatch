# SvcBatch: Overview

SvcBatch is a program that allows users to run batch files as Windows service.

The program's main goal is to run any application as a Windows
service by using a batch file wrapper as an application launcher.
This is particularly useful when running Java applications or
for scripts written in Perl, Python, Ruby, etc... without the need
for a specialized service wrapper.

SvcBatch was designed to be simple to use and lightweight, with a small
memory footprint. Its only dependency is win32 API, and only has
around 3K lines of **C** code. There are no configuration
files or installation requirements, so it can be easily distributed
alongside any application that requires Windows service functionality.

Read the rest of the documentation and check [Examples](#examples)
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
  - [Version Information](#version-information)
  - [Error Logging](#error-logging)
- [License](#license)

# Getting Started

## Supported Windows Versions

The minimum supported version is *Windows 7 SP1* or
*Windows Server 2008 R2* 64-bit.


## Building

To build the SvcBatch from source code follow the
directions explained in [Building](docs/building.md) document.
SvcBatch is targeted for Windows 64-bit versions, so make sure
to use 64-bit compiler.

## Creating Services

SvcBatch does not contain any code for service management.
Users should use Microsoft's `sc.exe` utility to
create, configure, manage, and delete services.
`Sc` is an integral part of every Windows distribution and
there is no need to replicate that functionality internally.
Check [Microsoft's SC documentation](https://docs.microsoft.com/en-us/windows-server/administration/windows-commands/sc-config)
for detailed description how to use the `SC` utility to create
and manage services.

SvcBatch uses System's `cmd.exe` as a shell to run a batch file.
Thus the batch file is an *actual* service application from a
conceptual point of view.

The batch file should behave like a *service* and must never
exit. Any exit is treated as an error because from the SCM
(Service Control Manager) point of view it is the same
as a service failure. On Stop or Shutdown events signaled
by SCM, SvcBatch will send a CTRL_C signal to cmd.exe, as
if a user hit Ctrl+C keys in interactive console session.

The simplest way to create a service for your batch file
is to put `svcbatch.exe` in the same directory where your
`myservice.bat` file is located. Open the command prompt and
type something like this...

```cnd
> sc create myservice binPath= ""%cd%\svcbatch.exe" myservice.bat"

```

Check [Examples](#examples) section for more
detailed usage.


* **Notice**

  If the program started from a service batch file creates
  its own child processes, ensure you setup the following
  privileges to the service:

  ```cmd
  > sc privs myservice SeDebugPrivilege

  ```

  This will allow SvcBatch to terminate the entire
  descendant process tree on shutdown.

# Examples

To get an overview on how the SvcBatch is used with the real
application, check the [documentation](docs/index.md)
section for some basic deployment guideline.


# Main Features

Here are listed some of the main features provided by
SvcBatch.

## Log Rotation

By default SvcBatch, on startup, creates a `Logs` directory inside its
working directory and creates an SvcBatch.log file that is used both
for internal logging and capturing output from `cmd.exe`

It also rotates (renames) previous log files if the files are
present inside `Logs` directory using the following procedure:

```no-highlight

* If exists move SvcBatch.log to SvcBatch.log.0
* If exists move SvcBatch.log.8 to SvcBatch.log.9

  This means that SvcBatch.log.9 will be overwritten, so make sure
  to backup SvcBatch.log.9 before log rotation occurs if needed

* If exists move SvcBatch.log.7 to SvcBatch.log.8
* ...
* ...
* If exists move SvcBatch.log.1 to SvcBatch.log.2
* If exists move SvcBatch.log.0 to SvcBatch.log.1
* Create new SvcBatch.log and use it as current log file.

```

Users can use `sc.exe control [service name] 234` to initiate a
log rotation at any time while the service is running.
Note that **234** is our custom service control code.
Number **234** has been randomly chosen, since win32
API requires that this number must be larger then `127` and
lower then `255`.

Users can disable log rotation by adding **-m 0** option.
In that case SvcBatch.log file will be be created or opened
for append if already present.

In case the last log rotation was less then `2` minutes ago, no
log rotation will happen.

## Command Line Options

SvcBatch command line options allow users to customize
service deployments. Options are case insensitive and both `-` and `/` can be
used as switches. This means that `/b /B -b and -B` can be used for the same option..

After handling switches SvcBatch will use the next argument
as the batch file to execute.

Any additional arguments will be passed as arguments to batch file.
If additional argument contains `@` characters, each `@` character
will be converted to `%` character at runtime.
In case you need to pass `@` character use `@@`.

Command line options are defined at service install time, so
make sure to get familiar with `sc.exe` utility.

* **-b**

  **Enable sending CTRL_BREAK_EVENT**

  This option enables our custom service control
  code to send `CTRL_BREAK_EVENT` to the child processes.

  See [Custom Control Codes](#custom-control-codes) section below for more details


* **-c [code page]**

  **Sets run-time code page**

  This option allows to change default system code page.
  If defined, all internal logging will be translated to **code page**.

  ```cmd
  > sc create ... -v -c utf-8 ...

  ```
  This will convert all internal logging to **utf-8** code page.


* **-e [program][argument]**

  **Set external log program**

  This option allows a user to set the external application
  which will be used instead log file.

  If set, the **program** will be executed when SvcBatch
  opens log files and all messages will be send to that
  program instead of log file.

  The first argument to the **program** is always
  log file name, unless additional **argument** is not defined.

  To define arguments for external **program** use
  multiple **-e** command options.

  ```cmd
  > sc create ... -e program.exe -e external.log -e argument ...

  ```

  The **program** current directory is always set
  to service output directory.

  **Notice**

  This option is mutually exclusive with log rotation
  related options. Do not use options `m`, `r`, or `v`
  together with this option when installing service.
  Service will fail to start, and write and error message
  to the Windows Event log.

* **-k [timeout]**

  **Set stop timeout in milliseconds**

  This option sets the **timeout** when service receives
  stop or shutdown signal.
  The valid **timeout** range is between `1000` and `20000`
  milliseconds.

  By default this value is set to `10` seconds.


* **-l**

  **Use local time**

  This option causes all logging and rotation
  to use local instead system time.


* **-m [number]**

  **Set maximum number of log files**

  In case the **number** contains a single decimal number
  between `0 and 9` it will be used instead default `1...9`.

  ```cmd
  > sc create ... -m 4

  ```
  Instead rotating Svcbatch.log from `1...9` it will rotate
  exiting log files from `1...4.`. In case that number is `0`,
  log rotation will be disabled.


* **-n [log name][shutdown name]**

  **Set log file name**

  This option allows a user to set the alternate log file names.

  By default SvcBatch will use `SvcBatch.log` as **log name**
  and `SvcBatch.shutdown.log` as **shutdown name**.

  To redefine default log names use multiple **-n**
  command options at service install:

  ```cmd
  > sc create ... -n MyService.log -n MyService.shutdown.log ...

  ```

  In case **-s** option is defined the **shutdown name** will be
  used if provided instead default `SvcBatch.shutdown.log` file name.
  In case **shutdown name** is `NUL`, shutdown logging will be disabled.

  If **-n** argument includes any `@` characters, they will be replaced
  with `%` character at runtime and treated as a format string
  to `strftime` function.

  When using `strftime` filename formatting, be sure the
  log file name format has enough granularity to produce a different
  file name each time the logs are rotated. Otherwise rotation
  will overwrite the same file instead of starting a new one.
  For example, if logfile was `service.@Y-@m-@d.log` with log rotation
  at `5` megabytes, but `5` megabytes was reached twice in the same day,
  the same log file name would be produced and log rotation would
  overwrite the same file.

  **Supported formatting codes**

  Here are listed the supported formatting codes:

  ```no-highlight
    @a  Abbreviated weekday name in the locale
    @b  Abbreviated month name in the locale
    @d  Day of month as a decimal number (01 - 31)
    @H  Hour in 24-hour format (00 - 23)
    @I  Hour in 12-hour format (01 - 12)
    @j  Day of the year as a decimal number (001 - 366)
    @m  Month as a decimal number (01 - 12)
    @M  Minute as a decimal number (00 - 59)
    @p  The locale's A.M./P.M. indicator for 12-hour clock
    @S  Second as a decimal number (00 - 59)
    @y  Year without century, as decimal number (00 - 99)
    @Y  Year with century, as decimal number
  ```

  Make sure that log names contain only valid file name characters.
  The following are reserved characters:

  ```no-highlight
    <  (less than)
    >  (greater than)
    :  (colon)
    "  (double quote)
    /  (forward slash)
    \  (backslash)
    |  (vertical bar or pipe)
    ?  (question mark)
    *  (asterisk)
  ```

  In case the result from `strftime` contains any of the reserved
  characters they will be removed.

  For example if the **-n** argument contains `@D` it will be resolved
  as equivalent to `@m/@d/@y`. Since `/` is invalid file name character,
  all occurrences of the `/` character will be removed from the final result.
  This means that resolved `03/06/23` will make final result as `030623`.

* **-o [path]**

  **Set service output directory**

  This option allows a user to set the output directory, which is where SvcBatch
  will create any runtime data files.

  If set, the **path** parameter will be used as the
  location where SvcBatch.log files will be created.
  SvcBatch will create a **path** directory if it doesn't exist.

  If not set, SvcBatch will create and use the  **SVCBATCH_SERVICE_HOME\Logs**
  directory as a location for log files and any runtime data
  that has to be created.

  This directory has to be unique for each service instance. Otherwise the
  service will fail if another service already opened SvacBatch.log
  in that location.


* **-p**

  **Enable preshutdown service notification**

  When defined, SvcBatch will accept `SERVICE_CONTROL_PRESHUTDOWN` control code.
  The service control manager waits until the service stops or the specified
  preshutdown time-out value expires


* **-q**

  **Disable logging**

  This option disables both logging and log rotation.

  When defined no log files or directories will be created and
  any output from service batch files will be discarded.

  Use this option when output from `cmd.exe` is not needed or
  service batch file manages logging on its own.

  **Notice**

  This option is mutually exclusive with other log related
  command options. Do not use options `-e`, `m`, `n`, `r`,
  `t` or `v` together with this option when installing service.
  Service will fail to start, and write an error message
  to the Windows Event log.


* **-r [rule]**

  **Rotate logs by size or time interval**

  SvcBatch can automatically rotate log files beside
  explicit `sc.exe control [service name] 234` command.

  Depending on the **rule** parameter service can rotate
  log files at desired interval, once a day at specific time
  or when log file gets larger then defined size.

  Time and size values can be combined, that allows
  to rotate logs at specific time or size which ever first.
  For example one can define **rule** so that rotate logs
  is run each day at `17:00:00` hours or if log files gets
  larger then `100K` bytes.

  ```cmd
  > sc create ... -r 17:00:00 -r 100K

  ```

  If time is given without a colons, SvcBatch will use it
  as minutes between log rotation.

  ```cmd
  >sc create ... -r 90 - r 200K

  ```

  The upper example will rotate logs each `90` minutes. In case
  log file gets larger the 200Kbytes within that interval,
  it will be rotated as well. In that case internal timer
  will be reset and next rotation will occur after `90` minutes.

  In case **rule** parameter is `0` SvcBatch will rotate
  log files each day at midnight. This is the same as
  defining `-r 00:00:00`.

  In case **rule** parameter is `60` SvcBatch will rotate
  log files every full hour.

  In case **rule** parameter for rotation based on log file size
  is less then `1K` (1024 bytes), SvcBatch will not rotate logs by size.

  The **rule** parameter uses the following format:

  ```no-highlight
      <[minutes|hh:mm:ss]>|<size[B|K|M|G]>
  ```

  When this parameter is defined log rotation will not use
  the logic defined in [Log Rotation](#log-rotation) section.

  Instead rotating Svcbatch.log from `1...9` it will rename
  existing `SvcBatch.log` to `SvcBatch.log.YYYYMMDDhhmmss`.
  The `YYYYMMDDhhmmss` is the current local or system time.


* **-s [batchfile][argument]**

  **Execute batch file on service stop or shutdown**

  If defined, on shutdown or stop event the service
  will create separate `svcbatch.exe` process and call **batchfile**.
  The purpose of that file is to use some sort of `IPC` mechanism
  and signal the service to exit.

  This is particularly useful for services that do not handle
  `CTRL_C_EVENT` or have specific shutdown requirements.

  If multiple **-s** command line option are defined the
  first one will be used as **batchfile** and rest will
  be used as additional **argument** send to the **batchfile**.

* **-t**

  **Truncate log file instead reusing**

  This option causes the logfile to be truncated instead of rotated.

  This is useful when a log is processed in real time by a command
  like tail, and there is no need for archived data.


  ```cmd
  > sc create ... -t ...

  ```

  This will truncate existing `SvcBatch.log` and `SvcBatch.shutdown.log`
  on open or rotate instead creating a new file.

  If multiple **-t** options are defined only the service stop log file
  will be truncated.

  ```cmd
  > sc create ... -tt ...

  ```

  This will truncate existing `SvcBatch.shutdown.log` instead
  appending on service shutdown. This feature is usable
  only if **-s** option is defined.


* **-v**

  **Log internal messages**

  This option enables logging of various internal
  SvcBatch messages.

  If enabled SvcBatch will put something like the following
  when log file is opened:

  ```no-highlight
    [00:00:00.034518] SvcBatch 2.1.0.0_2.dbg (msc 193532217.1)
    [00:00:00.034646] Log opened       : 2023-04-30 00:38:32
    [00:00:00.034711] Service name     : adummysvc
    [00:00:00.034728] Service uuid     : 0a5c-d10e67f7-c9c1-c420-1fc5-d7e26f223282
    [00:00:00.034764] Batch file       : C:\Workplace\svcbatch\test\dummyservice.bat
    [00:00:00.034780] Shutdown batch   : C:\Workplace\svcbatch\test\dummyservice.bat
    [00:00:00.034795] Program directory: C:\Workplace\svcbatch\.build\dbg
    [00:00:00.034842] Base directory   : C:\Workplace\svcbatch\test
    [00:00:00.034857] Home directory   : C:\Workplace\svcbatch\test
    [00:00:00.035426] Logs directory   : C:\Workplace\svcbatch\test\Logs\adummysvc

    ...

    [00:00:11.003626] Service signaled : SERVICE_CONTROL_STOP

    ...

    [00:00:21.059643] Log closed       : 2023-04-30 00:38:53

  ```


* **-w [path]**

  **Set service working directory**

  This option enables users to explicitly set the working
  directory. This allows for having a relative path
  for batch file parameters and a common location for
  svcbatch.exe.

  If not specified, the working directory is set
  to the path of the batch file if it was defined
  as an absolute path. Otherwise directory of svcbatch.exe
  will be used as the working directory.


## Private Environment Variables

SvcBatch sets a few private environment variables that
provide more info about running environments to batch files.


Here is the list of environment variables that
SvcBatch sets for each instance.

* **SVCBATCH_SERVICE_BASE**

  This variable is set to the directory of the service
  batch file.

* **SVCBATCH_SERVICE_HOME**

  This variable is set to the service working directory.

* **SVCBATCH_SERVICE_LOGS**

  This variable is set to the service's log directory.

  In case the logging is disabled, by using **-q**
  command option, this variable is
  set to the **SVCBATCH_SERVICE_HOME** directory.

  However, if the **-o** command line option was defined
  together with **-q** option, the directory specified by
  the **-o** command option parameter must exist, or the
  service will fail to start and write error message to the
  Windows Event Log.


* **SVCBATCH_SERVICE_NAME**

  This variable is set to the actual service name
  defined with `sc create [service name] ...`

  ```batchfile
  @echo off
  rem
  rem Simple example
  rem

  echo Running service %SVCBATCH_SERVICE_NAME%

  ```

* **SVCBATCH_SERVICE_UUID**

  This is the service's unique identifier in following hexadecimal format
  `abcd-01234567-89ab-cdef-0123-456789abcdef`.
  The first four digits are current process id, and remaining digits
  are randomly generated at service startup.

  `SVCBATCH_SERVICE_UUID` can be used inside batch file
  when unique identifier is needed.

  ```batchfile
  rem
  rem Create unique temp directory
  rem
  md "%TEMP%\%SVCBATCH_SERVICE_UUID%"
  ...
  ... do some work using that directory
  ...
  rd /S /Q "%TEMP%\%SVCBATCH_SERVICE_UUID%"

  ```

* **SVCBATCH_APP_BIN**

  Set to the current SvcBatch executable running the service

* **SVCBATCH_APP_VER**

  This environment variable is set to the current version
  of the SvcBatch executable running the service.


## Custom Control Codes

SvcBatch can send `CTRL_BREAK_EVENT` signal to its child processes.

This allows programs like **java.exe** to act upon that signal.
For example JVM will dump it's full thread stack in the same way
as if user hit the `CTRL` and `Break` keys in interactive console.

Use `sc.exe control [service name] 233` to send
`CTRL_BREAK_EVENT` to all child processes.
Again, as with log rotate, the **233** is our custom control code.

* **Important**

  This option is enabled at service install time with `/b` command
  switch option.

  Do not send `CTRL_BREAK_EVENT` if the batch file runs a process
  that does not have a custom `CTRL_BREAK_EVENT` console handler.
  By default the process will exit and the service will either fail.


## Stop and Shutdown

When you type `sc.exe stop myservice` or when your machine gets into the
shutdown state, SvcBatch running as a service will receive
stop or shutdown event. SvcBatch will send `CTRL_C_EVENT` to its
child processes or run shutdown batch file in case
**-s [batchfile]** was defined at install time.

It is up to the application started from batch file to
handle this event and do any cleanup needed and then exit.
On startup service  writes 'Y' to `cmd.exe` stdin,
to handle that obnoxious `Terminate batch job (Y/N)?` prompt.
If batch file or any of downstream processes do not exit within that timeout,
SvcBatch will give another 20 seconds for all processes to exit.
After that timeout it will simply kill each descendant process
that originated from svcbatch.exe.

## Version Information

The simplest way to obtain the version information
is to right click on the `svcbatch.exe` from Windows File
Explorer, click on Properties, and then on the Details tab.


Another way to get SvcBatch version and build information
is to open command prompt and type

  ```cmd
  > svcbatch.exe
  SvcBatch 1.2.3.4 ...

  >
  ```

The actual version information can be obtained by inspecting
the top of the service's `SvcBatch.log` file. This information
will be present in the log file, only if **-v** command
option was defined at service's install.

Make sure to use the correct information when filing
bug reports.

## Error Logging

SvcBatch logs any runtime error to Windows Event Log.
Use Windows **Event Viewer** and check `Windows Logs/Application/SvcBatch`
events.

# License

The code in this repository is licensed under the [Apache-2.0 License](LICENSE.txt).

