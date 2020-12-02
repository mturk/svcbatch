# Running Apache Tomcat

This is simple example how to use SvcBatch to run Apache Tomcat
as a Windows service.

Ensure that you have at least jdk version 8 installed, so that
java.exe does not exit on user log off.

#### Step 1:
Download latest `svcbatch.exe` [release](https://github.com/mturk/svcbatch/releases)
into your `tomcat/bin` directory

#### Step 2:
Create a batch file named `tomcatsvc.bat` inside `tomcat/bin`
with the following content

```no-highlight
@echo off
setlocal
rem Set any environment variables here missing from LOCAL_SERVICE account
rem Eg. set "JAVA_HOME=C:\Your\JDK\or\JRE\location"
rem
catalina.bat run

```

#### Step 3:
Install service by opening command prompt with Administrator
privileges inside your `tomcat/bin` directory

```no-highlight

> sc.exe create Tomcat DisplayName= "Apache Tomcat" binPath= ""%CD%\svcbatch.exe" tomcatsvc.bat"
  Optionally you can add ...
> sc.exe description Tomcat "Apache Tomcat Service"
  And/Or ...
> sc.exe config Tomcat start= auto
  Ensure system networking is up
> sc.exe config Tomcat depend= LanmanServer

```

To start the service at boot time either add the `start= auto` to `sc.exe create ... `,
or type `sc.exe config Tomcat start= auto` after calling create.

SC utility has somehow unusual approach to the command line options
which require a space after equal character. This means that you have
to use `start= auto` since `start=auto` will fail. Yes those
blanks are not typos :D

Check [SC documentation](https://docs.microsoft.com/en-us/windows-server/administration/windows-commands/sc-create)
for detailed description how to use the SC utility to create a service

#### Step 4:
Start the service by entering

```no-highlight
> sc.exe start Tomcat

```

#### Step 5:
Get full java thread dump

```no-highlight
> sc.exe control Tomcat 233

```
SvcBatch sends `CONSOLE_CTRL_BREAK` signal which is captured
by `java.exe` in the same way as clicking CTRL+Break keys in interactive console.
The output is written to SvcBatch.log file.


#### Step 6:
Rotate log files
This will move Logs/SvcBatch.log to Logs/SvcBatch.log.1
and create a new Logs/SvcBatch.log file
Read the Log Rotation section for more details.

```no-highlight
> sc.exe control Tomcat 234

```

#### Step 7:
Stop the service by entering

```no-highlight
> sc.exe stop Tomcat

```

#### Step 8:
Uninstall the service by entering

```no-highlight
> sc.exe delete Tomcat

```
