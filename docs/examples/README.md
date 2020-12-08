# SvcBatch Examples

This directory contains various examples explaining how
to run and deploy SvcBatch utility inside different
environments and target applications.

More content will be added as we go.

### dummyservice.bat

This is example batch that shows how to create,
run and delete SvcBatch service.

### Apache Tomcat

Open [apachetomcat.md](apachetomcat.md) for more details
how to create and run Apache Tomcat as Windows service.

### Python

Open [python.md](python.md) for detailed explanation
about how to easily convert Python applications to Windows services.

### Ruby

Inside [ruby](ruby) directory you can find simple
web server example and a batch file that is used as
service wrapper.

Put `svcbatch.exe` into that directory and create
the service by typing

```no-highlight

> rbhttpserver.bat create

... you should see something like below ...

[SC] CreateService SUCCESS
[SC] ChangeServiceConfig SUCCESS

```

After that, start the service by typing

```no-highlight

> sc start rbhttpserver

  SC should display something like this ...

SERVICE_NAME: rbhttpserver
        TYPE               : 10  WIN32_OWN_PROCESS
        STATE              : 2  START_PENDING
                                (NOT_STOPPABLE, NOT_PAUSABLE, IGNORES_SHUTDOWN)
        WIN32_EXIT_CODE    : 0  (0x0)
        SERVICE_EXIT_CODE  : 0  (0x0)
        CHECKPOINT         : 0x1
        WAIT_HINT          : 0x1388
        PID                : 2368
        FLAGS              :

```

Get response from our web server by typing ...

```no-highlight

# curl -v http://localhost:8088

... This is typical output

*   Trying ::1:8088...
* Connected to localhost (::1) port 8088 (#0)
> GET / HTTP/1.1
> Host: localhost:8088
> User-Agent: curl/7.73.0
> Accept: */*
>
* Mark bundle as not supporting multiuse
< HTTP/1.1 200 OK
< Server: WEBrick/1.6.0 (Ruby/2.7.2/2020-10-01)
< Date: Tue, 08 Dec 2020 15:06:58 GMT
< Content-Length: 67
< Connection: Keep-Alive
<
* Connection #0 to host localhost left intact
Service name=rbhttpserver uuid=01234567-89ab-cdef-0123-456789abcdef

```
