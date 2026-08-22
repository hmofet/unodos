> VENDORED FILE - DO NOT EDIT HERE.
>
> UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
> An edit made here is lost at the next sync, and until then it silently
> forks the editor away from the tree the desktop builds are cut from.
>
> Change it there; bring it back with pc64/tools/sync_unocode.py.
> See pc64/UNOCODE-UPSTREAM.md.

# UnoCode, subsystem contract

**Where this file is canonical:** `hmofet/unocode-desktop`, in `core/`. UnoDOS
vendors it, along with the rest of the editor, into `pc64/unocode/` - see
[`core/README.md`](README.md) there and `pc64/UNOCODE-UPSTREAM.md` here. The
copy in an UnoDOS checkout carries a do-not-edit banner; this one does not,
because this is the one to edit.

**Owner:** UnoCode Desktop's `ROADMAP.md` and `AGENTS.md`. In UnoDOS, the
`UnoCode` row in `/AGENTS.md` §1 records it as vendored.
**Public surface:** the ON-DISK FORMATS in this document, plus the unoui-class
module ABI (`pc64/uno_uuiapp.h`) every app already speaks. `unocode.h` is
subsystem-internal: nothing outside the editor directory may include it -
`core/` here, `pc64/unocode/` there.
**Contract version:** 1.0, `[EXPERIMENTAL]` until the second extension written
by somebody other than its author loads unmodified.

UnoCode is a code editor for UnoDOS/pc64 in the shape of Visual Studio Code:
an activity bar, a side bar with five views, tabbed editors with a minimap,
an integrated terminal, a command palette, and an extension host. It ships as
`APPS\UNOCODE.UNO` and a distro drops it by not shipping the file.

**The formats below are VS Code's.** A colour theme, a keybindings file, a
snippet file and an extension manifest written for VS Code load here, and the
same file loads in VS Code. That is the whole design constraint: an editor
nobody can write an extension for is a text editor with ambitions.

---

## 1. Where things live

FAT has 8.3 names, so the FILE names are shortened and the CONTENT is not.
Every loader accepts the long spelling too, for a volume that can carry it.

```
UNOCODE\SETTINGS.JSN     user settings          (VS Code: settings.json)
UNOCODE\KEYBIND.JSN      user keybindings       (keybindings.json)

<folder>\TASKS.JSN       tasks for this folder  (.vscode/tasks.json)
<folder>\LAUNCH.JSN      run configurations     (.vscode/launch.json)

EXT\<ID>\PACKAGE.JSN     an extension manifest  (package.json)
EXT\<ID>\MAIN.JS         its entry point, if it has one
EXT\<ID>\THEMES\*.JSN    colour themes
EXT\<ID>\SYNTAX\*.JSN    TextMate grammars
EXT\<ID>\SNIPPET\*.JSN   snippets
```

`UNOCODE\` goes on the volume `uno_fs_pref_vol()` picks - the boot volume
first, never the RAM disk when a real one exists. `EXT\` is looked for on
*every* mounted volume, so an extension on a second disk or a USB stick is
found without being installed anywhere; the first volume to offer a given `<ID>`
wins.

**Every file is JSONC**: `//` and `/* */` comments are legal, and so is a
trailing comma. The default settings file VS Code ships opens with a comment;
a parser that rejected that would reject the files people actually write.

---

## 2. Extensions

### 2.1 The manifest

```jsonc
{
    "name": "hello",
    "displayName": "Hello UnoCode",
    "description": "What it does.",
    "version": "1.0.0",
    "publisher": "you",
    "main": "MAIN.JS",                    // omit for a declarative extension
    "activationEvents": ["onCommand:hello.sayHello"],
    "permissions": ["languageModels"],    // only if vscode.lm is used - see 2.4

    "contributes": {
        "commands":    [{ "command": "id", "title": "Title", "category": "Cat" }],
        "keybindings": [{ "key": "ctrl+k h", "command": "id", "when": "editorTextFocus" }],
        "themes":      [{ "label": "Nord", "uiTheme": "vs-dark", "path": "THEMES/NORD.JSN" }],
        "languages":   [{ "id": "unoscript", "aliases": "UnoScript",
                          "extensions": [".URC"],
                          "configuration": { "comments": { "lineComment": "#" } } }],
        "grammars":    [{ "language": "unoscript", "scopeName": "source.unoscript",
                          "path": "SYNTAX/URC.JSN" }],
        "snippets":    [{ "language": "c", "path": "SNIPPET/C.JSN" }]
    }
}
```

