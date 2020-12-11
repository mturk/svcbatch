## Running Python scripts with SvcBatch

Python scripts can be easily run as Windows
services using SvcBatch. All you need is to create a simple
batch file that will launch your Python interpreted and
actual Python script.


### Prerequisites

Install or download your favorite Python distribution
and `svcbatch.exe` binary release (or build it from the source code)
The example depends on Python3 functionallity, but if
familiar, you can easily modify the code.

### Application

Create a file named `pyservice.py` with the following content

```python
#!/usr/bin/env python

"""
One dummy infinite loop pretending to be a service
"""

import time

def main():
    print("Pyservice started", flush=True)

    while True:
        time.sleep(5)
        now = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        print("Running : ", now, flush=True)

if __name__ == '__main__':
    main()

```

Save the file in the same directory where
the `svcbatch.exe` is located.


### Create service batch file

Create batch file named `pyservice.bat`
with the following content

```batchfile
@echo off
rem
set "PATH=C:\Program Files\Python38;%PATH%"
rem
python pyservice.py

```

Save that file in the same location where the
`svcbatch.exe` and `pyservice.py` are located.

Modify the batch file's `set "PATH= ...` if your Python
install location is different then `C:\Program Files\Python38`

### Installation

Service installation and management tasks are done
by using **SC** Windows utility.


```cmd
> sc create pyservice binPath= ""%CD%\svcbatch.exe" /c /s pyservice.bat"

```

After installation you can use SCM to start or stop the service.

```cmd
> sc start pyservice

```

To stop the service type

```cmd
> sc stop pyservice

```

### Example files

The above examples can be found inside
[python](python/) directory.

