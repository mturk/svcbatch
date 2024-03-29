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
Microsoft C/C++ Compiler from Microsoft Visual Studio 2015
or any later version. Alternatively you use
[MSYS2](https://www.msys2.org) mingw64 compiler toolchain.

The official distributions are build using
[Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/)


### Build using Visual Studio

To build the SvcBatch using an already installed Visual Studio,
you will need to open the Visual Studio native x64 command
line tool.

If using Visual Studio 2017 or later, open command prompt
and call `vcvars64.bat` from Visual Studio install location
eg, `C:\Program Files\Visual Studio 2017\VC\Auxiliary\Build`


After setting the compiler, use the following

```cmd
> cd C:\Some\Location\svcbatch
> nmake

```

The binary should be inside **build\rel** subdirectory.

Using Visual Studio, svcbatch.exe can be built
as statically linked to the MSVCRT library.

Add `_STATIC_MSVCRT=1` as nmake parameter:

```cmd
> nmake _STATIC_MSVCRT=1

```

Use `_STATIC_MSVCRT=Hybrid` as nmake parameter
to create so called 'Hybrid CRT' build.

The 'Hybrid CRT' is a technique using the Universal
CRT AND the static CRT to get functional coverage without
the overhead of the static CRT or the external dependency
of the dynamic CRT.

```cmd
> nmake _STATIC_MSVCRT=Hybrid

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
inside **build\rel** subdirectory.

### Debug builds

Adding **_DEBUG=1** argument to nmake will produce debug
builds of svcbatch.exe

```cmd
> nmake "_DEBUG=1"
```

In case there are no compile errors the svcbatch.exe is located
inside **build\dbg** subdirectory.


### Makefile targets

Makefile has additional target which can be useful
for SvcBatch development and maintenance

```cmd
> nmake clean
```

This will remove all produced binaries and object files
by deleting **build\rel** subdirectory.

```cmd
> nmake tests
```

This will compile various test programs
and put them inside **build\rel** subdirectory.

### Vendor version support

At compile time you can define vendor suffix and/or version
by using the following:

```cmd
> nmake "VERSION_SFX=_1.acme"
```

This will create build with version strings set to `x.y.z_1.acme` where
`x.y.z` are SvcBatch version numbers.


## Creating Release

Ensure that each release tag starts with letter **v**,
eg **v0.9.1-dev**, **v1.0.37.alpha.1** or similar.
Check [Semantic Versioning 2.0](https://semver.org/spec/v2.0.0.html)
for more guidelines.

To create a .zip distribution archive download
and extract the 7-zip standalone console version from
[7-Zip Extra](https://www.7-zip.org/a/7z2301-extra.7z)
and put **7za.exe** somewhere in the PATH.

Change `SVCBATCH_ISDEV_VERSION` to `0` in [svcbatch.h](../svcbatch.h)
file and remove `* In development` for the current version
from [CHANGES](../CHANGES.md) file.

Commit and push the changes with a message similar to
`Prepare for x.y.z release`.

Use something like unix `date +%y%j` or `date +%y%V%u` command to get
the release date number and use it to modify the  `SVCBATCH_MICRO_VERSION`
in [svcbatch.h](../svcbatch.h) and `version` parameter in
[svcbatch.manifest](../svcbatch.manifest) files.

Run the [mkrelease.bat](../mkrelease.bat) script file
to compile and create required metadata and release assets.

Edit **svcbatch-x.y.z.txt** and put it's content
in GitHub release description box.

Finally add the **svcbatch-x.y.z-win-x64.zip**
file to the release assets.

After publishing the release, increment the `SVCBATCH_PATCH_VERSION`
and set `SVCBATCH_ISDEV_VERSION` back to `1` in [svcbatch.h](../svcbatch.h).

Update `version="x.x.x.x"` to the new one in [svcbatch.manifest](../svcbatch.manifest)
file, and add a new section above the current one in [CHANGES](../CHANGES.md) file.

The official binary release contains standard and debug builds.

It is done by invoking:

```cmd
> mkrelease.bat /h x.y.x
...
> mkrelease.bat /d x.y.x
...

```