`path` may use `/` or `\`; it is resolved against the extension's own
directory and upper-cased for FAT.

**Declarative extensions cost no interpreter.** A manifest with no `main` -
a theme, a language, a grammar, a snippet set - is read at startup and never
runs a line of JavaScript. Twenty installed themes are twenty manifest reads.

### 2.2 Activation

A manifest DECLARES its commands, so they are in the command palette
immediately. The JavaScript that implements them runs when an activation
event fires:

| event | fires |
|---|---|
| `onCommand:<id>` | the first time that command is invoked, from anywhere |
| `onStartupFinished` | once, after the workbench is up |
| `*` | at startup, unconditionally |

An extension with a `main` and no `activationEvents` is treated as
`onStartupFinished`.

### 2.3 The extension host

Extensions run in **unojs** (`unojs/UNOJS.md`), embedded in the module. They
are CommonJS modules: `require('vscode')`, assign `exports.activate`.

**A runaway extension cannot hang the machine.** Every entry from C into JS
runs on a fuel budget (`extensions.fuelPerSlice`, resumed a bounded number of
times); an extension that does not finish is stopped, reported, and disabled.
On a system with no preemption and no process boundary that is not a nicety,
it is the whole safety story - and it is implemented in exactly one function
(`call_fn` in `uc_api.c`) so there is one place that can hang and one place
that says why it did not.

### 2.4 The API

The names and shapes are `vscode`'s.

```js
vscode.version

vscode.commands.registerCommand(id, fn)      // also registerTextEditorCommand
vscode.commands.executeCommand(id)
vscode.commands.getCommands()                // -> [id, ...]

vscode.window.showInformationMessage(msg)    // also Warning / Error
vscode.window.setStatusBarMessage(msg)
vscode.window.showQuickPick(items, placeholder)   // -> Promise
vscode.window.showInputBox(placeholder, value)    // -> Promise
vscode.window.createOutputChannel(name)      // .append .appendLine .show
vscode.window.activeTextEditor               // accessor, undefined if none

editor.document.getText() / .lineAt(n) / .lineCount / .fileName
      / .languageId / .isDirty / .save()
editor.selection.start | .end | .active | .isEmpty   // {line, character}
editor.getSelectedText() / .replaceSelection(s) / .insert(s) / .setCursor(l, c)
editor.edit(fn)                              // fn(builder): .insert(pos, s)
                                             //              .replace(range, s)

vscode.workspace.name / .rootPath
vscode.workspace.getConfiguration(section)   // .get(key, dflt) .update(key, v)
vscode.workspace.openTextDocument(path)
vscode.workspace.fs.readFile(path) / .writeFile(path, text)
vscode.workspace.fs.readDirectory(path)      // -> [[name, type], ...]; 1 file,
                                             //    2 directory; "" = the root
vscode.workspace.canRunPrograms              // can this platform launch one?
                                             // (the terminal's own child
                                             //  processes are UCD-14 and are
                                             //  not exposed to extensions)
vscode.workspace.runUserProgram(path)        // "" = launched; else the reason
                                             // (on pc64, the Python error)
vscode.workspace.onDidSaveTextDocument(fn)   // also onDidOpen / onDidChange

vscode.languages.registerCompletionItemProvider(selector, provider)
    // provider.provideCompletionItems(document, position)
    //   -> [{ label, detail, insertText }] or [string]

// on the context handed to activate(context):
context.secrets.store(name, value)           // -> Promise
context.secrets.get(name)                    // -> Promise<string|undefined>
context.secrets.delete(name)                 // -> Promise

// the model - REQUIRES "languageModels" in PACKAGE.JSN "permissions"
vscode.lm.selectChatModels()                 // -> Promise<[model]>
model.sendRequest(messages)                  // -> a response object
    // messages: strings, {role, content} objects, or
    // vscode.LanguageModelChatMessage.User(s) / .Assistant(s)
