// VENDORED FILE - DO NOT EDIT HERE.
//
// UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
// An edit made here is lost at the next sync, and until then it silently
// forks the editor away from the tree the desktop builds are cut from.
//
// Change it there; bring it back with pc64/tools/sync_unocode.py.
// See pc64/UNOCODE-UPSTREAM.md.
// The assistant (UCD-51): an agentic loop over vscode.lm with four tools -
// read_file, write_file, list_dir, and (where the platform can) run.
//
// THE TOOL PROTOCOL IS LINES, NOT JSON.  The model asks for a tool by
// replying with only a fenced block:
//
//     ```tool
//     read_file APP.PY
//     ```
//
// and write_file carries its content after the verb line.  A line protocol
// because the extension host has no JSON parser to lean on, and because a
// model mis-nesting quotes inside JSON-inside-JSON is a classic failure this
// sidesteps entirely.
//
// EVERY FILE WRITE IS SHOWN AS A DIFF FIRST, in the Assistant output channel,
// and applied only when the user picks Apply.  The model must not hold the
// pen - the same rule the chat view's proposals follow.

var out = vscode.window.createOutputChannel('Assistant');
var model = null;
var msgs = [];
var steps = 0;
var MAX_STEPS = 8;

// ---- the system prompt: Studio's platform knowledge, plus the tools -------
function sysPrompt() {
    var p =
        'You are the assistant inside UnoCode, the code editor of UnoDOS ' +
        'pc64. Apps are written in UnoC or in Python - both are first-class.\n' +
        'UnoC is a C subset: char/short/int/long (long is 4 bytes), pointers, ' +
        'arrays, structs/unions, enums, typedef, function pointers; no ' +
        'floats, varargs, or function-like macros. A UnoC app includes ' +
        '"UNO.H", defines const AppInterface *uno_app_main(const KernelApi ' +
        '*k) returning a draw/key/click/tick vtable, and draws through the ' +
        'KernelApi.\n' +
        'Python apps `import uno` and define a class subclassing uno.App ' +
        'with a module-global `app = MyApp()`. Methods: build(self, cv) ' +
        '(setup), draw(self, cv) (paint a frame), tick(self) (~60 Hz ' +
        'update), key(self, uni, scan, ctrl)->bool. `cv` is a Canvas: ' +
        'clear(color), fill_rect(x,y,w,h,color), rect(...), pixel(x,y,color), ' +
        'hline/vline, text(x,y,str,color), width(), height(). uno.rgb(r,g,b) ' +
        'makes a colour; uno.beep(midi,ticks), uno.read/write/size for ' +
        'files. Full float math is available in Python.\n' +
        'File names are FAT 8.3: at most 8 characters, a dot, at most 3 ' +
        'more, upper case, e.g. APP.PY or GAME.C.\n\n' +
        'You have tools. To use one, reply with ONLY a fenced block and ' +
        'nothing else:\n' +
        '```tool\n' +
        'read_file APP.PY\n' +
        '```\n' +
        'Tools:\n' +
        '  read_file NAME       - the file\'s text comes back\n' +
        '  list_dir DIR         - the entries (use . for the workspace root)\n' +
        '  write_file NAME      - the file\'s NEW content follows the verb ' +
        'line, inside the same fence\n';
    if (vscode.workspace.canRunPrograms)
        p += '  run NAME             - launch the program; a failure comes ' +
             'back with the error text, fix the file and run again\n';
    p += '\nAfter a line starting TOOL RESULT arrives, continue the task. ' +
         'When the task is complete, reply with a short summary and no tool ' +
         'block.';
    return p;
}

// ---- small string helpers (conservative: no split/regex assumed) ----------
function splitLines(s) {
    var a = [], i = 0, j;
    while (i <= s.length) {
        j = s.indexOf('\n', i);
        if (j < 0) { a.push(s.substring(i)); break; }
        a.push(s.substring(i, j));
        i = j + 1;
    }
    return a;
}

// The first tool fence in a reply, or null.
function findTool(reply) {
    var open = reply.indexOf('```tool');
    if (open < 0) return null;
    var bodyAt = reply.indexOf('\n', open);
    if (bodyAt < 0) return null;
    var close = reply.indexOf('```', bodyAt);
    if (close < 0) return null;
    var body = reply.substring(bodyAt + 1, close);
    var nl = body.indexOf('\n');
    var head = nl < 0 ? body : body.substring(0, nl);
    var content = nl < 0 ? '' : body.substring(nl + 1);
    var sp = head.indexOf(' ');
    if (sp < 0) return null;
    return {
        verb: head.substring(0, sp),
        path: head.substring(sp + 1),
        content: content
    };
}

