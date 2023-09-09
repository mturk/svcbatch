## Running Apache Tomcat

This example shows how to use SvcBatch to run Apache Tomcat
as a Windows service.


### Prerequisites

Ensure that you have at least jdk version 8 installed,
so that jvm does not exit on user log off.

Download the latest [SvcBatch release](https://github.com/mturk/svcbatch/releases)
and put `svcbatch.exe` into your `tomcat/bin` directory.

The SvcBatch executable can be shared between multiple Tomcat instances.
Put `svcbatch.exe` into the desired directory and modify
your service create scripts to set the work directory using `-w`
command line option that is unique for each different instance.


### Example service

Inside the [Tomcat](tomcat/) directory there are two batch files that
provide the complete solution to run and manage Apache Tomcat as
windows service.


Put [winservice](tomcat/winservice.bat) and eventually
[setenv](tomcat/setenv.bat) batch files
together with `svcbatch.exe` into your `tomcat/bin` directory.

[winservice](tomcat/winservice.bat) is a batch file
that can be used to manage service instead typing multiple commands.

```no-highlight

> winservice.bat create

```

That's it! Now, just type ...

```no-highlight

> winservice.bat start

```

The [setenv](tomcat/setenv.bat) batch file (or the existing one)
can be used to modify any environment variables needed.
For example, you can set the JAVA_HOME or JRE_HOME or any other
environment variable to the location different then the one defined
for the account under which the service is running.


### Step by step

Instead above example, you can create your own
service by following the next few steps.

#### Step 1:

Create a service by opening command prompt with Administrator
privileges inside your `tomcat/bin` directory

```no-highlight

> svcbatch create Tomcat --displayName "Apache Tomcat" -f:CR -h .. bin\catalina.bat run"
  Optionally you can add description ...
> svcbatch config Tomcat --description "Apache Tomcat Service"
  And ...
> svcbatch config Tomcat --start=auto

```

Check the [Managing Services documentation](../manage.md)
for detailed description how to use the SvcBatch to create and manage services

#### Step 2:

To manually start the service use:

```no-highlight

> svcbatch start Tomcat [--no-wait]
  Or ..
> sc start Tomcat

```


#### Step 3:

Get the full java thread dump

```no-highlight

> svcbatch control Tomcat 233

```

SvcBatch sends `CONSOLE_CTRL_BREAK` signal which is captured
by `java.exe` in the same way as clicking CTRL+Break keys in interactive console.
The output is written to SvcBatch.log file.

This feature is enabled only if `-f:C` command line option was
defined at service's install.


#### Step 4:

Rotate log files

```no-highlight

> svcbatch control Tomcat 234

```

This feature is enabled only if the log rotation is enabled.
Add `-f:R` command option to enable manual log rotation.

Read the Log Rotation section for more details.


#### Step 5:

Stop the service by entering

```no-highlight

> svcbatch stop Tomcat [--no-wait]

```


#### Step 6:

Delete the service by entering

```no-highlight

> svcbatch delete Tomcat

```

* **Notice**

  This command will fail if the service is
  running. In that case stop the service, and
  call this command again.

