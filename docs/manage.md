# Managing SvcBatch Services

Starting with version **3.0.0** SvcBatch has a Service management
code that contains a subset of Microsoft's `sc.exe` utility to
create, configure, manage, and delete services.

# Table of Contents

- [Table of Contents](#table-of-contents)
- [Commands](#commands)
  - [Help](#help)
  - [Version](#help)
  - [Create](#create)
  - [Config](#config)
  - [Control](#control)
  - [Delete](#delete)
  - [Start](#start)
  - [Stop](#stop)
- [Command Options](#command-options)
  - [Common options](#common-options)
  - [Create and Config options](#create-and-config-options)
  - [Start options](#start-options)
  - [Stop options](#stop-options)

# Commands

Every command except [Help](#help) and [Version](#version)
must be followed by the **Service Name**.

## Help

This command prints help to the console output.

  ```no-highlight
  > svcbatch help

    Usage:
      SvcBatch [command] [service name] <option1> <option2>...

        Commands:
          Create.....Creates a service.
          Config.....Changes the configuration of a service.
          Control....Sends a control to a service.
          Delete.....Deletes a service.
          Help.......Print this screen and exit.
                     Use Help [command] for command help.
          Start......Starts a service.
          Stop.......Sends a STOP request to a service.
          Version....Print version information.
  ```

To get more detailed help for the individual command option
add command name as an argument to the help command.

  ```no-highlight
  > svcbatch help delete

    Description:
      Deletes a service entry from the registry.
    Usage:
      SvcBatch delete [service name] <options ...>

        Options:
          --quiet            Quiet mode, do not print status or error messages.
```

## Version

Use this command to display build and version information
to the console output.

  ```no-highlight
  > svcbatch version
  SvcBatch 1.2.3.4 ...
  ```


## Create

Creates a service.

The following command:

  ```no-highlight
  > svcbatch create myService
    Service Name : myService
         Command : Create
                 : SUCCESS
         STARTUP : Manual (3)
  ```

will create **myService** service presuming
that the script file is **myService.bat**.

Check [Create and Config options](#create-and-config-options) for
options that can be used to customize services creation.

## Config

Changes the configuration of a service.

This command should be used only when the
service needs a runtime configuration change
or as part of service's installation process.

All the configuration for the service should
be done within **create** command.


## Control

Sends a control to a service.

This command can be used to send Custom control
codes to the SvcBatch service.

For example `234` is SvcBatch custom control code
that will signal log rotation. In case the log
rotation is not ready the following will be displayed:


  ```no-highlight
  > svcbatch control myService 234
    Service Name : myService
         Command : Control
                 : FAILED
            LINE : 4982
           ERROR : 1061 (0x425)
                   The service cannot accept control messages at this time
                   234
  ```


## Delete

Deletes a service.

The following command:

  ```no-highlight
  > svcbatch delete myService
    Service Name : myService
         Command : Delete
                 : SUCCESS
  ```

will delete **myService** service.

The service must be stopped before calling this command.
In case the service is running the delete command
will return something as follows:

  ```no-highlight
  > svcbatch delete myService
    Service Name : myService
         Command : Delete
                 : FAILED
            LINE : 4688
           ERROR : 1056 (0x420)
                   An instance of the service is already running
                   Stop the service and call Delete again
  ```


## Start

Starts a service.

The following command:

  ```no-highlight
  > svcbatch start myService
    Service Name : myService
         Command : Start
                 : SUCCESS
                   1047 ms
             PID : 7048
  ```

will start the **myService** service and return
when the service is in the running state.


## Stop

Sends a STOP request to a service.

The following command:

  ```no-highlight
  > svcbatch stop myService
    Service Name : myService
         Command : Stop
                 : SUCCESS
                   3047 ms
        EXITCODE : 0 (0x0)
  ```

will send a STOP request to the **myService** service
and return when the service is stopped.

In case the service did not stop within `--wait[:seconds]`
interval, SvcBatch will report something similar to:

  ```no-highlight
  > svcbatch stop myService --wait=2
    Service Name : myService
         Command : Stop
                 : FAILED
                   2047 ms
            LINE : 4853
           ERROR : 1053 (0x41d)
                   The service did not respond to the start or control request in a timely fashion
  ```




# Command options

SvcBatch management command line options are case insensitive.
This means that `--binPath, --BinPath or --BINPATH` can be used
for the same option.

Command options arguments can be part of the command option
separated by `=` or `:` character. This means that
either `--start=auto`, `--start:auto` or `--start auto` can be used.



## Common options

* **--quiet**

  **Disable printing of status messages**

  By default SvcBatch will print the status message
  to the current console. Use this option to disable
  printing both status or error messages.


## Create and Config options

* **--binPath [path]**

  **Set service binary path**

  This option sets the service's binary path name
  to the .exe file pointed by **path** argument.

  By default SvcBatch will set the service's BinaryPathName
  to the current path of the svcbatch.exe executable
  used when creating the service.

  Use this option when svcbatch.exe is shared between
  multiple services or if its inside a different path
  then the svcbatch.exe used for creating a service.

  ```no-highlight
  > svcbatch create myService --binpath "@ProgramFiles@\SvcBatch\svcbatch.exe" ...
  >
  ```

  **Notice**

  In case the **path** contains any `@` character(s) they will be
  replaced by `%` character. In case there is a `%` character
  in the resulting **path**, SvcBatch will expand environment
  variable strings and replace them with the values defined
  for the current user.

  In case the resulting **path** contains space characters, SvcBatch
  will properly quote the **path**.

  However if the **path** starts with `"` character, no quoting
  will be performed, since the **path** parameter is presumed
  to be properly quoted.


* **--description [description]**

  **Sets the description string for a service**

  This option sets the description string for a service.

  ```no-highlight
  > svcbatch config myService --description "This is My Service"
  >
  ```

* **--depend [dependencies]**

  **Sets the service dependencies**

  This option sets service dependencies.
  The **dependencies** string is list of services
  separated by `/` (forward slash) character.

  ```no-highlight
  > svcbatch config myService --depend=Tcpip/Afd
  >
  ```

  The myService will depend on `Tcpip` and `Afd` services.


* **--displayname [name]**

  **Sets the service display name**

  This option sets the DisplayName for a service.

  ```no-highlight
  > svcbatch create myService --displayName "My Service"
  >
  ```

* **--username [name]**

  **Sets the service account name**

  This option sets the **name** of the account under which
  the service should run.

  By default when the service is created it will
  be run under **LocalSystem** account.

  The **name** parameter can be either the full
  account name or:

  * **--username=0**

    This option is the same as **--username .\LocalSystem**

  * **--username=1**

    This option is the same as **--username "NT AUTHORITY\LocalService"**

  * **--username=2**
    This option is the same as **--username "NT AUTHORITY\NetworkService"**


* **--password [password]**

  **Sets the password for the service account name**

  This option sets the **password** to the account name
  specified by the **--username [name]** parameter.
  Do not specify this option if the account has no password
  or if the service runs in the LocalService, NetworkService,
  or LocalSystem account.


* **--privs [privileges]**

  **Changes the required privileges setting of a service**

  The privilege settings take effect when the
  service process starts due to the first service in
  the process being started. At that time, the Service
  Control Manager (SCM) computes the union of all privileges
  required by all services that will be hosted in the same
  process and then creates the process with those
  privileges. An absence of this setting is taken to imply
  that the service requires all the privileges that
  the security subsystem allows for the process running
  in the service's configured account.

  The **privileges** are separated by `/` (forward slash) character.

  ```no-highlight
  > svcbatch config myService --privs SeBackupPrivilege/SeRestorePrivilege
  >
  ```

* **--start [type]**

  **Sets the service start options**

  The **type** parameter is case insensitive and
  can be one of the following values.

    * **auto|automatic**

    A service started automatically by the service
    control manager during system startup.

    * **demand|manual**

    A service started by the service control manager
    when a process calls the StartService function.
    This is default value.


    * **disabled**

    A service that cannot be started. Attempts to start the
    service result in the error code ERROR_SERVICE_DISABLED.


  ```no-highlight
  > svcbatch config myService --start=disabled
  >
  ```


When the create or config command encounters the unknown option,
SvcBatch will stop option processing and save this and
any additional argument inside Windows Registry. Those
arguments will be used as command line arguments for the
SvcBatch service.

For example:

  ```no-highlight
  > svcbatch create myService
  ...
  > svcbatch config myService --start=auto /F:L ...
  ```

Since the `/F:L` is not valid config or create option,
it will be used as argument to svcbatch.exe when the
service is started, as well as any following argument.



## Start options

* **--wait[=seconds]**

  **Wait for service to start**

  If the service is not running, SvcBatch will wait
  up to **seconds** for service to start.

  The **seconds** is optional parameter with value
  between `0` and `180`. If not provided as part of
  the **--wait** option, the maximum
  value of `180` seconds will be used.

  Use **--wait=0** option to return without waiting for
  service to enter the running state.


* **arguments ...**

  **Optional arguments for service start**

  If defined **arguments** will be passed to
  the service on startup.

  ```no-highlight
  > svcbatch start myService --wait arg1 arg2 ...
  >
  ```


## Stop options

* **--wait[=seconds]**

  **Wait for service to stop**

  If the service is running, SvcBatch will wait
  up to **seconds** for service to stop.

  The **seconds** is optional parameter with value
  between `0` and `180`. If not provided as part of
  the **--wait** option, the maximum value of `180` seconds will be used.

  Use **--wait=0** option to return without waiting for
  service to enter the stopped state.


* **reason**

  **Optional reason code number for service stop**

  The optional **reason** code number for service stop
  formed with the following elements in the format:

  **Flag:Major reason:Minor reason**

  E.g. **1:2:8** means Hardware Disk (Unplanned)

  ```no-highlight
        Flag                       Major reason
    ------------------       ---------------------------
    1    -   Unplanned       1       -   Other
    2    -   Custom          2       -   Hardware
    4    -   Planned         3       -   Operating System
                             4       -   Software
                             5       -   Application
                             64-255  -   Custom

            Minor reason
    -----------------------------------
    1            -   Other
    2            -   Maintenance
    3            -   Installation
    4            -   Upgrade
    5            -   Reconfiguration
    6            -   Hung
    7            -   Unstable
    8            -   Disk
    9            -   Network Card
    10           -   Environment
    11           -   Hardware Driver
    12           -   Other Driver
    13           -   Service Pack
    14           -   Software Update
    15           -   Security Fix
    16           -   Security
    17           -   Network Connectivity
    18           -   WMI
    19           -   Service Pack Uninstall
    20           -   Software Update Uninstall
    22           -   Security Fix Uninstall
    23           -   MMC
    256-65535    -   Custom

  ```

* **comment**

  **Optional comment for the reason above**

  The **comment** is optional argument used as a
  comment for the reason above (127 characters maximum)