response.onText(fn)                          // fn(delta) per chunk
response.onDone(fn)                          // fn(fullText) at the end
response.onError(fn)                         // fn(message) on any failure
response.cancel()                            // tear the exchange down

vscode.Position(line, character)   vscode.Range(l1, c1, l2, c2)
console.log / .info / .warn / .error      -> the "Extension Host" output channel
```

**PROMISES ARE REAL** (UCD-21). Every asynchronous call here returns an actual
`Promise`: `.then`, `.catch`, `.finally`, `Promise.resolve/reject/all`, and
`async`/`await` all work, because unojs has a microtask queue and a suspending
`await` underneath. This paragraph used to be a deviation saying the opposite.

Reactions run on the next frame, never inside the call that settled them -
which is the standard's ordering, and the reason an extension can settle a
promise from a callback without re-entering the interpreter.

**One deliberate deviation, because of what this machine is:**

1. **`require` resolves only `'vscode'`.** There is no module resolver and no
   `node_modules`; anything else throws immediately rather than failing later
   and less clearly.

**`vscode.lm`** is the model API in `vscode.lm`'s shape, and it is GATED: an
extension host that can reach a model can leak a workspace into a prompt, and
`EXT\` is a folder anyone can drop a file into - so reaching the model is a
privilege declared in the manifest (`"permissions": ["languageModels"]`), the
file a user can read before enabling anything. An undeclared caller is refused
with an Error that names the missing declaration. Deviations from VS Code:
there is ONE model (the `ai.model` setting; the selector argument is
ignored), ONE request at a time, and the response streams through
`onText`/`onDone`/`onError` callbacks rather than an async iterator - there
is no event loop to build one on. The key is UCD-48's `anthropic.key`; no
key is an Error at `sendRequest`, worded to say how to set one.

**`context.secrets`** is `SecretStorage`'s shape with its trust model stated
rather than implied. Names are prefixed with the extension's identity by the
host, so one extension cannot read another's secrets - or the editor's own API
key - by guessing a string. What backs it is the PLATFORM'S answer, not ours:
DPAPI on Windows, the Keychain on macOS, a file only the user can read on
Linux, and on UnoDOS a plain file on FAT - the last two are honestly named in
the UI when a key is saved, because FAT protects nothing and saying otherwise
would be a padlock icon on an open door. Thenables resolve on the next frame,
not synchronously. `onDidChange` and enumeration do not exist: a list of
secret NAMES is itself information an extension has no business asking for.

Not implemented (and therefore not pretended): hover providers, definition
providers, diagnostics from extensions, webviews, tree-view containers, tasks
contributed from JavaScript, and the `Uri` class beyond plain paths.

That list is about what an **extension** may contribute, and it is unchanged by
section 8: the editor gets diagnostics and completions from language servers
directly, over a client written in C, without going through this API at all.

---

## 3. Colour themes

A theme file IS a VS Code colour theme:

```jsonc
{ "name": "Nord", "type": "dark",
  "colors": { "editor.background": "#2E3440", ... },
  "tokenColors": [ { "scope": ["keyword", "storage"],
                     "settings": { "foreground": "#81A1C1",
                                   "fontStyle": "italic" } } ] }
```

Two rules make a hand-written theme workable:

- **An unknown colour key is ignored.** VS Code has several hundred workbench
  colours; UnoCode paints with about ninety. A theme written against a newer
  editor still loads.
- **An unset key is DERIVED, not left black.** The whole workbench is computed
  from `type` plus whatever the theme did set, so a three-key theme renders
  correctly. This is why writing a theme by hand here is reasonable.

An eight-digit `#RRGGBBAA` colour is flattened against the editor background
when it is read: there is no alpha buffer to composite it into later.

Token scopes resolve by **longest dotted prefix**, TextMate's rule:
`keyword.control.c` is matched by `keyword` and by `keyword.control`, and the
longer one wins. `fontStyle` supports `bold`, `italic` and `underline`.

Built in: Dark+, Light+, Monokai, Solarized Dark, High Contrast, UnoDOS Blue.
They are tables of key/hex pairs fed through the *same* apply path as a file,
so the loader is exercised on every boot and cannot rot.

