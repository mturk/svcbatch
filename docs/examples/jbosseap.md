## Running JBoss EAP

This example shows how to use SvcBatch to run JBoss EAP
as a Windows service.

### Prerequisites

Download latest [SvcBatch release](https://github.com/mturk/svcbatch/releases)
and put `svcbatch.exe` into your `jboes-eap-x.x/bin` directory.

SvcBatch executable can be shared between multiple JBoss instances.
Simply put `svcbatch.exe` into desired directory and modify
your service create scripts to set output directory  using `-o`
command line option for each different instance.


### Example service

Inside [JBoss EAP](jboeseap/) directory there are two batch files that
provide complete solution to run and manage JBoss EAP as
windows service.


Put [winservice](jboeseap/winservice.bat) and [servicemgr](jboeseap/servicemgr.bat)
batch files into your `jboes-eap-x.x/bin` directory.
[servicemgr](jboeseap/servicemgr.bat) is a simple batch file
that can be used instead typing multiple commands.

```cmd

> servicemgr.bat create JBossEap7

... Optionaly you can use

> servicemgr.bat create JBossEap7 domain

```

Before executing that command, edit `servicemgr.bat` and modify
`JBOSSEAP_DISPLAY` variable to match the exact version.
version you are using. You can actually just put any string
for `DisplayName=` and `sc description ...` directly as fits.

After creating a service, edit `winservice.bat` file and modify
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
