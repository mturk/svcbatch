## Running Apache Tomcat

This example shows how to use SvcBatch to run Apache Tomcat
as a Windows service.


### Prerequisites

Ensure that you have at least jdk version 8 installed, so that
jvm does not exit on user log off.

Download the latest [SvcBatch release](https://github.com/mturk/svcbatch/releases)
and put `svcbatch.exe` into your `tomcat/bin` directory.

The SvcBatch executable can be shared between multiple Tomcat instances.
Put `svcbatch.exe` into the desired directory and modify
your service create scripts to set the home directory using `/W`
command line option for each different instance.


### Example service

Inside the [Tomcat](tomcat/) directory there are two batch files that
provide the complete solution to run and manage Apache Tomcat as
windows service.


Put [servicemgr](tomcat/servicemgr.bat) and [winservice](tomcat/winservice.bat)
batch files together with `svcbatch.exe` into your `tomcat/bin` directory.

[servicemgr](tomcat/servicemgr.bat) is a simple batch file
that can be used to manage service instead typing multiple commands.

Before executing `servicemgr.bat`, edit `servicemgr.bat` and modify
default `SERVICE_NAME`, `displayName` and `description`
parameters to match the Tomcat version you are using.

```no-highlight

> servicemgr.bat create

```

After creating a service, edit [winservice](tomcat/winservice.bat) file and add
or modify any environment variables needed. You can set
JAVA_HOME to your actual jdk location or replace that line
with JRE_HOME. You can set JAVA_HOME or JRE_HOME directly inside
System Environment.

That's it! Now, just type ...

```no-highlight

> servicemgr.bat start tomcat10
  or ...
> sc start tomcat10
  or ...
> net start tomcat10
  or ...
> svcbatch start tomcat10 -security

```

### Step by step

Instead above example, you can create your own
service by following the next few steps.

#### Step 1:

Create a service by opening command prompt with Administrator
privileges inside your `tomcat/bin` directory

```no-highlight

> svcbatch create Tomcat /displayName "Apache Tomcat" /b /h.. bin\catalina.bat run"
  Optionally you can add ...
> svcbatch config Tomcat /description "Apache Tomcat Service"
  And ...
> svcbatch config Tomcat /start=auto

```

Check the [Managing Services documentation](../manage.md)
for detailed description how to use the SvcBatch to create and manage services

#### Step 2:

Start the service by entering

```no-highlight

> svcbatch start Tomcat /wait
  Or ..
> sc start Tomcat

```

#### Step 3:

Get the full java thread dump

```no-highlight

> sc control Tomcat 233

```
SvcBatch sends `CONSOLE_CTRL_BREAK` signal which is captured
by `java.exe` in the same way as clicking CTRL+Break keys in interactive console.
The output is written to SvcBatch.log file.

This feature is enabled only if `/b` command line switch was
defined at service's install.

#### Step 4:

Rotate log files
This will move Logs/SvcBatch.log to Logs/SvcBatch.log.1
and create a new Logs/SvcBatch.log file
Read the Log Rotation section for more details.

```no-highlight

> svcbatch control Tomcat 234

```

#### Step 5:

Stop the service by entering

```no-highlight

> svcbatch stop Tomcat

```

#### Step 6:

Delete the service by entering

```no-highlight

> svcbatch delete Tomcat

```

* **Notice**

   Ensure the service is stopped before deletion