---

## 4. Grammars

A grammar is TextMate's model, cut to what a module can carry: a list of
patterns, each a one-line `match`, a `begin`/`end` pair with nested
`patterns`, or an `include` of `#name` from `repository` or of `$self`.
`captures` / `beginCaptures` assign scopes to capture groups.

The regex engine (`uc_rx.c`) supports literals, `.`, character classes with
ranges and negation, `\w \W \d \D \s \S \b \B`, anchors, `* + ? {n,m}` greedy
and lazy, alternation, and capture / non-capture groups. It **rejects**
backreferences and lookaround at compile time rather than mis-matching them
silently, and it is an iterative matcher with a step budget - a pathological
pattern returns "no match", it does not hang the desktop.

**One documented deviation.** Cross-line state is ONE open `begin`/`end` rule,
not a stack. Within a line, nesting is arbitrary; across a line break only the
outermost open rule is remembered. That covers block comments and multi-line
strings - everything real code leaves open at a newline - and it is what makes
the per-line state a single 16-bit number, which is what makes scrolling a
6000-line file free. Anything deeper re-syncs at the next line rather than
being coloured wrongly for the rest of the file.

Built-in languages: plaintext, C, Python, JavaScript, JSON, Markdown, HTML,
CSS.

---

## 5. Settings

`UNOCODE\SETTINGS.JSN` is a flat map of dotted keys. Nested objects are
accepted and read as their dotted spelling, because VS Code writes the flat
form and contributed configuration uses the nested one.

Every setting is declared in one table (`kDefs` in `uc_cfg.c`) with a type, a
default and a range; the Settings editor, the terminal's `get`/`set`, and
`getConfiguration()` all read that one table, so they cannot drift apart.

Notable keys: `editor.fontSize` `editor.tabSize` `editor.insertSpaces`
`editor.wordWrap` `editor.lineNumbers` `editor.minimap.enabled`
`editor.renderWhitespace` `editor.autoClosingBrackets` `editor.quickSuggestions`
`files.autoSave` `files.eol` `files.trimTrailingWhitespace`
`workbench.colorTheme` `workbench.sideBar.location` `breadcrumbs.enabled`
`terminal.integrated.fontSize` `extensions.autoActivate`
`extensions.fuelPerSlice` `extensions.heapMB` `extensions.disabled`.

---

## 6. Keybindings

`UNOCODE\KEYBIND.JSN` is an array of `{ "key", "command", "when" }`, VS Code's
records exactly. A leading `-` on the command REMOVES a default binding.
Chords are two chords separated by a space (`"ctrl+k ctrl+s"`).

Resolution is **last wins**: user beats extension beats default. `when` is a
real boolean expression over context keys - `!`, `&&`, `||`, parentheses,
`==` and `!=` against a literal - and a key this build has never heard of is
false, so a clause naming a future context is harmless.

Context keys: `editorFocus` `editorTextFocus` `editorHasSelection`
`editorHasMultipleSelections` `editorReadonly` `editorLangId` `sideBarVisible`
`panelVisible` `terminalFocus` `inQuickOpen` `suggestWidgetVisible`
`findWidgetVisible`.

**Open Keyboard Shortcuts (JSON)** writes the whole shipped default keymap out
in the file's own syntax the first time, so changing one binding does not
start with guessing what a key is called.

### What the keyboard can and cannot deliver

Two roads carry keys into a module, and they carry different things.

- The unoui **canvas event stream** carries the mouse, the navigation keys and
  printable characters *with a full modifier mask*. Alt chords work here.
- The module **`key(uni, scan, ctrl)` hook** carries Ctrl chords and the
  function keys, which never become canvas events at all. It has no Shift
  flag - but the transports report the *shifted character*, so `Ctrl+Shift+P`
  arrives as `'P'` where `Ctrl+P` arrives as `'p'`, and that is how Shift is
  recovered. **Alt chords cannot be seen on that road**, which is why the
  Alt bindings are all on keys that arrive as canvas events.
