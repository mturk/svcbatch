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

> svcbatch create Tomcat --displayName "Apache Tomcat" /F:L -h .. bin\catalina.bat run"
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

> svcbatch start Tomcat
  Or ..
> sc start Tomcat

```


#### Step 3:

Stop the service by entering

```no-highlight

> svcbatch stop Tomcat

```


#### Step 4:

Delete the service by entering

```no-highlight

> svcbatch delete Tomcat

```

* **Notice**

  This command will fail if the service is
  running. In that case stop the service, and
  call this command again.

