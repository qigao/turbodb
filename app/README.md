# TurboDB Studio

TurboDB Studio is the optional Windows desktop client for TurboDB.

This subtree owns native UI concerns only. Database semantics remain behind
TurboDB public APIs and runtime drivers.

## Initial dependency boundary

The `app` vcpkg manifest feature provides:

- WTL for native Windows shell/layout
- Scintilla for the SQL editor control
- Lexilla for editor lexing

These dependencies are resolved only when `TURBODB_BUILD_APP=ON`. They are
not linked into `Orm::C`, database drivers, or dbtools.

`chttp` is intentionally not part of the base application dependency closure.
A future HTTP/OpenAPI/remote feature may add it behind a separate explicit
option.

## Build

Configure on Windows with the repository vcpkg toolchain and:

~~~powershell
cmake -S . -B build/app `
  -DTURBODB_BUILD_APP=ON `
  -DTURBODB_BUILD_ORM=ON
cmake --build build/app --target turbodb-studio
~~~

The first slice provides the frame and pane ownership boundaries only:

~~~text
TurboDB Studio
├── Connections / Schemas
└── SQL workspace
    ├── editor host
    └── result host
~~~

The editor, schema explorer, query execution, and execution-plan behavior are
tracked separately by #167-#171.
