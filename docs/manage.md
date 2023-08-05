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
  - [Configure options](#configure-options)
  - [Start options](#start-options)
  - [Stop options](#stop-options)

# Commands

Every command must be followed by **Service Name**

## Create

Creates a service.

## Config

Changes the configuration of a service.

## Control

Sends a control to a service.

## Delete

Deletes a service.

## Start

Starts a service.

## Stop

Sends a STOP request to a service.


# Command options

SvcBatch management command line options are case insensitive
and both `-` and `/` can be used as switches. This means that
`/bin /Bin -bin and -BIN` can be used for the same option.

## Common options

* **/quiet**

  **Disable printing of status messages**

  By default SvcBatch will print the status message
  to the current console. Use this option to disable
  printing both status or error messages.

## Configure options

* **/binPath|/bin [path]**

  **Set service binary path name to .exe file**

  This option sets the service's binary path name
  to the .exe file pointed by **path** argument.

  By default SvcBatch will set the service's BinaryPathName
  to the current path of the svcbatch.exe on service create.


* **/description|/desc [description]**

  **Sets the description string for a service**

  This option sets the description string for a service.

  ```cmd
  > svcbatch config myService /desc "This is My Service"
  >
  ```

* **/depend [dependencies]**

  **Sets the service dependencies**

  This option sets service dependencies.
  The **dependencies** string is list of services
  separated by `/` (forward slash) character.

  ```cmd
  > svcbatch config myService /depend=Tcpip/Afd
  >
  ```

  The myService will depend on `Tcpip` and `Afd` services.


* **/displayname|/display [name]**

  **Sets the service display name**

  This option sets the DisplayName for a service.

  ```cmd
  > svcbatch create myService /displayName "My Service"
  >
  ```

* **/obj|/username|/user [name]**

  **Sets the service account name**

  This option sets the **name** of the account under which
  the service should run.

  By default when the service is created it will
  be run under **LocalSystem** account.

  The **name** parameter can be either the full
  account name or:

  **/user=0**

    This option is the same as **/userName=.\LocalSystem**

  **/user=1**

    This option is the same as **/user "NT AUTHORITY\LocalService"**

  **/user=2**

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

  ```cmd
  > svcbatch config myService /privs SeBackupPrivilege/SeRestorePrivilege
  >
  ```


## Start options

* **/wait[:seconds]**

  **Wait for service to start**

  If the service is not running, SvcBatch will wait
  up to **seconds** for service to start.

  The **seconds** is optional parameter. If not
  provided as part of **/wait** option, default
  value of `30` seconds will be used.

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

