## Running JBoss EAP

This example shows how to use SvcBatch to run JBoss EAP
as a Windows service.

### Prerequisites

Download the latest [SvcBatch release](https://github.com/mturk/svcbatch/releases)
and put `svcbatch.exe` into your `jboss-eap-x.x/bin` directory.

The SvcBatch executable can be shared between multiple JBoss instances.
Put `svcbatch.exe` into the desired directory and modify
your service create scripts to set the output directory  using `-o`
command line option for each different instance.


### Example service

Inside the [jbosseap](jbosseap/) directory there are two batch files that
provide the complete solution to run and manage JBoss EAP as
windows service.


Put [servicemgr](jbosseap/servicemgr.bat) and
eventually [winservice](jbosseap/winservice.bat)
batch files into your `jboss-eap-x.x/bin` directory.

The [servicemgr](jbosseap/servicemgr.bat) is a batch file
used to manage the services.

Before executing [servicemgr](jbosseap/servicemgr.bat) edit `servicemgr.bat` and modify
`SERVICE_NAME`, `SERVICE_DISPLAY`, `SERVICE_DESCIPTION` and `JBOSSEAP_SERVER_MODE` variables
to match the exact version you are using.

```no-highlight

> servicemgr.bat create

Service Name : jbosseap74
     Command : Create
             : SUCCESS
     STARTUP : Automatic (2)


```

After creating a service, edit the [winservice](jbosseap/winservice.bat)
file and modify JAVA_HOME to your actual jdk location. You can set JAVA_HOME to
System Environment.

That's it! Now, just type ...

```no-highlight

> servicemgr.bat start
  or ...
> sc start JBossEap74
  or ...
> net start JBossEap74

```
