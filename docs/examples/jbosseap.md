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

Inside the [JBoss EAP](jbosseap/) directory there are two batch files that
provide the complete solution to run and manage JBoss EAP as
windows service.


Put [winservice](jbosseap/winservice.bat) and [servicemgr](jbosseap/servicemgr.bat)
batch files into your `jboes-eap-x.x/bin` directory.
[servicemgr](jbosseap/servicemgr.bat) is a batch file
used to manage the services.

Before executing `servicemgr.bat` edit `servicemgr.bat` and modify
`JBOSSEAP_DISPLAY` variable to match the exact version you are using. You can put any string
for `DisplayName=` and `sc description ...` directly as fits.

```cmd

> servicemgr.bat create JBossEap7

... Optionaly you can use

> servicemgr.bat create JBossEap7 domain

```

After creating a service, edit the `winservice.bat` file and modify
JAVA_HOME to your actual jdk location. You can set JAVA_HOME to
System Environment, but then you must remove the `/s` switch inside
servicemgr.bat `sc create ...` command, because with `/s` switch,
SvcBatch will remove any *unsafe* environment variable.

That's it! Now, just type ...
```cmd

> sc start JBossEap7
  or ...
> net start JBossEap7

```
