Umler
=====

[![Build Status](https://travis-ci.org/bbannier/umler.svg)](https://travis-ci.org/bbannier/umler)

Low-cost extraction of class relationships from C++ classes.

This tool extract class relationships from C++ classes. Only a minimal subset
of information is extracted which requires parsing much less of the code than
e.g. needed by more powerful tools like [kythe](http://kythe.io).

Installation
------------

Required dependencies:

* a compiler able to build basic C++11, gcc-4.7 or clang-3.4 seem sufficient
* libsqlite with headers (probably in some dev package)

This tool needs to be checked out below the `extra` tree of clang in a full
checkout (`llvm/tools/clang/tools/extra`).  After that it can be built as part
of a normal LLVM/clang build, see `build.sh` for an example.

Usage
-----

Umler visits the AST from parsing C++ source files. The parameter `-c`
specifies which class to document.

    % umler -c ClassToDocument *.cpp

The needed compiler flags are extracted from a json compilation database which
can be generated automatically with `cmake` by specifying at configure time

    % cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON path/to/source

[Bear](https://github.com/rizsotto/Bear) can be used to extract compilation
databases for some other build systems. If no compilation database is used `--`
can be passed as last command line argument.

The infrastructure will automatically find the compilation database if it is
located in the current directory or any of its source directories.

Umler will then dump a [plantuml](http://plantuml.sourceforge.net/) description
of the parsed classes to stdout. The information to be dumped can be adjusted

* `-document-uses`: document *uses* relationships (default: `false`)
* `-document-owns`: document *owns* relationships (default: `false`)
* `-document-binds`: document *binds* relationships (default: `false`)
* `-document-methods`: document class methods (default: `true`)

To persist the parse result the internal database can be dumped with `-d`

    % umler -c ClassToDocument *.cpp -d db.sqlite

If subsequent invocations are given the same database parses the database will 
contain results from all parses. This allows to iteratively enhance descriptions. 
