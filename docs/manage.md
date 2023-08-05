# Managing SvcBatch Services

Starting with version **2.2** SvcBatch has a Service management
code that contains a subset of Microsoft's `sc.exe` utility to
create, configure, manage, and delete services.

# Table of Contents

- [Table of Contents](#table-of-contents)
- [Commands](#commands)
  - [Create](#cmd-create)
  - [Config](#cmd-config)
  - [Control](#cmd-control)
  - [Delete](#cmd-delete)
  - [Start](#cmd-start)
  - [Stop](#cmd-stop)
- [Command Options](#command-options)
  - [Common options](#common-options)
  - [Create and configure options](#create-options)
  - [Start options](#start-options)
  - [Stop options](#stop-options)

# Commands

Every command must be followed by **Service Name**

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

Check [Create and configure options](#create-options) for
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
            LINE : 4890
           ERROR : 1061 (0x425)
                   The service cannot accept control messages at this time
                   234
  ```

In case the service was not installed with `/b` command option,
the `233` custom control code will be disabled. Trying
to send this control code to the service will result in:

  ```no-highlight
  > svcbatch control myService 233
    Service Name : myService
         Command : Control
                 : FAILED
            LINE : 4890
           ERROR : 1052 (0x41c)
                   The requested control is not valid for this service
                   233
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
  > svcbatch start myService /wait
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
  > svcbatch stop myService /wait
    Service Name : myService
         Command : Stop
                 : SUCCESS
                   3047 ms
        EXITCODE : 0 (0x0)
  ```

will send a STOP request to the **myService** service
and return when the service is stopped.

In case the service did not stop within `/wait[:seconds]`
interval, SvcBatch will report something similar to:

  ```no-highlight
  > svcbatch stop myService /wait=2
    Service Name : myService
         Command : Stop
                 : FAILED
                   2047 ms
            LINE : 4853
           ERROR : 1053 (0x41d)
                   The service did not respond to the start or control request in a timely fashion
  ```




# Command options

SvcBatch management command line options are case insensitive
and both `-` and `/` can be used as switches. This means that
`/bin /Bin -bin and -BIN` can be used for the same option.

Command options arguments can be part or the command option
separated by either `:` or `=` character. This means that
`/start:auto`, `/start=auto` or `/start auto` are the same.

However arguments for options `/binpath`, `/description` and
`/displayname` must be declared as separate command line argument.

Some command options have alternate names. For example
either `/bin` or `/binpath` can be used to set the
service's BinaryPathName.


## Common options

* **/quiet**

  **Disable printing of status messages**

  By default SvcBatch will print the status message
  to the current console. Use this option to disable
  printing both status or error messages.


## Create and configure options

* **/binPath|/bin [path]**

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
  > svcbatch create myService /bin "@ProgramFiles@\SvcBatch\svcbatch.exe" ...
  >
  ```

  **Notice**

  In case the **path** contains any `@` character(s) they will be
  replaced by `%` character. In case there is a `%` character
  in the resulting **path**, SvcBatch will expand environment
  variable strings and replace them with the values defined
  for the current user.

  In case the resulting **path** contains space characters, SvcBatch
  will properly quote the **path**, so make sure that original **path**
  parameter is not already quoted.


* **/description|/desc [description]**

  **Sets the description string for a service**

  This option sets the description string for a service.

  ```no-highlight
  > svcbatch config myService /desc "This is My Service"
  >
  ```

* **/depend [dependencies]**

  **Sets the service dependencies**

  This option sets service dependencies.
  The **dependencies** string is list of services
  separated by `/` (forward slash) character.

  ```no-highlight
  > svcbatch config myService /depend=Tcpip/Afd
  >
  ```

  The myService will depend on `Tcpip` and `Afd` services.


* **/displayname|/display [name]**

  **Sets the service display name**

  This option sets the DisplayName for a service.

  ```no-highlight
  > svcbatch create myService /displayName "My Service"
  >
  ```

* **/username|/user [name]**

  **Sets the service account name**

  This option sets the **name** of the account under which
  the service should run.

  By default when the service is created it will
  be run under **LocalSystem** account.

  The **name** parameter can be either the full
  account name or:

  * **/user=0**
    This option is the same as **/userName=.\LocalSystem**

  * **/user=1**
    This option is the same as **/user "NT AUTHORITY\LocalService"**

  * **/user=2**
    This option is the same as **/obj "NT AUTHORITY\NetworkService"**


* **/password [password]**

  **Sets the password for the service account name**

  This option sets the **password** to the account name
  specified by the **/user [name]** parameter.
  Do not specify this option if the account has no password
  or if the service runs in the LocalService, NetworkService,
  or LocalSystem account.


* **/privileges|/privs [privileges]**

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
  > svcbatch config myService /privs SeBackupPrivilege/SeRestorePrivilege
  >
  ```

* **/start [type]**

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
  > svcbatch config myService /start:auto
  >
  ```


When the create or control command encounters the unknown option,
SvcBatch will stop option processing and save this and
any additional argument inside Windows Registry. Those
arguments will be used as command line arguments for the
SvcBatch service.

For example:

  ```no-highlight
  > svcbatch create myService
  ...
  > svcbatch config myService /start=auto /vl -rS ...
  ```

Since the `/vl` is not valid control or create option,
it will used as argument to svcbatch.exe when the
service is started, as well as any following argument.



## Start options

* **/wait[:seconds]**

  **Wait for service to start**

  If the service is not running, SvcBatch will wait
  up to **seconds** for service to start.

  The **seconds** is optional parameter. If not
  provided as part of **/wait** option, default
  value of `30` seconds will be used.

* **arguments ...**

  **Optional arguments for service start**

  If defined **arguments** will be passed to
  service on startup.

  ```no-highlight
  > svcbatch start myService /wait arg1 arg2 ...
  >
  ```


## Stop options

* **/wait[:seconds]**

  **Wait for service to stop**

  If the service is running, SvcBatch will wait
  up to **seconds** for service to stop.

  The **seconds** is optional parameter. If not
  provided as part of **/wait** option, default
  value of `30` seconds will be used.

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

