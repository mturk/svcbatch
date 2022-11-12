## Running JBoss EAP

This example shows how to use SvcBatch to run JBoss EAP
as a Windows service.

### Prerequisites

Download the latest [SvcBatch release](https://github.com/mturk/svcbatch/releases)
and put `svcbatch.exe` into your `jboes-eap-x.x/bin` directory.

The SvcBatch executable can be shared between multiple JBoss instances.
Put `svcbatch.exe` into the desired directory and modify
your service create scripts to set the output directory  using `-o`
command line option for each different instance.


### Example service

Inside the [jbosseap](jbosseap/) directory there are two batch files that
provide the complete solution to run and manage JBoss EAP as
windows service.


Put [servicemgr](jbosseap/servicemgr.bat)
batch file into your `jboes-eap-x.x/bin` directory.
[servicemgr](jbosseap/servicemgr.bat) is a batch file
used to manage the services.

Before executing [servicemgr](jbosseap/servicemgr.bat) edit `servicemgr.bat` and modify
`SERVICE_NAME`, `SERVICE_DISPLAY`, `SERVICE_DESCIPTION` and `JBOSSEAP_SERVER_MODE` variablea
to match the exact version you are using.

```cmd

> servicemgr.bat create

[SC] CreateService SUCCESS
[SC] ChangeServiceConfig SUCCESS
[SC] ChangeServiceConfig SUCCESS
[SC] ChangeServiceConfig2 SUCCESS
[SC] ChangeServiceConfig2 SUCCESS

```

After creating a service, edit the [winservice](jbosseap/winservice.bat)
file and modify JAVA_HOME to your actual jdk location. You can set JAVA_HOME to
System Environment.

That's it! Now, just type ...
```cmd

> servicemgr.bat create
  or ...
> sc start JBossEap7
  or ...
> net start JBossEap7

```
