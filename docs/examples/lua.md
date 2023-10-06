## Running Lua scripts with SvcBatch

Lua scripts can be run as Windows services using SvcBatch.

Inside the [lua](lua) directory you can find
a basic example that runs `.lua` script as services.

Put `svcbatch.exe` and `lua.exe` into that directory and create
the service by typing

```no-highlight

> svcbatch create luasvc /C:lua.exe :iloop.lua

```

After that, start the service by typing

```no-highlight

> svcbatch start luasvc

```

To stop the service type

```no-highlight

> svcbatch stop luasvc

```
