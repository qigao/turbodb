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

From a Visual Studio developer environment, use the Windows Release user
preset. It enables Studio and its required ORM drivers through the repository
vcpkg manifest. This setting belongs to the user preset; CI profiles remain
separate.

WTL also requires ATL from the selected Visual Studio installation. Install
**C++ ATL for latest v143 build tools (x86 & x64)**
(`Microsoft.VisualStudio.Component.VC.ATL`) through Visual Studio Installer
if that instance lacks `atlbase.h`; the vcpkg WTL package does not supply ATL.
See the [Visual Studio component list](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022).
With multiple VS installations, configure and build from the same instance's
x64 developer environment. When switching instances, use
`cmake --preset win-release-user --fresh` to replace the cached compiler.

~~~powershell
cmake --preset win-release-user
cmake --build --preset win-release-user --target turbodb-studio
ctest --preset win-release-user -R "^turbodb-app-" --output-on-failure
~~~

### CMake Tools with multiple Visual Studio instances

CMake Tools can automatically select a newer Visual Studio instance that has
the compiler but lacks ATL. Installing ATL in another instance does not make
it available to the selected compiler.

To use an existing instance with ATL, set this in the editor's workspace
settings, then save your work and fully exit the editor:

~~~json
{
  "cmake.useVsDeveloperEnvironment": "never"
}
~~~

Start the editor from that instance's x64 developer environment. For example,
from the repository root in PowerShell, with VS 2022 Professional and VS Code:

~~~powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && code .'
~~~

If the current shell already uses another VS developer environment
(`VSINSTALLDIR` is set), clean it in the same child process before switching:

~~~powershell
cmd /d /c 'call "%VSINSTALLDIR%Common7\Tools\VsDevCmd.bat" -clean_env >nul && call "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && code .'
~~~

Calling another instance's `VsDevCmd.bat` without cleaning can retain the old
`VSINSTALLDIR` and continue selecting that instance's compiler and headers.

Use your installed Visual Studio instance's path. An already running VS Code
process may retain its old environment, so opening
another window or an integrated terminal is insufficient.