// A minimal line diff: shared head and tail, then what leaves and arrives.
function lineDiff(oldText, newText) {
    var a = splitLines(oldText), b = splitLines(newText);
    var i = 0, ae = a.length - 1, be = b.length - 1, s = '', j;
    while (i <= ae && i <= be && a[i] === b[i]) i++;
    while (ae >= i && be >= i && a[ae] === b[be]) { ae--; be--; }
    for (j = i; j <= ae; j++) s += '- ' + a[j] + '\n';
    for (j = i; j <= be; j++) s += '+ ' + b[j] + '\n';
    return s === '' ? '(no change)\n' : s;
}

// ---- the loop -------------------------------------------------------------
function finish(text) {
    out.appendLine('=== done');
    out.show();
    vscode.window.showInformationMessage('Assistant: ' + text);
}

function step() {
    var r = model.sendRequest(msgs);
    r.onError(function (e) {
        out.appendLine('ERROR: ' + e);
        out.show();
        vscode.window.showErrorMessage('Assistant: ' + e);
    });
    r.onDone(function (full) {
        msgs.push(vscode.LanguageModelChatMessage.Assistant(full));
        out.appendLine('--- assistant');
        out.appendLine(full);
        var t = findTool(full);
        if (!t) { finish(full.length > 80 ? full.substring(0, 77) + '...' : full); return; }
        runTool(t);
    });
}

function feed(t, result) {
    steps++;
    if (steps >= MAX_STEPS) {
        out.appendLine('=== stopped: ' + MAX_STEPS + ' tool steps');
        out.show();
        vscode.window.showWarningMessage('Assistant: stopped after ' +
                                         MAX_STEPS + ' tool steps');
        return;
    }
    out.appendLine('--- tool result (' + t.verb + ' ' + t.path + ')');
    out.appendLine(result);
    msgs.push(vscode.LanguageModelChatMessage.User(
        'TOOL RESULT (' + t.verb + ' ' + t.path + '):\n' + result));
    step();
}

function runTool(t) {
    out.appendLine('--- tool: ' + t.verb + ' ' + t.path);
    if (t.verb === 'read_file') {
        var text;
        try { text = vscode.workspace.fs.readFile(t.path); }
        catch (e) { text = 'ERROR: ' + e.message; }
        feed(t, text);
    } else if (t.verb === 'list_dir') {
        var dir = t.path === '.' ? '' : t.path;
        var es = vscode.workspace.fs.readDirectory(dir), i, s = '';
        for (i = 0; i < es.length; i++)
            s += es[i][0] + (es[i][1] === 2 ? '\\' : '') + '\n';
        feed(t, s === '' ? '(empty)' : s);
    } else if (t.verb === 'run') {
        if (!vscode.workspace.canRunPrograms) {
            feed(t, 'ERROR: this platform cannot run programs');
            return;
        }
        var err = vscode.workspace.runUserProgram(t.path);
        feed(t, err === '' ? 'launched cleanly' : 'RUN FAILED:\n' + err);
    } else if (t.verb === 'write_file') {
        // the diff first, the pen only on consent
        var old = '';
        try { old = vscode.workspace.fs.readFile(t.path); } catch (e) {}
        out.appendLine('--- proposed write: ' + t.path);
        out.appendLine(lineDiff(old, t.content));
        out.show();
        vscode.window.showQuickPick(
            ['Apply the write to ' + t.path, 'Reject it'],
            'The assistant wants to write ' + t.path + ' - the diff is in ' +
            'the Assistant output'
        ).then(function (choice) {
            if (choice && choice.indexOf('Apply') === 0) {
                vscode.workspace.fs.writeFile(t.path, t.content);
                try { vscode.workspace.openTextDocument(t.path); } catch (e) {}
                feed(t, 'written');
            } else
                feed(t, 'REFUSED by the user - propose something else or stop');
        });
    } else
        feed(t, 'ERROR: no such tool; the tools are read_file, write_file, ' +
                'list_dir' +
                (vscode.workspace.canRunPrograms ? ', run' : ''));
}

exports.activate = function () {
    vscode.commands.registerCommand('assist.task', function () {
        vscode.window.showInputBox('What should I build or change?', '')
            .then(function (task) {
                if (!task) return;
                vscode.lm.selectChatModels().then(function (models) {
                    model = models[0];
                    msgs = [vscode.LanguageModelChatMessage.User(
                        sysPrompt() + '\n\nTASK: ' + task)];
                    steps = 0;
                    out.appendLine('=== task: ' + task);
                    out.show();
                    step();
                });
            });
    });
};
