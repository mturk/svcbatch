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


Put [winservice](jbosseap/winservice.bat) and
eventually [eapservice](jbosseap/eapservice.bat)
batch files into your `jboss-eap-x.x/bin` directory.

The [eapservice](jbosseap/eapervice.bat) is an example
that can be used as a service wrapper calling the actual
batch file (eg. standalone.bat or domain.bat).


The [winservice](jbosseap/winservice.bat) is a batch file
that can be used to manage the services.

```no-highlight

Usage: winservice.bat command [service_name] [server_mode] [arguments ...]
commands:
  create            Create the service
  createps          Create the service using powershell
  delete            Delete the service
  start             Start the service
  stop              Stop the service

```

Before executing [winservice](jbosseap/winservice.bat) edit `winservice.bat`
and modify `DEFAULT_SERVICE_NAME` `SERVICE_DISPLAY`, `SERVICE_DESCIPTION`
and `DEFAULT_SERVER_MODE` variables to match the exact version you are using.


To create a service type:

```no-highlight

> winservice.bat create

Service Name : JBossEAP74
     Command : Create
             : SUCCESS
     STARTUP : Automatic (2)

```

That's it! Now, just type:

```no-highlight

> winservice.bat start

Service Name : JBossEAP74
     Command : Start
             : SUCCESS
               1046 ms
         PID : 4492

```

To manually stop the service type:


```no-highlight

> winservice.bat stop
Service Name : JBossEAP74
     Command : Stop
             : SUCCESS
               4032 ms
    EXITCODE : 0 (0x0)

```
