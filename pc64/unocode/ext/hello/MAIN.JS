// Hello UnoCode - the sample extension.
//
// This is an ordinary CommonJS module: require('vscode'), export activate().
// It runs in the UnoCode extension host (unojs) under a fuel budget, so a
// runaway loop here stops the extension, not the machine.
//
// Everything it uses is in the API subset documented in UNOCODE.md.  The one
// shape that differs from VS Code: asynchronous calls return a THENABLE with
// .then(cb), not a Promise.

var vscode = require('vscode');

function activate(context) {
    var out = vscode.window.createOutputChannel('Hello');
    out.appendLine('Hello UnoCode activated.');

    // 1. A plain command.
    vscode.commands.registerCommand('hello.sayHello', function () {
        var ed = vscode.window.activeTextEditor;
        var where = ed ? ed.document.fileName : 'no editor';
        vscode.window.showInformationMessage('Hello from an extension - ' + where);
    });

    // 2. A command that edits the document through the editor API.
    vscode.commands.registerCommand('hello.insertBanner', function () {
        var ed = vscode.window.activeTextEditor;
        if (!ed) { vscode.window.showWarningMessage('Open a file first.'); return; }
        vscode.window.showInputBox('Banner text', 'SECTION').then(function (text) {
            if (!text) { return; }
            var line = '';
            for (var i = 0; i < 68; i++) { line += '='; }
            ed.insert('/* ' + line + '\n * ' + text + '\n * ' + line + ' */\n');
        });
    });

    // 3. A quick pick, wired to a built-in command.
    vscode.commands.registerCommand('hello.pickColorTheme', function () {
        vscode.window.showQuickPick(
            ['Dark+ (default dark)', 'Light+ (default light)', 'Monokai',
             'Solarized Dark', 'UnoDOS Blue'],
            'Which theme?'
        ).then(function (choice) {
            if (!choice) { return; }
            vscode.workspace.getConfiguration('workbench').update('colorTheme', choice);
            vscode.window.showInformationMessage('Theme: ' + choice);
        });
    });

    // 4. Reading the document.
    vscode.commands.registerCommand('hello.countLines', function () {
        var ed = vscode.window.activeTextEditor;
        if (!ed) { return; }
        var doc = ed.document;
        var blank = 0;
        for (var i = 0; i < doc.lineCount; i++) {
            if (doc.lineAt(i).text.length === 0) { blank++; }
        }
        vscode.window.showInformationMessage(
            doc.lineCount + ' lines, ' + blank + ' blank, language ' + doc.languageId);
    });

    // 5. A completion provider, for C only.
    vscode.languages.registerCompletionItemProvider('c', {
        provideCompletionItems: function (document, position) {
            return [
                { label: 'uno_fs_read', detail: 'UnoDOS', insertText: 'uno_fs_read(vol, name, buf, max)' },
                { label: 'uno_fs_write', detail: 'UnoDOS', insertText: 'uno_fs_write(vol, name, buf, len)' },
                { label: 'fb_fill_rect', detail: 'UnoDOS', insertText: 'fb_fill_rect(x, y, w, h, c)' },
                { label: 'line' + (position.line + 1), detail: 'where you are' }
            ];
        }
    });

    // 6. An event listener.
    vscode.workspace.onDidSaveTextDocument(function (doc) {
        out.appendLine('saved: ' + doc.fileName + ' (' + doc.lineCount + ' lines)');
    });

    context.subscriptions.push(out);
}

exports.activate = activate;
exports.deactivate = function () { };
