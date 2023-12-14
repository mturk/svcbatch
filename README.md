# SvcBatch: Overview

SvcBatch is a program that allows users to run batch files as Windows service.

The program's main goal is to run any application as a Windows
service by using a batch file wrapper as an application launcher.
This is particularly useful when running Java applications or
for scripts written in Perl, Python, Ruby, etc... without the need
for a specialized service wrapper.

SvcBatch was designed to be simple to use and lightweight, with a small
memory footprint. Its only dependency is win32 API, and only has
around 5K lines of **C** code. There are no configuration
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
  - [Stop and Shutdown](#stop-and-shutdown)
  - [Version Information](#version-information)
  - [Error Logging](#error-logging)
- [Limitations](#limitations)
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

  To manually start the service use either `svcbatch`, `sc`
  or Microsoft Services GUI application.

  SvcBatch will append any additional arguments from the
  service start application to the script file's existing arguments
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


## Command Line Options

SvcBatch command line options allow users to customize
service deployments. Options are case insensitive and
defined with **/** or **-** as command switch when running in
service mode, or with **--** for SvcBatch service management.
This means that `-h or /H` and `--Wait or --wait`
can be used interchangeably.

Check the [managing](docs/manage.md) for more info about
command line options when SvcBatch is used for managing services.

Command line option value can be either the rest of the
command option or the entire next argument.

In case the value is the rest of the command option,
the command switch must be **/** and the
character after the option must be **:** character,
followed by the actual option value.

For example:

```no-highlight
> svcbatch create myService ... /L:log\directory ...

Is the same as

> svcbatch create myService ... -l log\directory ...

```

After handling switches SvcBatch will pass remaining
arguments to the script interpreter.

The first argument is treated as script file.
If the first argument is not the absolute path, it will
be resolved relative to the service home directory.


If there are no additional arguments, SvcBatch will
append `.bat` to the running Service Name.
In that case, if `ServiceName` contain any of the
invalid file name characters `/\:;<>?*|"`,
the service will fail and error message will be
reported to Windows Event log.


Although command options are case insensitive,
use capital letters with **/** command switch,
and lowercase letters with **-** command switch.





* **F [features]**

  **Set runtime features**

  This option sets various runtime features.
  The **features** parameter is a combination of
  one or more characters, where each character sets
  the particular feature option.

  Feature options are case sensitive, and can be
  listed in any order.

  ```no-highlight
      <B><L><N><P><Q><R><T><W><Y><0|1|2>
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


    * **L**

      **Use local time**

      This option causes all logging and rotation
      to use local instead system time.


    * **N**

      **Do not export private environment variables**

      If set this option will disable export of all
      private environment variables to the script program.

      Check [Private Environment Variables](#private-environment-variables)
      section, for the list of exported variables.



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

      **Enable log rotation by control code**

      When defined, SvcBatch will accept custom **234**
      control code and initiate log rotation when this control
      is signaled.

      Note that **234** is our custom service control code.
      Number **234** has been randomly chosen, since win32
      API requires that this number must be larger then `127` and
      lower then `255`.

      To initiate log rotation manually use the following:

      ```no-highlight
      > svcbatch control myService 234

      ```

      In case the last log rotation was less then `2` minutes ago,
      or if there was no data written to the log file from the last
      rotation, SvcBatch will not rotate the logs.


    * **T**

      **Truncate log on rotation**

      If set this option will truncate existing
      log file on log rotation instead creating
      a new log file.



    * **W**

      **Use Windows Long Path Names**

      By default Windows paths are limited to **260** characters,
      unless prefixed by **\\?\\**. SvcBatch adds that prefix for
      all directory names larger the **248** characters, and file
      names larger then **MAX_PATH** (defined as `260`).

      This option enables long path behavior if enabled
      on the system running Windows 10, Version 1607, and later.

      To enable the new long path behavior, the value of
      the **LongPathsEnabled** must be set to **1** in
      the **HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\FileSystem**
      registry key.

      This option can be useful for new applications targeting
      Windows 10 platform, without the overhead of prefixing each
      path or file name parameter with **\\?\\**.


    * **Y**

      **Write Y to child console**

      If set this option will write `Y\r\n` characters to script interpreter's
      console standard input.

      This option is enabled by default when **cmd.exe** is used,
      and handles `Terminate batch job (Y/N)?` prompt.


    * **0**

      Flags **[0|1|2]** determine how the SvcBatch will handle
      service failure in case it enters a `STOP` state
      without explicit Stop command from the SCM.

      This mode will not set the error code when the service
      fails. The information message will be written to
      the Windows Event log and service will enter a stop state.

      The typical usage of this feature is for the services
      that run for some time and then stop.


    * **1**

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

      **This is the default mode.**


    * **2**

      This mode will not report error code to the SCM when
      the service fails. The error message will be written to
      the Windows Event log.
      SvcBatch will call `exit(ERROR_INVALID_LEVEL)` and terminate
      the current service.




* **C [program][<[> parameters <]>]**

  **Use alternative program for running scripts**

  This option allows to use alternative **program** program
  instead default **cmd.exe** to run the scripts.

  For example:

  ```no-highlight
  > svcbatch create ... /C:powershell /C:[ -NoProfile -ExecutionPolicy Bypass -File ] myservice.ps1 ...

  ```

  SvcBatch will execute **powershell.exe** instead **cmd.exe** and pass
  **[ parameters ]** as arguments to the powershell.

  Additional **parameters** for alternative shell must be enclosed
  inside square brackets before script file and its arguments.

  Parameters for default **cmd.exe** interpreter
  are **/D /E:ON /V:OFF /C**.

  ```no-highlight
  > svcbatch create ... /C:cmd.exe [ /D /E:ON /V:OFF /C ] myservice.bat ...

  ```

  If the **program** parameter contains any **@** or **%** characters,
  SvcBatch will evaluate all environment strings in the **program** parameter
  and use it as application name.

  In case the **program** parameter is not an absolute path,
  SvcBatch will search the current environment for the **program[.exe]**
  application.



* **K [depth]**

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


* **E [name=value]**

  **Sets environment variable**

  This option allows to set the contents of the specified
  environment variable. The content of the **name** environment
  variable is to the **value**.

  For example:

  ```no-highlight
  > svcbatch create ... /E:NOPAUSE=Y /E:CATALINA_BASE=$WORK ...

  ```

  This will set the `NOPAUSE` environment variable to `Y`,
  and `CATALINA_BASE` to the value of the current working
  directory.

  SvcBatch will evaluate **value** parameter and replace each
  **$variable** string with corresponding runtime value.

  If the **$variable** does not exits, it will be replaced by
  empty string.

  The **$WORK** will be evaluated to the current working directory,
  **$NAME** will set the **value** to the current Service name, etc.


  The runtime **variable** values are:

  ```no-highlight

    BASENAME    Program application name
    BASE        Base directory
    DIRNAME     Program directory
    HOME        Home directory
    LOGS        Logs directory
    NAME        Service Name
    PROCESSID   Program ProcessId
    RELEASE     SvcBatch release version
    UUUID       Service UUID
    VERSION     SvcBatch version number
    WORK        Work directory

  ```



  The following example will modify `PATH` environment
  variable for the current process:

  ```no-highlight
  > svcbatch create ... -e "PATH=$ProgramFiles\SomeApplication;$PATH" ...

  ```

  In the upper example, each **$variable** will be evaluated
  to the current runtime value.

  Each **$$** character pair will be replaced by the
  single **$** character. This allows to use **$** characters
  as part of **value** without evaluating them.


  ```no-highlight
  > svcbatch create ... /E "SOME_VARIABLE=RUN$$1" ...

  ```
  In the upper example, SvcBatch will set `SOME_VARIABLE`
  environment variable to the value `RUN$1`.



  SvcBatch will evaluate **E** command options in order they
  are defined in service configuration.

  In case the **/F:N** feature was not defined, SvcBatch
  will first set all [Private Environment Variables](#private-environment-variables)

  After that it will evaluate and set all **/E:name=value** variables.



* **EE [variables]**

  **Export private environment variables**

  This option exports various runtime environment variables.
  The **variables** parameter is a combination of
  one or more characters, where each character sets
  the particular environment variable.

  Variables are case sensitive, and can be
  listed in any order.

  ```no-highlight

    A   Program application name
    B   Base directory
    D   Program directory
    H   Home directory
    L   Logs directory
    N   Service Name
    P   Program ProcessId
    R   SvcBatch release version
    U   Service UUID
    V   SvcBatch version number
    W   Work directory

  ```

  Each variable name will be evaluated at runtime
  using either default or prefix or the one defined
  by the **/EP:PREFIX** command option.

  ```no-highlight

    A  ...  [PREFIX]_BASENAME
    B  ...  [PREFIX]_BASE
    D  ...  [PREFIX]_DIRNAME
    H  ...  [PREFIX]_HOME
    L  ...  [PREFIX]_LOGS
    N  ...  [PREFIX]_NAME
    P  ...  [PREFIX]_PROCESSID
    R  ...  [PREFIX]_RELEASE
    U  ...  [PREFIX]_UUID
    V  ...  [PREFIX]_VERSION
    W  ...  [PREFIX]_WORK

  ```

  The following example will export `SVCBATCH_PROCESSID`
  and `SVCBATCH_VERSION` environment variables with the
  values set to their corresponding runtime values.

  ```no-highlight
  > svcbatch create ... /EE:PV ...

  ```

  This feature is usually used when the **/F:N** feature
  is defined, or to export specific runtime value(s).

  The following example will only export service's home
  and work directories as `MYSERVICE_HOME` and `MYSERVICE_WORK`
  environment variables:

  ```no-highlight
  > svcbatch create myService ... /F:N /EP:@ /EE:HW ...

  ```



* **EP [prefix]**

  **Sets private environment variables prefix**

  By default SvcBatch uses uppercase program name as prefix
  for private environment variables names.

  Check [Private Environment Variables](#private-environment-variables)
  section, for the list of exported variables.

  To change default **SVCBATCH** prefix, add
  **/EP:prefix** to your service configuration.


  The following example will cause SvcBatch to
  export **ASERVICE_NAME** instead default **SVCBATCH_NAME**, etc.

  ```no-highlight
  > svcbatch create ... /EP:ASERVICE ...

  ```

  In case the **prefix** is a single **@** character,
  SvcBatch will use service name as private environment prefix.

  The following example will cause SvcBatch to
  export **MYSERVICE_NAME** instead default **SVCBATCH_NAME**, etc.

  ```no-highlight
  > svcbatch create myService ... /EP:@ ...

  ```


  **Notice**

  If **/F:N** was added to the service's configuration
  this option will ensure that all private environment
  variables are removed from the current process.



* **H [path]**

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


* **L [path]**

  **Set service log directory**

  This option allows a user to set the log directory, which is where
  SvcBatch will create any runtime log files.

  If set, the **path** parameter will be used as the
  location where SvcBatch.log files will be created.
  SvcBatch will create a **path** directory if it doesn't exist.

  If not set, SvcBatch will create and use the  **SVCBATCH_WORK\Logs**
  directory as a location for log files that has to be created.

  If the **path** is not the absolute path, it will
  be resolved relative to the **W** directory.

  **Notice**

  This directory has to be unique for each service instance. Otherwise the
  service will fail if another service already opened SvacBatch.log
  in that location.



* **LN [name]**

  **Set log file name**

  This option allows a user to set the alternate log file names.

  By default SvcBatch will use `SvcBatch.log` as **name**.
  To redefine default log name use the **LN**
  command option at service install:

  ```no-highlight
  > svcbatch create ... /LN:myService.log ...

  ```

  If the **name** parameter contains `@` characters,
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
    @S  Second as a decimal number (00 - 59)
    @s  Millisecond as a decimal number (000 - 999)
    @w  Weekday as a decimal number (0 - 6; Sunday is 0)
    @y  Year without century, as decimal number (00 - 99)
    @Y  Year with century, as decimal number
    @0  Current counter as a decimal number (0    -    9)
    @2  Current counter as a decimal number (00   -   99)
    @4  Current counter as a decimal number (0000 - 9999)

  ```

  Make sure that log names contain only valid file name characters.
  The following are reserved characters:

  ```no-highlight
    <  (less than)
    >  (greater than)
    :  (colon)
    ;  (semicolon)
    "  (double quote)
    /  (forward slash)
    \  (backslash)
    |  (vertical bar or pipe)
    ?  (question mark)
    *  (asterisk)
  ```

  In case the result from `strftime` contains any of the reserved
  characters the function will fail.


* **LM [number]**

  **Set maximum number of log files**

  In case the **number** contains a single decimal number
  between `1 and 9` it will be used instead default `1 .. 2`.

  ```no-highlight
  > svcbatch create ... /LM:4

  ```

  Instead rotating Svcbatch.log from `1 .. 2` it will rotate
  exiting log files from `1 .. 4.`.



* **LR [rule]**

  **Rotate logs by size or time interval**

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
    <@Time><+><Size>
  ```

  ```no-highlight
  > svcbatch create ... /LR:@17:00:00+100K

  ```

  If time is given without a colons, SvcBatch will use it
  as minutes between log rotation.

  ```no-highlight
  >svcbatch create ... /LR:@90+200K

  ```

  The upper example will rotate logs each `90` minutes. In case
  log file gets larger the 200Kbytes within that interval,
  it will be rotated as well. In that case internal timer
  will be reset and next rotation will occur after `90` minutes.

  In case **rule** parameter is `@0` SvcBatch will rotate
  log files each day at midnight. This is the same as
  defining `/LR:@00:00:00`.

  In case **rule** parameter is `@60` SvcBatch will rotate
  log files every full hour.

  In case **rule** parameter for rotation based on log file size
  is less then `1K` (1024 bytes), SvcBatch will not rotate logs by size.

  The **rule** parameter uses the following format:

  ```no-highlight
      <[@hh:mm:ss|@minutes]><+><size[B|K|M|G]>
  ```



* **S [script][<[> arguments <]>]**

  **Execute script file on service stop or shutdown**

  If defined, on shutdown or stop event the service
  will create separate `svcbatch.exe` process and call **script**.
  The purpose of that file is to use some sort of `IPC` mechanism
  and signal the service to exit.

  This is particularly useful for services that do not handle
  `CTRL_C_EVENT` or have specific shutdown requirements.

  In case the **script** equals to **NUL**, SvcBatch
  will only pass additional arguments to the script interpreter.
  Service will fail to start if the additional argument(s) were not defined.

  In case the **script** starts with **./** or **.\\**,
  SvcBatch will use the string following the **./** or **.\\**
  as script file without checking for its existence.

  In case the **script** equals to **@**,
  SvcBatch will use the main service script file for shutdown,
  add pass **stop** string as the argument to that script file,
  if the additional argument(s) were not defined.


  To set additional **arguments** for stop script
  enclose them inside square brackets `[ ... ]`.


  ```no-highlight

  > svcbatch create ... /S:stop.bat /S:[ --connect --command=:shutdown ] ...
  ...
  > svcbatch create ... /S:@ ...
  ...
  > svcbatch create ... /S:@ [ used instead default stop ] ...

  ```


* **SM [number]**

  **Set maximum number of stop log files**


  Default maximum number of stop log files is `0` (zero),
  which means that no log rotation will be performed for stop
  script logging. To enable log rotation for stop logging, set
  the **number** parameter value between `1 and 9`.

  ```no-highlight
  > svcbatch create ... /SM:2 /SN:myService.stop.log

  ```

  This will rotate stop log files from `1 .. 2`.

  **Notice**

  If this option is defined, and **SN** option was not defined,
  SvcBatch will use default `SvcBatch.stop.log` as stop log name.


* **SN [name]**

  **Set stop log file name**

  This option allows a user to enable stop logging for scripts
  defined by **S** option.

  ```no-highlight
  > svcbatch create ... /SN:myService.stop.log ...

  ```

  The **name** parameter is handled the same as
  described with **LN** command option.



* **ST [timeout]**

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


* **T [path]**

  **Set temp directory**

  This option allows a user to set different temp directory
  then the one defined by the service account environment.

  If the **path** is not the absolute path, it will
  be resolved relative to the **W** directory.

  SvcBatch will set **TEMP** and **TMP** environment
  variables to that directory.

  In case the directory specified by **path** parameter
  does not exists, it will be created.


* **W [path]**

  **Set service working directory**

  This option enables users to explicitly set the working
  directory. When batch file is executed its current directory
  is set to this path.

  If not specified, the working directory is set
  to the home directory defined using **H** option.
  Check **H** command option for more details.

  If the **path** is not the absolute path, it will
  be resolved relative to the **H** directory.


  **Notice**

  If defined, the working directory must exist and have
  read and write access rights for the current process.




## Private Environment Variables

SvcBatch sets a few private environment variables that
provide more info about running environments to batch files.
Those variable by default have **SVCBATCH** prefix,
determined from the program executable name.

Here is the list of environment variables that
SvcBatch sets for each instance:


* **SVCBATCH_BASE**

  This variable is set to the directory of the service
  script file.


* **SVCBATCH_HOME**

  This variable is set to the service home directory.


* **SVCBATCH_LOGS**

  This variable is set to the service's output directory.

  In case the logging is disabled, by using **/F:Q**
  command option, this variable is set to the **SVCBATCH_WORK**
  directory.


* **SVCBATCH_NAME**

  This variable is set to the actual service name
  defined with `svcbatch create [service name] ...`

  ```batchfile
  @echo off
  rem
  rem Simple example
  rem

  echo Running service %SVCBATCH_NAME%

  ```

* **SVCBATCH_UUID**

  This is the service's unique identifier in following hexadecimal format
  `abcd-01234567-89ab-cdef-0123-456789abcdef`.
  The first four digits are current process id, and remaining digits
  are randomly generated at service startup.

  The **SVCBATCH_UUID** environment variable can be used
  inside batch file when unique identifier is needed.

  ```batchfile
  rem
  rem Create unique temp directory
  rem
  md "%TEMP%\%SVCBATCH_UUID%"
  ...
  ... do some work using that directory
  ...
  rd /S /Q "%TEMP%\%SVCBATCH_UUID%"

  ```

* **SVCBATCH_WORK**

  This variable is set to the service working directory.

  The working directory is set to **SVCBATCH_HOME**
  directory, unless the **W** command option was configured.

  This variable is set as current directory for the
  shell process launched from SvcBatch, and as base directory
  for **SVCBATCH_LOGS** in case the **L** parameter
  was defined as relative path.


**Notice**

  In case you rename SvcBatch executable from **svcbatch.exe**
  to for example **myservice.exe**, SvcBatch will use **MYSERVICE**
  as prefix for those variables. In that case it will export
  **MYSERVICE_NAME**, **MYSERVICE_HOME**, etc.

  In case the **/EP:@** or **/EP:PREFIX** command option is
  defined, it will take precedence over this feature.

**Important**

  Make sure to use only ASCII alphanumeric and underscore (`_`)
  characters as executable name, or the service will fail to start.



## Stop and Shutdown

When you type `sc.exe stop myservice` or when your machine gets into the
shutdown state, SvcBatch running as a service will receive
stop or shutdown event. SvcBatch will send `CTRL_C_EVENT` to its
child processes or run shutdown batch file in case
**/S [batchfile]** was defined at install time.

It is up to the application started from batch file to
handle this event and do any cleanup needed and then exit.
By default, on startup, SvcBatch writes 'Y' to `cmd.exe` stdin,
to handle that obnoxious `Terminate batch job (Y/N)?` prompt.
If batch file or any of downstream processes do not exit within stop timeout,
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
Use Windows **Event Viewer** and check `Windows Logs/Application/YourServiceName`
events.



# Limitations

SvcBatch have limits for the following features:


* **Service Name**

  The Service Name must consists of only ASCII characters
  except the following set:

  ```no-highlight
    <  (less than)
    >  (greater than)
    :  (colon)
    ;  (semicolon)
    "  (double quote)
    /  (forward slash)
    \  (backslash)
    |  (vertical bar or pipe)
    ?  (question mark)
    *  (asterisk)
  ```

  Service Name must start with alphanumeric character,
  and its length is limited to **255** characters.

  Service will **fail** to start if the upper criteria is not met.


* **Program File Name**

  Default Program File Name is **svcbatch.exe**.

  Program File Name can contain only alphanumeric ASCII
  characters including underscore (`_`) character,
  and must end with **.exe** file extension.


* **File name length**

  The maximum file name length must be less then
  **2040** characters, and is defined by the
  `#define SVCBATCH_PATH_SIZ 2040` macro
  inside [svcbatch header file](svcbatch.h)



* **Maximum number of arguments**

  The maximum number of arguments that can be passed
  to the command processor is limited to **32**,
  and is defined by the `#define SVCBATCH_MAX_ARGS 32`
  macro inside [svcbatch header file](svcbatch.h)

  This option also defines the maximum number of
  parameters passed to the command interpreter when
  **C** command option is used.




# License

The code in this repository is licensed under the [Apache-2.0 License](LICENSE.txt).

