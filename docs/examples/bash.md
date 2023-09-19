## Running Bash scripts with SvcBatch

Bash scripts can be run as Windows services using SvcBatch.

Inside the [bash](bash) directory you can find
a basic example that runs `.sh` script as services.

Put `svcbatch.exe` into that directory and create
the service by typing

```no-highlight

> svcbatch create adummybash -f:FB -c:bash.exe [ --norc --noprofile ] -e:PATH=@SystemDrive@\msys64\usr\bin;@PATH@ ./dummyloop.sh

```

After that, start the service by typing

```no-highlight

> svcbatch start adummybash

```

To stop the service type

```no-highlight

> svcbatch stop adummybash

```
