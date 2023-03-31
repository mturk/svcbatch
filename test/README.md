## SvcBatch test programs

This folder contains various SvcBatch tests
used for development.

Further documentation we will presume that
you have a current SvcBatch git branch inside
**C:\Workplace\projects\svcbatch** directory
and installed Visual Studio compiler.

## Prerequisites

Check [Building](../docs/building.md) for basic
build steps and requirements.

## Create debug build

From Start menu open your Visual Studio's
`x64 Native Tools Command Prompt ...`
and then type:

```cmd
> cd C:\Workplace\projects\svcbatch
> nmake _DEBUG=1 tests

Microsoft (R) Program Maintenance Utility Version ...
Copyright (C) Microsoft Corporation.  All rights reserved.

...

```

The build binaries are located inside **.build\dbg** directory
in case the build was successful.
