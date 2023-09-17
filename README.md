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
  - [Building SvcBatch](#building-svcbatch)
  - [Creating Services](#creating-services)
  - [Managing Services](#managing-services)
  - [Debugging Services](#debugging-services)
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


## Building SvcBatch

To build the SvcBatch from source code follow the
directions explained in [Building](docs/building.md) document.
SvcBatch is targeted for Windows 64-bit versions, so make sure
to use 64-bit compiler.

## Creating Services

Starting with version **3.0.0** SvcBatch has a Service management
code that contains a subset of Microsoft's `sc.exe` utility to
create, configure, manage, and delete services.
Check the [managing](docs/manage.md) section for some basic
guidelines.

Check [Microsoft's SC documentation](https://docs.microsoft.com/en-us/windows-server/administration/windows-commands/sc-config)
for detailed description how to use the `SC` utility to create
and manage services.

By default SvcBatch uses System's `cmd.exe` as a shell to run a batch file.
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

```no-highlight
> svcbatch create myservice

```
... or using `SC` utility

```no-highlight
> sc create myservice binPath= ""%cd%\svcbatch.exe" myservice.bat"

```

Check [Examples](#examples) section for more
detailed usage.


* **Modifying services**

  Once installed you can edit the **ImagePath** value
  from the service's registry key:

  **HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\myservice**

  In case the service was installed using SvcBatch, additional
  **ImagePathArgumets** value is created under the service's
  registry key that contains arguments SvcBatch will merge at
  runtime with the value of **ImagePath**.

  The changes made, will be used next the service starts.


* **Starting services**

  To manually start the service use either `sc` or Microsoft
  Services GUI application.

  SvcBatch will append any additional arguments from the
  service start application to the batch file's existing arguments
  defined at install time.

  Since SvcBatch version **3.0.0**, you can use the
  SvcBatch itself to start the service.

  ```no-highlight
  > svcbatch create myservice myservice.bat param1

  ...

  > svcbatch start myservice param2 param3

  ```

  Or you can use Microsoft `sc` utility

  ```no-highlight
  > sc create myservice binPath= ""%cd%\svcbatch.exe" myservice.bat param1"

  ...

  > sc start myservice param2 param3

  ```

  When started the `myservice.bat` will receive `param1 param2 param3`
  as arguments.



* **Notice**

  If the program started from a service batch file creates
  its own child processes, ensure you setup the following
  privileges to the service:

  ```no-highlight
  > svcbatch configure myservice --privs=SeDebugPrivilege

  ```

  This will allow SvcBatch to terminate the entire
  descendant process tree on shutdown in case the
  child process creates a process with different
  security credentials.

  Also check the [Managing Services](#managing-services)
  section for further guidelines.

## Managing Services

To get an overview on how to create and manage
SvcBatch services, check the [managing](docs/manage.md)
section for some basic guidelines.


## Debugging Services

Debugging a service can be a complex task, because the services
run without interacting with the user.

In case your service fails without apparent reason, first check
the Windows Event Log.

The next option is to use **debug** build of the SvcBatch.
Download or build svcbatch.exe compiled with `_DEBUG=1` option
and replace the svcbatch.exe with this binary.

Debug version of SvcBatch will create a **svcbatch_debug.log** file
inside the **TEMP** directory of the current service user account.

The typical content of the **svcbatch_debug.log** file might
look something like the following:


```no-highlight

```

The format of this log file is:

**[ProcessId:ThreadId:MonthDayYear/HourMinuteSecond.Millisecond:Mode:FunctionName(line)] message**


If the **svcbatch_debug.log** file already exists, debug messages will
be appended to the end of the file.

You can rename **svcbatch.exe** executable to **myservice.exe**
and modify service ImagePath registry value. When the service
start, SvcBatch will use **myservice_debug.log** as log file.
This can be useful when multiple service share the same SvcBatch executable.



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

It also renames previous log files if the files are
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

In case **-f:R** option was defined, users can use
`sc.exe control [service name] 234` to initiate a
log rotation at any time while the service is running.
Note that **234** is our custom service control code.
Number **234** has been randomly chosen, since win32
API requires that this number must be larger then `127` and
lower then `255`.

In case the last log rotation was less then `2` minutes ago,
or if there was no data written to the log file from the last
rotation, SvcBatch will not rotate logs.


## Command Line Options

SvcBatch command line options allow users to customize
service deployments. Options are case insensitive and
defined with **-** as command switch when running in
service mode, or with **--** for SvcBatch service management.
This means that `-h or -H` and `--Wait or --wait`
can be used interchangeably.

Command line option values can be either the rest of the
command option or the entire next argument.

In case they are the rest of the command option, the
character after the option must be **:** or **=** character
or the service will fail to start.

For example:

```no-highlight
> svcbatch create ... -o:log\directory ...

Is the same as

> svcbatch create ... -o log\directory ...

```

After handling switches SvcBatch will pass remained
arguments to the script interpreter.

If there is no additional arguments, SvcBatch will
append `.bat` to the running Service Name.
In that case, if `ServiceName` contain any of the
invalid file name characters `/\:;<>?*|"`,
the service will fail and error message will be
reported to Windows Event log.





* **-f [features]**

  **Set runtime features**

  This option sets various runtime features.
  The **features** parameter is a combination of
  one or more characters, where each character sets
  the particular feature option.

  Feature options are case sensitive, and can be
  listed in any order.

  ```no-highlight
      <B><C><L><P><Q><R><U><Y><0|1|2>
  ```

    * **B**

      **Generate CTRL_BREAK on service stop**

      This option can be used to send `ctrl+break` instead `ctrl+c`
      signal when the service is stopping.

      This is useful when the service batch file uses `start /B ...`
      to launch multiple applications.

      ```batchfile
      ...
      start /B some.exe instance1
      start /B some.exe instance2
      start /B some.exe instance3
      ...
      ```

      When using `start /B application`, the application does
      not receive `ctrl+c` signal. The `ctrl+break` is the only
      way to interrupt the application.


    * **C**

      **Enable sending CTRL_BREAK_EVENT**

      This option enables our custom service control
      code to send `CTRL_BREAK_EVENT` to the child processes.

      See [Custom Control Codes](#custom-control-codes)
      section below for more details

      **Notice**

      This option is mutually exclusive with **B** feature option.
      If this option is defined together with the mentioned option,
      the service will fail to start, and write an error message
      to the Windows Event log.


    * **F**

      **Create stop file**

      This option will create a file inside service logs
      directory on service stop.

      The created temporary file name is **ss-%SVCBATCH_SERVICE_UUID%**,
      and can be used by service script or application to signal
      that service should stop.

      Service should monitor if that file exists on regular intervals,
      and if the file is present, service should exit.

      **Notice**

      This option is mutually exclusive with **-s** command option.
      If this feature is defined together with the mentioned option,
      the service will fail to start, and write an error message
      to the Windows Event log.

    * **L**

      **Use local time**

      This option causes all logging and rotation
      to use local instead system time.


    * **P**

      **Enable preshutdown service notification**

      When defined, SvcBatch will accept `SERVICE_CONTROL_PRESHUTDOWN` control code.
      The service control manager waits until the service stops or the specified
      preshutdown time-out value expires


    * **Q**

      **Disable logging**

      This option disables both logging and log rotation.

      Use this option when output from script file is not needed or
      service batch file manages logging on its own.


      **Notice**

      Any eventual log rotation option will not be processed.


    * **R**

      **Enable log rotation by signal**

      When defined, SvcBatch will enable on demand log rotation.

      See [Custom Control Codes](#custom-control-codes)
      section below for more details


    * **U**

      **Unset private environment variables**

      If set this option will disable export of all
      private environment variables to the script program.

      Check [Private Environment Variables](#private-environment-variables)
      section, for the list of exported variables.

    * **Y**

      **Write Y to child console**

      If set this option will write Y character to script interpreter's
      console standard input.

      This option is enabled by default when **cmd.exe** is used,
      and handles `Terminate batch job (Y/N)?` prompt.


    * **0**

      Flags **[0|1|2]** determine how the SvcBatch will handle
      service failure in case it enters a `STOP` state
      without explicit Stop command from the SCM.

      This mode will set the error code when the service
      fails. The error message will be written to
      the Windows Event log and service will enter a stop state.

      If the service was in `RUNNING` state the error
      code will be set to `ERROR_PROCESS_ABORTED`, otherwise
      the error code will be set to `ERROR_SERVICE_START_HANG`.

      You can use this mode to initialize service recovery if
      defined.

      ```no-highlight

      > sc failure myService reset= INFINITE actions= restart/10000

      > sc failureflag myService 1

      ```

      The upper example will restart `myService` service after `10`
      seconds if it enters a stop state without Stop command.

      This is the default mode.

    * **1**

      This mode will not set the error code when the service
      fails. The information message will be written to
      the Windows Event log and service will enter a stop state.

    * **2**

      This mode will not report error code to the SCM when
      the service fails. The error message will be written to
      the Windows Event log.
      SvcBatch will call `exit(ERROR_INVALID_LEVEL)` and terminate
      the current service.




* **-b [path]**

  **Set service base directory**

  This option enables users to explicitly set the
  service base directory.

  By default, service base is set to the directory
  of the service script file.

  If the **path** is not the absolute path, it will
  be resolved relative to the **-h** directory.


* **-c [program]**

  **Use alternative program for running scripts**

  This option allows to use alternative **program** program
  instead default **cmd.exe** to run the scripts.

  For example:

  ```no-highlight
  > svcbatch create ... -c:powershell [ -NoProfile -ExecutionPolicy Bypass -File ] myservice.ps1 ...

  ```

  SvcBatch will execute **powershell.exe** instead **cmd.exe** and pass
  **[ parameters ]** as arguments to the powershell.

  Additional parameters for alternative shell must be enclosed
  inside square brackets before script file and its arguments.

  Parameters for default **cmd.exe** interpreter
  are **/D /E:ON /V:OFF /C**.

  ```no-highlight
  > svcbatch create ... -c:cmd.exe [ /D /E:ON /V:OFF /C ] myservice.bat ...

  ```


* **-k [depth]**

  **Set the nested process kill depth**

  This option sets the **depth** of the process
  tree that SvcBatch will kill on service stop.

  The valid **depth** range is between **0**
  and **4**. By default this value is set to zero.

  This option is used only when manually stopping the
  service. In case the service STOP is initiated by
  system shutdown, SvcBatch will not traverse its
  process tree, but rather let the operating system
  to kill all child processes.

  Use this option if the service script file creates
  a process that do not respond to STOP command, but
  keeps running in the background.


* **-e [name<=value>]**

  **Sets or deletes environment variable**

  This option allows to set the contents of the specified
  environment variable. The content of the **name** environment
  variable is to the **value**.

  For example:

  ```no-highlight
  > svcbatch create ... -E:NOPAUSE=Y -e:CATALINA_BASE=@_W$ ...

  ```

  This will set the `NOPATH` environment variable to `Y`,
  and `CATALINA_BASE` to the value of current working
  directory.

  If the **value** parameter starts with **@_**, followed
  by the single character and **$** it will be evaluated to the
  corresponding runtime value.
  The **@_W$** will be evaluated to the current working directory,
  **@_N$** will set the **value** to the current Service name, etc.


  The supported **@_x$** options are:

  ```no-highlight

    B   Base directory
    D   Program directory
    H   Home directory
    L   Logs directory
    N   Service Name
    P   Program Name
    T   Temp directory
    U   Service UUID
    V   SvcBatch version
    W   Work directory

  ```


  The following example will modify `PATH` environment
  variable for the current process:

  ```no-highlight
  > svcbatch create ... -e "PATH=@ProgramFiles@\SomeApplication;@_H$@;@PATH@" ...

  ```

  In the upper example, each **@** character will be replaced
  with **%** character, and then evaluated.

  This is much safer then using **%** directly, since it
  ensures that it will be evaluated at runtime.

  Each **@@** character pair will be replaced by the
  single **@** character. This allows to use **@** characters
  as part of **value** without replacing them to **%**.

  The `@_H$@` variable will be evaluated to current home
  directory, and `@_W$@` to current work directory.


  ```no-highlight
  > svcbatch create ... -e "SOME_VARIABLE=RUN@@1" ...

  ```
  In the upper example, SvcBatch will set `SOME_VARIABLE`
  environment variable to the value `RUN@1`.



  In case the **value** is empty, the **name** variable
  will be deleted from the current process's environment.

  The following example will delete `SOME_VARIABLE` environment
  variable for the current process:

  ```no-highlight
  > svcbatch create ... -e:SOME_VARIABLE ...

  ```


* **-h [path]**

  **Set service home directory**

  This option enables users to explicitly set the
  home directory.

  The home directory is the location from where all relative
  path parameters will be resolved.

  If not specified, the home directory will set to the
  path of the batch file if it was defined
  as an absolute path. Otherwise the directory of the svcbatch.exe
  will be used as home directory.

  In case the **path** is relative, it will be resolved
  either to the directory of the batch file, if it was
  defined as an absolute path, or to the directory of the
  svcbatch executable.

  The resulting **path** value must exist on the system
  or the service will fail to start, and write an error message
  to the Windows Event log.


* **-t [timeout]**

  **Set stop timeout in seconds**

  This option sets the **timeout** when service receives
  stop or shutdown signal.
  The valid **timeout** range is between `2` and `120`
  seconds (two minutes).

  By default this value is set to `10` seconds.

  Also make sure to check the **WaitToKillServiceTimeout**
  value specified in the following registry key:

  **HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control**

  If the operating system is rebooting, this value is used
  as time limit.


* **-m [number][<.>max stop logs]**

  **Set maximum number of log files**

  In case the **number** contains a single decimal number
  between `1 and 9` it will be used instead default `1 .. 2`.

  ```no-highlight
  > svcbatch create ... -m:4

  ```

  Instead rotating Svcbatch.log from `1 .. 2` it will rotate
  exiting log files from `1 .. 4.`.

  Default maximum number of stop log files is `0` (zero),
  which means that no log rotation will be performed for stop
  script logging. To enable log rotation for stop logging, add
  the dot character to the end of **number**,
  followed by **max stop logs** number.

  ```no-highlight
  > svcbatch create ... -m:4.2 -n myService.log/myService.stop.log

  ```

  This will rotate service log files from `1 .. 4.`,
  and stop log files from `1 .. 2`.


* **-n [log name][</>stop log name]**

  **Set log file name**

  This option allows a user to set the alternate log file names.

  By default SvcBatch will use `SvcBatch.log` as **log name**.
  To redefine default log name use the **-n**
  command option at service install:

  ```no-highlight
  > svcbatch create ... -n myService.log ...

  ```

  If the **-n** argument contains `@` characters,
  it will be treated as a format string
  to our custom `strftime` function.

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
    @d  Day of month as a decimal number (01 - 31)
    @F  Equivalent to @Y-@m-@d
    @H  Hour in 24-hour format (00 - 23)
    @j  Day of the year as a decimal number (001 - 366)
    @m  Month as a decimal number (01 - 12)
    @M  Minute as a decimal number (00 - 59)
    @N  Service Name
    @P  Program Name
    @S  Second as a decimal number (00 - 59)
    @s  Millisecond as a decimal number (000 - 999)
    @w  Weekday as a decimal number (0 - 6; Sunday is 0)
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
  characters the function will fail.

  To enable stop logging for scripts defined by **-s** option,
  add forward slash `/` character to the end of **log name**, followed
  by **stop log name**.

  ```no-highlight
  > svcbatch create ... -n myService.log/myService.stop.log ...

  ```


* **-o [path]**

  **Set service output directory**

  This option allows a user to set the output directory, which is where SvcBatch
  will create any runtime data files.

  If set, the **path** parameter will be used as the
  location where SvcBatch.log files will be created.
  SvcBatch will create a **path** directory if it doesn't exist.

  If not set, SvcBatch will create and use the  **SVCBATCH_SERVICE_WORK\Logs**
  directory as a location for log files and any runtime data
  that has to be created.

  This directory has to be unique for each service instance. Otherwise the
  service will fail if another service already opened SvacBatch.log
  in that location.



* **-p [prefix]**

  **Set prefix for private environment variables**

  This option allows to change default prefix for the
  private runtime environment variables.

  Check [Private Environment Variables](#private-environment-variables)
  section, for the list of exported variables.

  To change default **SVCBATCH_SERVICE** prefix, add
  **-p:prefix** to your service configuration.


  The following example will cause SvcBatch to
  export **MYSERVICE_NAME** instead default **SVCBATCH_SERVICE_NAME**, etc.

  ```no-highlight
  > svcbatch create ... -p:MYSERVICE ...

  ```


  If **-f:U** was added to the service's configuration
  this option will have no meaning.



* **-r [rule]**

  **Rotate logs by size or time interval**

  SvcBatch can automatically rotate log files beside
  explicit `sc.exe control [service name] 234` command.

  Depending on the **rule** parameter service can rotate
  log files at desired interval, once a day at specific time
  or when log file gets larger then defined size.

  Time and size values can be combined, which allows
  to rotate logs at specific time or size which ever first.
  For example one can define **rule** so that rotate logs
  is run each day at `17:00:00` hours or if log files gets
  larger then `100K` bytes.

  To combine multiple values use the **+** character as
  value separator. The order is important.

  ```no-highlight
    <@Time><+Size>
  ```

  ```no-highlight
  > svcbatch create ... -r:@17:00:00+100K

  ```

  If time is given without a colons, SvcBatch will use it
  as minutes between log rotation.

  ```no-highlight
  >svcbatch create ... -r:@90+200K

  ```

  The upper example will rotate logs each `90` minutes. In case
  log file gets larger the 200Kbytes within that interval,
  it will be rotated as well. In that case internal timer
  will be reset and next rotation will occur after `90` minutes.

  In case **rule** parameter is `@0` SvcBatch will rotate
  log files each day at midnight. This is the same as
  defining `-r:@00:00:00`.

  In case **rule** parameter is `@60` SvcBatch will rotate
  log files every full hour.

  In case **rule** parameter for rotation based on log file size
  is less then `1K` (1024 bytes), SvcBatch will not rotate logs by size.

  The **rule** parameter uses the following format:

  ```no-highlight
      <[@hh:mm:ss|@minutes]><+size[B|K|M|G]>
  ```



* **-s [script]**

  **Execute script file on service stop or shutdown**

  If defined, on shutdown or stop event the service
  will create separate `svcbatch.exe` process and call **script**.
  The purpose of that file is to use some sort of `IPC` mechanism
  and signal the service to exit.

  This is particularly useful for services that do not handle
  `CTRL_C_EVENT` or have specific shutdown requirements.

  In case the **script** starts with **./** or **.\\**,
  SvcBatch will use the string following the **./**
  as script file without checking for its existence.

  In case the **script** equals to **@**,
  SvcBatch will use the main service script file for shutdown,
  add pass **stop** string as the argument to that script file,
  unless additional argument(s) were not defined.

  In case the **script** starts with **@** character,
  SvcBatch will use the string following the **@**
  as application that will be executed instead default
  script interpreter.


  To set additional arguments for stop script
  enclose them inside square brackets `[ ... ]`.


  ```no-highlight
  > svcbatch create ... -s:stop.bat [ --connect --command=:shutdown ] ...
  ...
  > svcbatch create ... -s:@ ...
  ...
  > svcbatch create ... -s:@ [ used instead default stop ] ...
  ...
  > svcbatch create ... -s:@shutdown.exe [ param1 param2 ] ...

  ```



* **-w [path]**

  **Set service working directory**

  This option enables users to explicitly set the working
  directory. When batch file is executed its current directory
  is set to this path.

  If not specified, the working directory is set
  to the home directory defined using **-w** option.

  Check **-h** command option for more details.

  If the **path** is not the absolute path, it will
  be resolved relative to the **-h** directory.


* **-x [prefix]**

  **Set posix path prefix**

  This option enables to set the prefix for private
  environment values for posix shells.

  If set the path values of the private environment
  variables will be converted to posix format and
  prepend by the **prefix**.

  Use the **-x:/** for mingw/msys or **-x:/cygroot/** for cygwin
  shells set by **-c** command option.

  For example if the **SVCBATCH_SERVICE_TEMP** variable is set
  to the **C:\\Temp**

  ```no-highlight

  > svcbatch create ... -x:/ -c:bash.exe ...
  ...

  ```

  The bash.exe process will have the **SVCBATCH_SERVICE_TEMP**
  variable set to the **/c/Temp**




  **Notice**

  Use this option only if running scripts using posix shell.
  Otherwise the exported private environment variables will
  be unusable.


## Private Environment Variables

SvcBatch sets a few private environment variables that
provide more info about running environments to batch files.
Those variable by default have **SVCBATCH_SERVICE** prefix.

Here is the list of environment variables that
SvcBatch sets for each instance.

* **SVCBATCH_SERVICE_BASE**

  This variable is set to the directory of the service
  script file.

* **SVCBATCH_SERVICE_HOME**

  This variable is set to the service home directory.

* **SVCBATCH_SERVICE_LOGS**

  This variable is set to the service's log directory.

  In case the logging is disabled, by using **-f:Q**
  command option, this variable is set to the **SVCBATCH_SERVICE_WORK**
  directory.

* **SVCBATCH_SERVICE_NAME**

  This variable is set to the actual service name
  defined with `svcbatch create [service name] ...`

  ```batchfile
  @echo off
  rem
  rem Simple example
  rem

  echo Running service %SVCBATCH_SERVICE_NAME%

  ```

* **SVCBATCH_SERVICE_TEMP**

  This variable is set to the temp directory
  of the account that was used to start the service.

  SvcBatch uses GetTempPath function that checks for
  the existence of environment variables in the following
  order and uses the first path found:

  * The path specified by the TMP environment variable.
  * The path specified by the TEMP environment variable.
  * The path specified by the USERPROFILE environment variable.
  * The Windows directory.

  SvcBatch verifies that the path exists, and tests to see if the
  current process has read and write access rights to the path.

  Service will fail to start in case the temp directory does
  not exist or if it misses read and write access rights.

* **SVCBATCH_SERVICE_UUID**

  This is the service's unique identifier in following hexadecimal format
  `abcd-01234567-89ab-cdef-0123-456789abcdef`.
  The first four digits are current process id, and remaining digits
  are randomly generated at service startup.

  The **SVCBATCH_SERVICE_UUID** environment variable can be used
  inside batch file when unique identifier is needed.

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

* **SVCBATCH_SERVICE_WORK**

  This variable is set to the service working directory.

  The working directory is set to **SVCBATCH_SERVICE_HOME**
  directory, unless the **-w** command option was configured.

  This variable is set as current directory for the
  shell process launched from SvcBatch, and as base directory
  for **SVCBATCH_SERVICE_LOGS** in case the **-o** parameter
  was defined as relative path.


## Custom Control Codes

SvcBatch can send `CTRL_BREAK_EVENT` signal to its child processes.

This allows programs like **java.exe** to act upon that signal.
For example JVM will dump it's full thread stack in the same way
as if user hit the `CTRL` and `Break` keys in interactive console.

Use `svcbatch control [service name] 233` to send
`CTRL_BREAK_EVENT` to all child processes.
Again, as with log rotate, the **233** is our custom control code.

* **Important**

  This option is enabled at service install time with **-f:C** command
  switch option.

  Do not send `CTRL_BREAK_EVENT` if the batch file runs a process
  that does not have a custom `CTRL_BREAK_EVENT` console handler.
  By default the process will exit and the service will either fail.




## Stop and Shutdown

When you type `sc.exe stop myservice` or when your machine gets into the
shutdown state, SvcBatch running as a service will receive
stop or shutdown event. SvcBatch will send `CTRL_C_EVENT` to its
child processes or run shutdown batch file in case
**-S [batchfile]** was defined at install time.

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

  ```no-highlight
  > svcbatch.exe version
  SvcBatch 1.2.3.4 ...

  >
  ```

Make sure to use the correct information when filing
bug reports.

## Error Logging

SvcBatch logs any runtime error to Windows Event Log.
Use Windows **Event Viewer** and check `Windows Logs/Application/SvcBatch`
events.



# License

The code in this repository is licensed under the [Apache-2.0 License](LICENSE.txt).