In the newly launched editor's terminal, run
`cmake --preset win-release-user --fresh`, then select `win-release-user` in
CMake Tools. Keep launching the editor from this developer environment while
the setting is `never`; it makes CMake Tools inherit the editor's environment.
See the [CMake Tools environment selection implementation](https://github.com/microsoft/vscode-cmake-tools/blob/v1.23.52/src/presets/preset.ts)
and [Microsoft's developer prompt instructions](https://code.visualstudio.com/docs/cpp/config-msvc#_check-your-microsoft-visual-c-installation).

The first slice provides the frame and pane ownership boundaries only:

~~~text
TurboDB Studio
├── Connections / Schemas
└── SQL workspace
    ├── editor host
    └── result host
~~~

## Menus and keyboard commands

### Open a database connection

Choose **File > New Connection** (`Ctrl+N`), select SQLite, PostgreSQL, or
MySQL from the Driver dropdown. The driver module is selected automatically:
an INI profile uses its exact `module_path`; manual entry uses the default
matching profile's module, then the first matching profile's module, or (when
there is no matching profile) `turbodb_driver_sqlite.dll`,
`turbodb_driver_postgresql.dll`, or `turbodb_driver_mysql.dll` beside the
executable. Module paths stay internal and are not displayed in the connection
form; custom module locations belong in the INI, not a second driver picker.
The absolute DLL path and selected
driver identity are passed to the existing ORM runtime, which loads the module
through Salts Plugin and validates its CMeta `TurboDb.Driver` contract.
No directory scan or automatic driver substitution occurs.

For SQLite, use `:memory:` for a temporary database, select an existing database
file, or enter a file path to create a database. PostgreSQL and MySQL require
host, port, database, user, CA certificate, and TLS server name; password may
be empty. PostgreSQL uses the driver's direct TLS transport. Both network
providers verify the CA and server name, with a 10-second connect timeout
(PostgreSQL also has a 10-second TLS handshake timeout).

Driver loading, connection setup and schema discovery run on one background
task while the modal dialog continues processing paint and window messages.
The form and close actions are disabled during the attempt: the current drivers
do not expose cancellation for this operation. Failures restore the editable
form with an error; success publishes the schema and closes the dialog.
Form edits and credentials are not written back to disk.
SQLite uses a compact form; network and server-verification fields appear for
PostgreSQL and MySQL. Switching providers selects the matching module and preserves
each provider's database and port within the current dialog; passwords are cleared on every
provider change. Errors expand below the form and clear when fields are edited.
Native controls use the Windows Common Controls v6 theme from the embedded
application manifest, including their system focus and disabled states.
After connection, the schema explorer and SQL language context are refreshed.
This workspace uses one active connection: choose **File > Disconnect**
(`Ctrl+Shift+D`) before opening another. Disconnect is unavailable while SQL
or PLAN/ANALYZE work is active or its result is awaiting UI consumption.
Connecting and disconnecting clear the displayed results and plans.

**File > Recent Connections** loads the same INI without first opening the
connection dialog. It initially lists the default profile followed by the other
configured profiles (up to 32). Selecting an entry attempts that connection
directly; missing passwords or connection errors leave the form open for
correction. Successfully used profiles move to the top for this Studio session;
cancelled/failed attempts do not reorder the list. Entries are disabled while a
connection is active or work is pending. Loading another valid INI replaces the
list; an invalid replacement preserves it. An empty/error state points to
New Connection / Load INI. This menu does not persist manual form entries,
passwords, or session ordering, and does not rewrite the INI.

#### Connection task ownership

The frame owns one pending `std::async` future. It transfers a copy of the
profile to the worker and rejects additional connection/query/close actions
until the result is consumed. The worker exclusively borrows `ConnectionManager`
and builds a separate schema model/session; it never accesses UI controls or
the live workspace. A dialog timer polls readiness without waiting. `get()`
synchronizes completion before the UI publishes the model/session. Failed
preparation closes the newly opened connection before reporting the error.
The future is destroyed before the frame's other members, so forced teardown
joins the task before destroying its connection/runtime/plugin owners.

This keeps the existing caller-blocking ORM/Plugin contract and connection
identity owner, with one worker and one completion slot. Running the same calls
on the UI thread froze the dialog; a general task queue or new driver async ABI
would add unnecessary scope. Driver timeouts remain unchanged and are not an
overall schema-discovery deadline. Closing waits for the driver to return;
there is no forced cancellation or detached worker. No configuration or database
format migration is required; the synchronous `OpenConnection` entry continues
to share the same preparation/publication path.

### INI connection configuration

Copy [examples/TurboDBStudio.ini](examples/TurboDBStudio.ini) beside
`TurboDBStudio.exe`. Opening **File** or **New Connection** first loads that
optional file; the connection dialog selects its default profile. Use
**Load INI…** to choose another file, then choose a named **Profile**. Loading
only fills the form; **Connect** (or choosing a **Recent Connections** entry)
explicitly loads the driver and opens the database. Loaded profiles remain available after
disconnecting during the current Studio session. Editing a field changes the
selection to **Manual entry** and does not modify the INI file.

~~~ini
[studio]
default_connection = local

[connection.local]
driver = sqlite
module_path = turbodb_driver_sqlite.dll
display_name = Local SQLite
database = :memory:
~~~

Each `[connection.NAME]` section defines one profile. Keys and driver IDs are
case-sensitive. The file is UTF-8, with an optional BOM; CRLF and LF are accepted.
The existing SaltsUtils `Salts::IniParser` supplies INI parsing, avoiding a new
parser or YAML dependency. Studio owns the resulting form presets; ORM and
Salts Plugin retain all module admission and connection lifecycle decisions.

| Key | Meaning |
| --- | --- |
| `driver` | Required: `sqlite`, `postgresql`, or `mysql`. |
| `module_path` | Required: the matching driver DLL. |
| `database` | Required: SQLite file path or `:memory:`; network database name. |
| `display_name` | Optional; defaults to the profile's `NAME`. |
| `host`, `port` | Network only; default to `localhost` and `5432` / `3306`. |
| `user` | Required for network providers. |
| `ca_certificate`, `tls_server_name` | Required for network providers; server verification remains enabled. |
| `password_env` | Optional network password environment variable, read when selecting and connecting the profile. |

DLL, SQLite database, and CA certificate paths can be absolute or relative to
the **INI file's directory**. Drive-relative paths such as `C:driver.dll` and
root-relative paths such as `\driver.dll` are rejected. Configuration filenames
and form values support Unicode through the app's native UTF-16 adapter.
Network timeout and PostgreSQL transport settings remain the same as manual
connections.

`[studio]` is optional; `default_connection` names a profile. Without it, the
first profile is selected. The reader accepts at most 64 KiB, 32 profiles, and
4096 UTF-8 bytes per field. Unknown keys/sections, unsupported drivers, invalid
ports, missing required fields, and malformed files fail visibly and preserve
the previous configuration. As in Salts IniParser, repeated sections merge and
the last duplicate key wins. Values are unquoted; `;` and `#` introduce comments.

Set passwords in the environment before launching Studio, for example:

~~~ini
password_env = TURBODB_REPORTING_PASSWORD
~~~

An unset named variable produces an editable form error naming the variable
and explaining recovery. Enter a password to switch to Manual entry, or set
the variable before launching Studio. Omit `password_env` to use an empty
password or enter one manually. A plaintext
`password` key is rejected. The loader reports schema errors without echoing
configuration values. Studio does not save or rewrite configuration files.

The native menu bar provides File (New Connection, Recent Connections, Disconnect, Exit), Edit (SQL undo/redo and clipboard
actions), Query (Execute SQL, Execution Plan, Analyze SQL), View (focus the
schema explorer, SQL editor, or results), and Help (About).

Use Alt plus a menu mnemonic to open a menu. F5 or Ctrl+Enter executes SQL,
F6 requests a plan, and Shift+F6 requests ANALYZE from any workspace pane.
ANALYZE keeps its existing confirmation because it may execute the statement.
Query actions use selected SQL when available, otherwise the whole editor.
Edit shortcuts apply in the SQL editor; menu actions target that editor.
In a displayed execution plan, **View > Plan Details** (`Ctrl+Shift+P`) focuses
the raw plan. Tab moves from the plan tree to its raw details, Shift+Tab returns
to the tree, and Tab from raw details returns to the editor. Ctrl+A selects the
raw details and Ctrl+C copies the selection using the native edit control.
Unavailable undo/redo and clipboard actions are disabled when the menu opens,
as are query actions while work or result consumption is pending.

The top toolbar groups Execute / Plan / Analyze, Undo / Redo, and
Schemas / Editor / Results. Each action has a native icon and a visible label;
buttons wrap onto additional rows when the window narrows. The toolbar and
menus share command handlers and availability checks. Use View > Toolbar or
Ctrl+Shift+T to focus it, arrow keys to choose a button, Enter or Space to
activate, and Tab or Escape to return to the visible query pane.
Its grouping is informed by the [CuteSqlite query interface](https://github.com/shinehanx/CuteSqlite#readme).

Schemas / Editor / Results are pane visibility switches, not focus-only buttons.
A checked button (recessed surface and underline) or a check in the View menu
means the pane is shown. Click again to hide it, or to restore it and focus its
contents. Schemas applies to the workspace; Editor/Results visibility belongs
to each query tab. At least one of those two query panes remains visible:
hiding the last one shows its sibling. The existing native splitters own these
states and retain divider positions; no SQL, undo history, results, or plans
are discarded or recreated. Keyboard navigation skips hidden panes, and toolbar
Tab/Escape returns to the visible query pane. Execute/Plan reveals output without
taking editor focus; View > Plan Details also restores a hidden result pane.

Studio also follows that interface's use of color to distinguish actions:
Execute is green, Plan is blue, Analyze is amber, Schemas is purple, and the
Editor is teal. The app-local palette in `src/studio_colors.h` is shared with
SQL syntax colors, a tinted line-number gutter/current line, and alternating
result rows. Following the reference screenshot, a charcoal command bar uses
bright action icons and light labels, while content stays white with compact
light-gray section headings and neutral row stripes. The schema tree has
distinct database, folder, table, view, and column glyphs; recoloring its image
list preserves native selection and expansion. Toolbar hover and press states
have distinct gray fills with a two-pixel light-blue keyboard focus outline.
Result status strips use green for success, amber while running, and red for
errors, always paired with explanatory text; message bodies stay white.
Connection hints and validation errors use the same semantic palette.
Native controls still own input, disabled states, tooltips, and selected-row
rendering. Windows High Contrast uses system colors and native toolbar/result
rendering instead. System color/theme changes refresh the palette
without replacing the editor document or clearing its undo history.
This is an independent implementation on the existing WTL/Scintilla stack;
no CuteSqlite source, icons, or additional UI dependencies are bundled.

The empty sidebar and result pane explain how to connect or execute SQL. The
sidebar's read-only empty state is keyboard-focusable; Tab returns to the editor.
Running states
show the action and cancellation limit without worker/request implementation
details. Floating-point grid values use locale-independent `max_digits10`
precision so distinct `double` values remain distinguishable and round-trip
through their displayed text; this may display more digits than earlier builds.

## SQL editor boundary

### Tab ownership and interaction design

Use **File > New Query** (`Ctrl+T`), **Close Query** (`Ctrl+W`), and
`Ctrl+Tab` / `Ctrl+Shift+Tab` to switch queries. The visible New/Close buttons
and query-tab context menu use the same commands. `Ctrl+Shift+Q` focuses the
query strip: arrows select a tab, Tab enters its editor, and Shift+Tab reaches
the New/Close controls. The editor retains Shift+Tab for SQL outdent. A `*`
marks edits; SQL is not saved to disk or restored after exit. Closing a
nonempty/modified page or exiting asks before discarding it. Close is disabled
while a query or plan is running or waiting for UI consumption.

Right-click a table/view in the schema tree for **Open First 100 Rows**,
**SELECT in New Query**, **Copy Name**, or **Refresh Schema** (`Ctrl+R`).
Double-click its label or press Enter to open the bounded query. Other tree
nodes offer copy/refresh, with relation-only commands disabled. Refresh runs on
a background task, publishes metadata only on success, and preserves query
text/results. Menus also work with the menu key or Shift+F10. The SQL editor's
context menu combines Execute/Plan with undo/redo and clipboard commands.

Each query has **Results**, **Messages**, and **Plan** tabs. The most recent
row result and successful plan remain selectable in that page; Messages shows
the latest operation, including failures. These are not an execution history.
Right-click a result cell for Copy Cell, Copy Row, or Copy All Rows. `Ctrl+C`
copies the selected row; `Ctrl+Shift+C` copies the context cell (the first column
for a keyboard-opened menu). Rows use tabs/newlines between displayed values;
this is plain text, not a lossless database export (NULL/BLOB use their display
labels). Messages and raw plans retain the native text-selection/copy menu.

The workspace owns up to 32 query pages. Each page owns its native editor,
undo history, caret/scroll state, splitter, and result controls; switching pages
does not copy SQL or reconstruct controls. Creation is committed only after all
controls succeed; failures leave existing pages intact. Closing nonempty or
modified SQL requires discard confirmation (there is no session persistence).
The last page is replaced with an empty page so query commands always have a
target. Pages and controls are UI-thread-only.

The existing connection and query controllers remain the sole database owners.
Execution remains serialized per connection. The frame borrows the initiating
page until the completion is consumed, and blocks page closing while work is
active or pending. Switching/new pages are allowed; completion never targets
whichever page happens to be selected. Worker threads do not borrow page or
editor objects. Connection changes clear every page's results and language
context but retain SQL. No database format or public ORM ABI changes are needed.

This uses composed WTL pages instead of document-pointer swapping (which would
require separately saving selection, undo, results, and scroll state), and avoids
a new docking framework. The tradeoff is more native controls per open page,
bounded by the page limit. Rollback is app-only; no stored state is migrated.

Object actions use loaded metadata and provider-specific identifier quoting;
opening a table/view runs a bounded `SELECT ... LIMIT 100` in a new query page.
Database writes, DDL designers, and import/export are outside this interaction
slice. Clipboard operations copy displayed text, bounded to 16 MiB, and report
failure instead of silently truncating.

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

The editor exposes selected/full SQL, caret state, find/replace, undo/redo,
provider-aware lexer refresh, and diagnostic marker hooks.

### Completion and contextual help

- Typing an identifier prefix of at least two UTF-8 bytes opens completion for
  keywords, supported built-ins, and loaded relation/column names. Qualified
  names such as `items.na` are supported. `Ctrl+Space` requests completion
  explicitly, including with an empty prefix. Arrow keys select; Tab or Enter
  accepts, replacing the rest of the current word.
- Function calls display a signature with the current argument highlighted,
  including nested calls. The completion list takes priority while selecting a
  name. `Ctrl+Shift+Space` requests parameter help or help for the symbol at the
  caret without requiring a mouse.
- Hover over a table, view, column, or supported built-in for 600 ms to show its
  help while the editor has focus. Field tips identify matching loaded owners;
  ambiguous unqualified names show up to four matches and a qualification hint.
- Escape dismisses a list or tip without changing SQL. Editing, moving the
  caret, scrolling, leaving the editor, or changing connection context clears
  stale tips. Comments, strings, and lexer-recognized quoted identifiers do not
  trigger assistance. Programmatic text changes do not open completion.
- Toolbar tooltips explain actions and their shortcuts, including the warning
  that Analyze may execute the statement.

Function help covers the portable forms of `abs`, `lower`, `upper`, `length`,
`coalesce`, `nullif`, `replace`, `substr`, `count`, `sum`, and `avg`. It is curated
UI guidance, not overload resolution: user-defined and schema-qualified
functions are not inferred. Parameter context scans at most 4096 preceding
UTF-8 bytes and is not a full SQL parser. Metadata tips use the session's loaded
names only; they do not infer SQL aliases, column types, or comments, and clear
on disconnect. No additional schema query runs on hover or keystrokes.

The implementation reuses [Scintilla completion and calltips](https://www.scintilla.org/ScintillaDoc.html#Autocompletion)
and Lexilla styles. Built-in forms follow the database documentation:
[SQLite scalar functions](https://www.sqlite.org/lang_corefunc.html),
[SQLite aggregates](https://www.sqlite.org/lang_aggfunc.html),
[PostgreSQL string functions](https://www.postgresql.org/docs/current/functions-string.html),
and [MySQL string functions](https://dev.mysql.com/doc/refman/8.4/en/string-functions.html).
