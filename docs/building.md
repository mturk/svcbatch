## Building SvcBatch

This project contains source code for SvcBatch, a program
that runs batch files as Windows services.

The build process supports only command line tools
for both Microsoft and GCC compilers.

SvcBatch release comes with the **svcbatch.exe** binary.
In case you wish to create your own binary build,
download or clone SvcBatch sources and follow a
few standard rules.

### Prerequisites

To compile SvcBatch from source code you will need either
Microsoft C/C++ Compiler from Microsoft Visual Studio 2010
or any later version. Alternatively you use
[MSYS2](https://www.msys2.org) mingw64 compiler toolchain.

The official distributions are build using
[Custom Microsoft Compiler Toolkit](https://github.com/mturk/cmsc)
compiler bundle.


### Build using CMSC

Presuming that you have downloaded and unzipped
[CMSC release](https://github.com/mturk/cmsc/releases)
in the root of C drive.

Open command prompt in the directory where you have
downloaded or cloned SvcBatch and do the following

```cmd
> C:\cmsc-15.0_45\setenv.bat
Using default architecture: x64
Setting build environment for win-x64/0x0601

> nmake _STATIC_MSVCRT=1

Microsoft (R) Program Maintenance Utility Version 9.00.30729.207
...
```

In case there are no compile errors, svcbatch.exe is located
inside **x64** subdirectory.

### Build using Visual Studio

To build the SvcBatch using an already installed Visual Studio,
you will need to open the Visual Studio native x64 command
line tool. The rest is almost the same as with CMSC toolkit.

Here is the example for Visual Studio 2012

Inside the Start menu select Microsoft Visual Studio 2012 then
click on Visual Studio Tools and click on
Open `VC2012 x64 Native Tools Command Prompt`.

If using Visual Studio 2017 or later, open command prompt
and call `vcvars64.bat` from Visual Studio install location
eg, `C:\Program Files\Visual Studio 2017\VC\Auxiliary\Build`


After setting the compiler, use the following

```cmd
> cd C:\Some\Location\svcbatch
> nmake

```

The binary should be inside **x64** subdirectory.

Using Visual Studio, svcbatch.exe can be built
as statically linked to the MSVCRT library.

Add `_STATIC_MSVCRT=1` as nmake parameter:
```cmd
> nmake _STATIC_MSVCRT=1

```

### Build using mingw64

SvcBatch can be built using GCC compiler from msys2.
You will need to install [msys2](https://www.msys2.org)

After installing msys2 open msys2 shell and
install required packages: base-devel and mingw-w64-x86_64-toolchain
if they are not already installed.

For example
```sh
$ pacman --noconfirm -Sy base-devel
$ pacman --noconfirm -Sy mingw-w64-x86_64-toolchain
```

Restart the shell with `-mingw64` parameter or open `mingw64.exe`
terminal and cd to SvcBach source directory and type

```sh

$ make -f Makefile.gmk
```

In case there are no compile errors the svcbatch.exe is located
inside **x64** subdirectory.

### Makefile targets

Makefile has additional target which can be useful
for SvcBatch development and maintenance

```cmd
> nmake clean
```

This will remove all produced binaries and object files
by deleting **x64** subdirectory.

### DebugView support

For debug and development purposes you can compile
SvcBatch with the `_DBGVIEW` option that will enable
internal tracing which can be viewed by using
[SysInternals DebugView](https://download.sysinternals.com/files/DebugView.zip)
utility.

This option can be enabled at compile time by using
the following:

```cmd
> nmake _DBGVIEW=1
```

For more information about DebugView check the
[DebugView](https://docs.microsoft.com/en-us/sysinternals/downloads/debugview)
official site.

### Vendor version support

At compile time you can define vendor suffix and/or version
by using the following:

```cmd
> nmake _VENDOR_SFX="_1.acme"
```

This will create build with version strings set to `x.y.z_1.acme` where
`x.y.z` are SvcBatch version numbers.

## Creating Release

Ensure that each release tag starts with letter **v**,
eg **v0.9.1-dev**, **v1.0.37.alpha.1** or similar.
Check [Semantic Versioning 2.0](https://semver.org/spec/v2.0.0.html)
for more guidelines.

Before publishing new SvcBatch version run the anti virus scan on produced binaries.
Download latest version of [ClamAV](https://www.clamav.net/downloads)
and run the installer or unzip the portable version to some directory of
choice and do the required setup. Check
[Installing](https://www.clamav.net/documents/installing-clamav-on-windows)
section for more info about ClamAV installation and setup.

To create a .zip distribution archive download
and extract the 7-zip standalone console version from
[7-Zip Extra](https://www.7-zip.org/a/7z2107-extra.7z)
and put **7za.exe** somewhere in the PATH.

After compilation, run the [release](../mkrelease.bat) batch file
to create required metadata and release assets.

Edit **svcbatch-x.y.z.txt** file and remove the directory
part of svcbatch,exe. Also remove the scan time data
(*those are usually last three lines*)
The content of this file has to be put in GitHub release description box.

Finally add the **svcbatch-x.y.z-win-x64.zip** and **svcbatch-x.y.z-sha256.txt**
files to the release assets.
