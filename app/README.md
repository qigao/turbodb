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

## SQL editor boundary

The SQL workspace uses Scintilla as a UTF-8 editor and Lexilla for SQL lexing.
Editor state is separated from database ownership:

```text
SqlEditor (WTL/Scintilla)
    ↓
SqlLanguageService
    ↓
SqlWorkspaceSession
    ├── provider/capabilities
    ├── catalog/schema
    └── relation/column metadata
```

`SqlWorkspaceSession` stores only stable application identity and metadata.
Database/native handles remain outside the editor and will be owned by the
connection/query controller slices.

The editor already exposes selected/full SQL, caret state, find/replace,
undo/redo, Ctrl+Space completion, provider-aware lexer refresh, and diagnostic
marker hooks. Schema discovery, query execution, result rendering, and
execution plans remain tracked by #168-#171.
