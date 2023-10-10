## Running Python scripts with SvcBatch

Python scripts can be run as Windows services using SvcBatch.


You can either use the simple batch file that will
launch your Python interpreter and actual Python script,
or use Python interpreter directly.


### Prerequisites

Install or download your favorite Python distribution
and `svcbatch.exe` binary release (or build it from the source code)
The example depends on Python3 functionality, but the code can be modified.


### Python script

Inside [python](python/) directory you can find a
simple python [script](python/pyservice.py)
that runs as infinite loop and prints current time
each five seconds.


### Wrapper batch file

Inside [python](python/) directory you can find a
simple python [batch](python/pyservice.bat) file
that calls the python interpreter.



### Installation

Service installation and management tasks are done
by using SvcBatch or Windows SC utility.


```no-highlight

> svcbatch create pyservice

... or

> sc create pyservice binPath= ""%CD%\svcbatch.exe" pyservice.bat"

```

To create a service that will use python.exe directly
without cmd.exe's wrapper pyservice.bat file, use the following:

```no-highlight

> svcbatch create pyservice /C:python /E:PATH=@ProgramFiles@\Python311;@PATH@ :pyservice.py


```


### Service management

After installation you can use SvcBatch to start or stop the service.
To manually start the service using SvcBatch, type:


```no-highlight

> svcbatch start pyservice

```

To stop the service, type

```no-highlight

> svcbatch stop pyservice

```

To delete the service, type

```no-highlight

> svcbatch delete pyservice

```

