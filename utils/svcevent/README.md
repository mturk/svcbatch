## Registers or removes event source in registry

This is message dll used by SvcBatch
used for reporting events to the Windows
Application event log.

Use **regsvr32.exe** to register
this message dll.

To register this message dll for the
service use the following:

```no-highlight
> regsvr32.exe /s /n "/i:Simple Service" svcevent.dll

```



After uninstalling the service, use the following
to clean up the Registry settings

```no-highlight
> regsvr32.exe /s /u "/i:Simple Service" svcevent.dll

```

* **Notice**

  Make sure to use **--noevent** command option
  when calling create or delete service commands

