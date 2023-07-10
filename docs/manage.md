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
and `/` is used as command switch.

## Common options

* **/verbose[:level]**

  **Print status messages**

* **/quiet**

  **Disable printing of status messages**

## Configure options

* **/binPath [path]**

## Start options

* **/wait[:seconds]**

  **Wait for service to start**

## Stop options

* **/wait[:seconds]**

  **Wait for service to stop**


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

    <comment> = Optional comment for the reason above (127 characters maximum)

  ```
