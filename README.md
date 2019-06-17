```
 ____  ___  ___  ___ ____  ___   ____
|  _ \/   \|   \/   \  _ \/ _ \ (__  \
|    (  -  | |  ) -  |   (   _/ /  __/
|__\__|_|__|___/__|__|_\__|___| |____|

             https://www.radare.org

                             --pancake
```

| | |
|----------|---------------------------------------------------------------------|
| **Jenkins**  	| [![Build Status](http://ci.rada.re/buildStatus/icon?job=radare2)](http://ci.rada.re/job/radare2)|
| **TravisCI** 	| [![Build Status](https://travis-ci.org/radare/radare2.svg?branch=master)](https://travis-ci.org/radare/radare2)|
| **AppVeyor**  | [![Build Status](https://ci.appveyor.com/api/projects/status/v9bxvsb1p6c3cmf9/branch/master?svg=true)](https://ci.appveyor.com/project/radare/radare2-shvdd)|
| **Coverity** 	| [![Build Status](https://scan.coverity.com/projects/416/badge.svg)](https://scan.coverity.com/projects/416) |
| **LGTM** | [![Total alerts](https://img.shields.io/lgtm/alerts/g/radare/radare2.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/radare/radare2/alerts/)
| **Infrastructure** |  [![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/741/badge)](https://bestpractices.coreinfrastructure.org/projects/741) |
| **Codecov** | [![codecov](https://codecov.io/gh/radare/radare2/branch/master/graph/badge.svg)](https://codecov.io/gh/radare/radare2)
<a href="https://repology.org/metapackage/radare2">
<img src="https://repology.org/badge/vertical-allrepos/radare2.svg" alt="Packaging status" align="right" width="150px">
</a>

# Introduction

r2 is a rewrite from scratch of radare in order to provide
a set of libraries and tools to work with binary files.

Radare project started as a forensics tool, a scriptable
command-line hexadecimal editor able to open disk files,
but later added support for analyzing binaries, disassembling
code, debugging programs, attaching to remote gdb servers...

radare2 is portable.

To learn more on radare2 you may want to read the [official radare2 book](https://radare.gitbooks.io/radare2book/content/).

## Operating Systems

Windows (since XP), GNU/Linux, OS X, [Net|Free|Open]BSD,
Android, iOS, OSX, QNX, Solaris, Haiku, FirefoxOS.

## Architectures

i386, x86-64, ARM, MIPS, PowerPC, SPARC, RISC-V, SH, m68k, m680x, AVR,
XAP, System Z, XCore, CR16, HPPA, ARC, Blackfin, Z80, H8/300, V810,
V850, CRIS, XAP, PIC, LM32, 8051, 6502, i4004, i8080, Propeller,
Tricore, CHIP-8, LH5801, T8200, GameBoy, SNES, SPC700, MSP430, Xtensa,
NIOS II, Java, Dalvik, WebAssembly, MSIL, EBC, TMS320 (c54x, c55x,
c55+, c66), Hexagon, Brainfuck, Malbolge, whitespace, DCPU16, LANAI,
MCORE, mcs96, RSP, SuperH-4, VAX.

## File Formats

ELF, Mach-O, Fatmach-O, PE, PE+, MZ, COFF, OMF, TE, XBE, BIOS/UEFI,
Dyldcache, DEX, ART, CGC, Java class, Android boot image, Plan9 executable,
ZIMG, MBN/SBL bootloader, ELF coredump, MDMP (Windows minidump),
WASM (WebAssembly binary), Commodore VICE emulator, QNX,
Game Boy (Advance), Nintendo DS ROMs and Nintendo 3DS FIRMs, various filesystems.

## Scripting

Native bindings are supported but the recommended way to interact with r2
from other languages is by using [r2pipe](https://github.com/radare/radare2-r2pipe)

D, Clojure Vala/Genie, Python, Erlang, Shell, Haskell, Prolog, Swift, Lisp, Nim,
Javascript, Lua, Rust, Go, Perl, Guile, PHP, Newlisp, c#, Java, Ruby, Java, OCaml...

# Install / Update

The recommended way to install or update radare2 from git for single-user systems:

	$ sys/install.sh

If you don't have root, or just want to install it in your home use:

	$ sys/user.sh

Note that those scripts will install using configure+make using symlinks, so you
don't need to reinstall everytime you change something in the builddir.

* If you don't like symlinks use `sys/install.sh --install`
* To use capstone5 use the `--with-capstone5` flag.

Alternatively you can also build with meson + ninja:

	$ ./sys/meson.py --prefix=/usr --shared --install

## Uninstall

In case of a polluted filesystem, you can uninstall the current
version or remove all previous installations:

	$ make uninstall
	$ make purge
	
To remove all stuff including libraries, use

	$ make system-purge

## Package Manager

Radare2 has its own package manager - r2pm. Its packages
repository is on [GitHub too](https://github.com/radare/radare2-pm).
To start to using it for the first time, you need to initialize packages:

	$ r2pm init

Refresh the packages database before installing any package:

	$ r2pm update

To install a package, use the following command:

	$ r2pm install [package name]

# Development

## Coding Style

Look at [CONTRIBUTING.md](https://github.com/radare/radare2/blob/master/CONTRIBUTING.md).

## Tests

Running `make tests` will fetch the [radare2-regressions](https://github.com/radare/radare2-regressions)
repository and run all the tests in order to verify that no changes break any functionality.

We run those tests on every commit, and they are also executed with ASAN
and valgrind on different platforms to catch other unwanted 'features'.


# Community

Website: [https://www.radare.org/](https://www.radare.org/)

Telegram: [https://t.me/radare](https://t.me/radare)

Twitter: [@radareorg](https://twitter.com/radareorg)

IRC: irc.freenode.net #radare

Matrix: @radare2:matrix.org
