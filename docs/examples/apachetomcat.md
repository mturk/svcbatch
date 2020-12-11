## Running Apache Tomcat

This example shows how to use SvcBatch to run Apache Tomcat
as a Windows service.

Ensure that you have at least jdk version 8 installed, so that
jvm does not exit on user log off.


### Prerequisites

Download latest [SvcBatch release](https://github.com/mturk/svcbatch/releases)
and put `svcbatch.exe` into your `tomcat/bin` directory.

SvcBatch executable can be shared between multiple Tomcat instances.
Simply put `svcbatch.exe` into desired directory and modify
your service create scripts to set working directory  using `/W`
commandline option for each different instance.


### Example service

Inside [Tomcat](tomcat/) directory there are two batch files that
provide complete solution to run and manage Apache Tomcat as
windows service.


Put [winservice](tomcat/winservice.bat) and [servicemgr](tomcat/servicemgr.bat)
batch files into your `tomcat/bin` directory.
[servicemgr](omcat/servicemgr.bat) is a simple batch file
that can be used instead typing multiple commands.

```cmd

> servicemgr.bat create Tomcat10

```

Before executing that command, edit `servicemgr.bat` and modify
`TOMCAT_DISPLAY` and `TOMCAT_FULLVER` variables to match the Tomcat
version you are using. You can actually just put any string
for `DisplayName=` and `sc description ...` directly as fits.

After creating a service, edit `winservice.bat` file and modify
JAVA_HOME to yours actual jdk location. You can replace that line
with JRE_HOME. You can set JAVA_HOME or JRE_HOME inside
System Environment, but then you must remove the `/s` switch inside
servicemgr.bat `sc create ...` command, because with `/s` switch, SvcBatch
will remove any *unsafe* environment variable.

That's it! Now, just type ...
```cmd

> sc start Tomcat10
  or ...
> net start Tomcat10

```

### Step by step

Instead above example, you can create your own
service batch file by following next few steps.

#### Step 1:
Create a batch file named `tomcatsvc.bat` inside `tomcat/bin`
with the following content

```cmd
@echo off
setlocal
rem Set any environment variables here missing from LOCAL_SERVICE account
rem Eg. set "JAVA_HOME=C:\Your\JDK\or\JRE\location"
rem
catalina.bat run

```

#### Step 2:
Create service by opening command prompt with Administrator
privileges inside your `tomcat/bin` directory

```cmd

> sc.exe create Tomcat DisplayName= "Apache Tomcat" binPath= ""%CD%\svcbatch.exe" /b tomcatsvc.bat"
  Optionally you can add ...
> sc.exe description Tomcat "Apache Tomcat Service"
  And/Or ...
> sc.exe config Tomcat start= auto

  Ensure system networking is up
> sc.exe config Tomcat depend= LanmanServer
  ... or at least TCP/IP and Winsock services
> sc.exe config Tomcat depend= Tcpip/Afd

```

To start the service at boot time either add the `start= auto` to `sc.exe create ... `,
or type `sc.exe config Tomcat start= auto` after calling create.

SC utility has somehow unusual approach to the command line options
which require a space after equal character. This means that you have
to use `start= auto` since `start=auto` will fail. Yes those
blanks are not typos :D

Check [SC documentation](https://docs.microsoft.com/en-us/windows-server/administration/windows-commands/sc-create)
for detailed description how to use the SC utility to create a service

#### Step 3:
Start the service by entering

```cmd
> sc.exe start Tomcat

```

#### Step 4:
Get full java thread dump

```cmd
> sc.exe control Tomcat 233

```
SvcBatch sends `CONSOLE_CTRL_BREAK` signal which is captured
by `java.exe` in the same way as clicking CTRL+Break keys in interactive console.
The output is written to SvcBatch.log file.

This feature is enabled only if `/b` command line switch was
defined at service's install.

#### Step 5:
Rotate log files
This will move Logs/SvcBatch.log to Logs/SvcBatch.log.1
and create a new Logs/SvcBatch.log file
Read the Log Rotation section for more details.

```cmd
> sc.exe control Tomcat 234

```

#### Step 6:
Stop the service by entering

```cmd
> sc.exe stop Tomcat

```

#### Step 7:
Delete the service by entering

```cmd
> sc.exe delete Tomcat

```

**!!!** Ensure the service was stopped before deletion