- **USB HID keyboards deliver no function keys in this build** (`hid_kbd.c`
  translates usages below 0x39 and F1..F12 are 0x3A..0x45). PS/2 keyboards do,
  including QEMU's. F-key bindings are therefore parsed, displayed and honoured
  where the transport provides them; the default keymap does not depend on any
  of them. See the request filed in `pc64/UNOAUTOMATE-REQUESTS.md`.

---

## 7. The integrated terminal

There is no shell process on pc64 to host, and no process model to host one in,
so the terminal is a small shell of its own over what a module already has.
`help` lists exactly what exists and an unknown word is an error - never a
silent no-op that reads like a shell that ran something.

`ls cd cat pwd vol mkdir rm cp echo find wc open run set get theme ext cmd js
task clear uptime ver`

`js <expr>` evaluates in the **same** unojs VM the extensions run in, which is
the difference between an extension system you can debug and one you can only
stare at. `cmd <id>` runs any UnoCode command, which makes every command
scriptable from `TASKS.JSN`.

`run <file>` runs a `.UNO` through the app loader, or wraps a `.PY` in a
`UNO_MODF_PYAPP` container (the app-registry lane's format, written field for
field so the container is byte-identical to `tools/mkuno.py pyapp`'s) and hands
it to PYRT.

---

## 8. Language servers

`uc_lsp.h` is a Language Server Protocol client: a server per language, spoken
to over pipes in JSON-RPC. It is a subsystem, not a platform seam - it sits on
`uc_proc.h`, and on pc64, where `uc_proc_available()` answers 0, it never starts
anything and every call costs nothing. The editor keeps the grammar-derived
heuristics it has.

Which server serves which language is a table with built-in defaults, overridden
from `SETTINGS.JSN`:

```json
{
  "lsp.enabled": true,
  "lsp.trace": false,
  "lsp.servers": {
    "python": "pyright-langserver --stdio",
    "rust": ""
  }
}
```

An empty command turns one language off; `lsp.enabled: false` turns them all
off. `lsp.trace` puts the traffic in the **Language Server** Output channel,
which is the only way to diagnose the failure this protocol produces most
often - a server that starts and then says nothing.

Four decisions worth knowing before changing anything:

- **Pipes, not a pty.** A pty echoes what you write, translates newlines, and
  has a line discipline that will rewrite a byte inside a message body. That is
  right for a human and fatal for a protocol, so `uc_proc_spawn_pipes()` is a
  separate call rather than a flag on the existing one. stderr is kept separate:
  merging it corrupts the stream, discarding it loses the only explanation a
  server gives when it dies before saying anything protocol-shaped.
- **Full document sync, on a quiet timer.** Incremental sync means client and
  server each keep a copy of the text and agree on every edit forever; one
  mis-ranged change and they diverge silently, with the server answering
  questions about a file that no longer exists. Full sync cannot diverge, and
  sending it only after 300 ms of no typing is what makes it affordable.
- **Server-initiated requests are answered, always.** pyright will not finish
  starting until `workspace/configuration` is answered, and the answer must be
  an array with one entry per item requested. Anything else the client does not
  recognise is answered with a null result rather than an error: an unknown
  request refused politely is survivable, one never answered is not.
- **Death is ordinary.** A server that exits is restarted after 1s, 2s, 4s and
  so on to a 32s cap, its documents are re-opened on the replacement, and the
  backoff resets once it has behaved for a minute.

### 8.1 Columns

There are now **four** ways to count a column in UnoCode, and they agree only on
tab-free ASCII:

| unit | who uses it | `"    char *s = "🙂🙂"; int y = bad"` |
|---|---|---|
| bytes | the buffer, `uc_replace_range` | 34 before `bad` |
| code points | `uc_col_of` / `uc_offset_of`, `UcProblem.col` | 28 |
| visual cells | `vcol_of`, the painter and the hit test | 30 |
| UTF-16 units | **LSP, and nothing else** | 30 |

An emoji is one code point, two UTF-16 units, two cells and four bytes, so a
conversion done in the wrong unit is correct in every ASCII test and wrong from
the first non-ASCII character onwards - which reads as "the server's ranges are
off" rather than as this side's bug. `uc_lsp_pos_to_offset()` and
`uc_lsp_offset_to_u16()` are the only code that may convert; everything else
takes a document offset.

`UcProblem` stores **1-based code-point** columns, because that is what
`uc_offset_of()` consumes and what the Problems panel's click already hands it.

### 8.2 Completions

Where a server answers, its list replaces the local one entirely - the word
scraper's whole point is to be useful in a language nothing understands, and
mixing its guesses into an answer that does understand the language makes the
good half harder to find. Where no server answers, nothing changes.

Three things follow from the protocol being asynchronous and the widget not
being:

- The widget opens on the local list and is **replaced** when the reply lands.
- Every request carries a **generation**; a reply from a superseded generation
  is dropped. This is why the widget never shows completions for a prefix you
  have already finished typing.
- A reply with no items **closes** the widget, because it may have been opened
  empty on the strength of a server merely being attached.

Ranking is the **server's**, not the fuzzy matcher's: it knows which overload
is in scope and which member is private, and a score derived from how the
letters line up does not.

### 8.3 Navigation

F12 goes to the definition, Shift+F12 finds all references into the Search
view, and Alt+Left / Alt+Right walk a **browser history** of caret locations -
not a stack, so a back pressed one time too many is recoverable.

Every jump in the workbench goes through it: Go to Definition, a Search result,
a row in the Problems panel. A history entry is a `(volume, directory, name,
line, column)`, never a document pointer, so it survives the tab being closed.

A definition **outside the workspace** - a system header - is reported by name
rather than silently doing nothing: the editor addresses files by volume and
cannot open one it was never given.

### 8.4 Rename and format

F2 renames the symbol at the caret across every file the server names; the box
opens pre-filled, because a rename that starts empty makes you retype a name you
are changing one letter of. Shift+Alt+F formats. `editor.formatOnSave` (default
false) formats **before** writing, and only on an explicit Ctrl+S.

A server's edits are always applied **last first**. A `TextEdit`'s range is
stated against the document as the server saw it, so front-to-back application
invalidates every range after the first one that changed a length. They arrive
unsorted - the protocol only forbids overlap - so they are sorted here.

Each file gets **one undo step**, and is left open and dirty rather than saved:
a rename you cannot look at before it reaches the disk is one you cannot undo
your way out of.

### 8.5 Hover

`editor.hover.enabled` (default true). The pointer must rest for 450 ms over a
word; Ctrl+K Ctrl+I asks at the caret instead, which is what a keyboard-driven
session wants because the pointer is wherever it was abandoned.

The server's answer is markdown and is **stripped, not rendered** - fences and
their language tag, headings, and emphasis runs. Nothing else is interpreted, so
an underscore inside an identifier stays an underscore. An empty answer shows
nothing at all: clangd returns one for whitespace and punctuation, which is most
of where a pointer comes to rest.

---

---

## 9. Testing

The same two files are tested in both trees. `tools/test.sh` finds its own
layout, so it is vendored verbatim rather than kept as two copies that drift.

```sh
# UnoCode Desktop, where the editor is canonical
sh core/tools/test.sh                        # host: the JSONC parser + regex
./build.sh --gate                            # + the four seam suites + a render

# UnoDOS, where it is vendored - and the only place a kernel break is visible
sh unocode/tools/test.sh                     # the same file, the same 71 checks
cd pc64 && sh tools/gate.sh                  # runs the above as a stage
cd pc64 && UNO_DEBUG=1 ./build.sh && \
    python3 unocode/tools/unocode_urc.py     # QEMU: the merge gate
```

**Run the UnoDOS side before calling a change done.** The desktop build
compiles the editor and its foundations, never the kernel, so a change to
anything the kernel also compiles is green there and can fail outright here.

`tools/test.sh` builds `uc_json.c` and `uc_rx.c` natively - they have no
framebuffer, no toolkit and no filesystem in them, and every config file,
theme, keymap, manifest and grammar arrives through those two files, so a bug
in either shows up as "themes stopped loading" three layers away.

`unocode_urc.py` boots the real image under QEMU and drives it over URC (QEMU's
usb-tablet delivers no pointer motion to this guest - see `tools/urcui.py`),
leaving a screenshot per step in `pc64/shots/`. It proves the module loads, the
palette filters, the terminal runs, an extension-contributed theme applies, an
extension's JavaScript command runs, a file highlights, and typing lands in the
order it was typed.

That last check exists because it caught a real bug that every other check
passed through: a cursor-adjustment boundary that never advanced the caret on a
plain insertion, so every line came out backwards. Nothing else in the gate
typed into a *document*.

---

## Changelog

- **1.0** (2026-08-20) First landing. Workbench, editor, themes, settings,
  keybindings, grammars, snippets, terminal, tasks, and the extension host.
- **1.1** (2026-08-21) `context.secrets` (UCD-48): SecretStorage over the
  platform's own store, the `AI: Set API Key` / `AI: Clear API Key` commands
  with a masked input box, and the store named on screen whenever a key is
  saved. Keys never enter `SETTINGS.JSN`.
- **1.10** (2026-08-22) Rename Symbol and Format Document (UCD-27). F2,
  Shift+Alt+F, and `editor.formatOnSave`, which formats before writing rather
  than after. A server's edits are applied last first, which is the only order
  in which ranges stated against the unedited document all stay valid.
- **1.9** (2026-08-22) Go to Definition, Find All References, and a navigation
  history (UCD-26). F12, Shift+F12, Alt+Left and Alt+Right; references fill the
  Search view; every jump in the workbench now records where it came from.
- **1.8** (2026-08-22) Hover (UCD-25): the server's answer about the symbol
  under the pointer, after a dwell, with its markdown stripped to readable
  text. Ctrl+K Ctrl+I asks at the caret. `editor.hover.enabled`.
- **1.7** (2026-08-22) Completions from language servers (UCD-24). Where a
  server answers, its list replaces the scraped words, in its order and with
  its kinds and signatures; where none does - plaintext, or pc64, where there
  are no processes at all - the word scraper is untouched and still the answer.
- **1.6** (2026-08-21) Diagnostics from language servers (UCD-23): squiggles
  under the reported range, marks in the minimap and on a new overview ruler,
  the Problems panel and the status-bar counts fed from real servers, and the
  counts coloured by severity. `UcProblem` grew an end position and a
  directory; see section 8.1 for the four column units and which one it stores.
- **1.5** (2026-08-21) A Language Server Protocol client (UCD-22): servers
  spawned on pipes, JSON-RPC framed over stdio, full-text document sync on a
  quiet timer, and a bounded restart when one dies. No UI yet - section 8
  describes the settings and the traffic log. Verified live against clangd and
  pyright; a killed server comes back and re-opens its documents.
- **1.4** (2026-08-21) Real Promises and `async`/`await` (UCD-21): unojs
  gained a microtask queue and a suspending `await`, so every asynchronous
  call in this API returns a genuine Promise and deviation #1 is gone.
- **1.3** (2026-08-21) Tier 1. The listing seam takes the caller's STRIDE
  rather than a baked-in 16 (UCD-11), so names up to `UC_NAME_MAX` (256)
  cross verbatim and the desktop's FAT-style alias table is gone. Workspace
  search recurses and runs in slices (UCD-12) and can replace across files
  with one undo step per file (UCD-13). The terminal runs real child
  processes through a pty where the platform has one (UCD-14) and tasks use
  them, with compiler diagnostics matched into the Problems panel (UCD-15).
  Ctrl+D adds a cursor, paste distributes across cursors, Alt+Shift drags a
  column selection (UCD-16). Go to Symbol (UCD-17). Split editors, each group
  with its own view (UCD-18). Files can be dropped on the window (UCD-19).
  Indentation and line endings are detected per file and preserved on save
  (UCD-20).
- **1.2** (2026-08-21) The assistant view (UCD-49): a sixth activity-bar view
  holding a streaming chat, code blocks in the editor's grammars, and edits
  proposed as a diff before they are applied as one undo step. `vscode.lm`
  (UCD-50): the model for extensions, behind the `languageModels` manifest
  permission. The Assistant extension (UCD-51, `EXT\ASSIST`): an agentic
  loop with read_file / write_file / list_dir - and run where the platform
  can (`workspace.canRunPrograms`) - every write shown as a diff and applied
  only on consent. Its tool protocol is documented at the top of its
  `MAIN.JS`, which is also the worked example of `vscode.lm`.
