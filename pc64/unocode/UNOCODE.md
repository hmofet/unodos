# UnoCode, subsystem contract

**Owner:** the `unocode` row in [`/AGENTS.md`](../../AGENTS.md) §1.
**Public surface:** the ON-DISK FORMATS in this document, plus the unoui-class
module ABI (`pc64/uno_uuiapp.h`) every app already speaks. `unocode.h` is
subsystem-internal and nothing outside `pc64/unocode/` may include it.
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
vscode.window.showQuickPick(items, placeholder)   // -> thenable
vscode.window.showInputBox(placeholder, value)    // -> thenable
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
vscode.workspace.onDidSaveTextDocument(fn)   // also onDidOpen / onDidChange

vscode.languages.registerCompletionItemProvider(selector, provider)
    // provider.provideCompletionItems(document, position)
    //   -> [{ label, detail, insertText }] or [string]

vscode.Position(line, character)   vscode.Range(l1, c1, l2, c2)
console.log / .info / .warn / .error      -> the "Extension Host" output channel
```

**Two deliberate deviations, both because of what this machine is:**

1. **Thenables, not Promises.** `showQuickPick` and `showInputBox` return an
   object with `.then(cb)`. There is no event loop and no microtask queue to
   build a real Promise on; `.then` is the part extensions use, and it works.
   `.catch`, `async`/`await` and `Promise.all` do not exist.
2. **`require` resolves only `'vscode'`.** There is no module resolver and no
   `node_modules`; anything else throws immediately rather than failing later
   and less clearly.

Not implemented (and therefore not pretended): hover providers, definition
providers, diagnostics from extensions, webviews, tree-view containers, tasks
contributed from JavaScript, and the `Uri` class beyond plain paths.

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

## 8. Testing

```sh
cd pc64/unocode && sh tools/test.sh          # host: the JSONC parser + regex
cd pc64 && UNO_DEBUG=1 ./build.sh && \
    python3 unocode/tools/unocode_urc.py     # QEMU: the merge gate
```

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
