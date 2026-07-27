/*  RemoteMain.cs - UnoDOS Remote Desktop client (WinForms).
 *
 *  Wraps the URC channel (Urc.cs) in a GUI: a live view of the device screen
 *  (polls `screen grab`, decodes QOI via Qoi.cs), mouse + keyboard forwarding
 *  (URC `pointer` / `key`), session recording (Recorder.cs), a log pane fed by
 *  the URC LOG stream, a clickable verb bar, and a raw-command box.  Built with
 *  csc as a single winexe, like the flasher.
 *
 *  pc64 dials OUT to us, so we LISTEN.  Put this machine's LAN ip:port in the
 *  device's DEBUG.CFG (`remote=<ip>:<port>`) and boot a debug build.
 */
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.Threading;
using System.Windows.Forms;

namespace UnoRemote
{
    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            int autoPort = 0;
            if (args.Length > 0) int.TryParse(args[0], out autoPort);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm(autoPort));
        }
    }

    /// <summary>Double-buffered live-view control that also captures input and
    /// maps view coordinates back to device framebuffer coordinates.</summary>
    sealed class ScreenView : Control
    {
        private Bitmap _frame;
        private Rectangle _dest;              // where the frame is drawn (letterboxed)
        public int DevW = 640, DevH = 480;    // device framebuffer size
        public event Action<int, int, int> PointerEvent;   // (fbX, fbY, btn)

        public ScreenView()
        {
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                     ControlStyles.AllPaintingInWmPaint | ControlStyles.Selectable, true);
            BackColor = Color.Black;
            TabStop = true;
        }

        public void SetFrame(Bitmap b)
        {
            var old = _frame; _frame = b;
            if (old != null) old.Dispose();
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(BackColor);
            if (_frame == null) return;
            // letterbox: preserve aspect within the client area
            double sx = (double)ClientSize.Width / _frame.Width;
            double sy = (double)ClientSize.Height / _frame.Height;
            double s = Math.Min(sx, sy);
            if (s <= 0) return;
            int w = (int)(_frame.Width * s), h = (int)(_frame.Height * s);
            _dest = new Rectangle((ClientSize.Width - w) / 2, (ClientSize.Height - h) / 2, w, h);
            e.Graphics.InterpolationMode = InterpolationMode.NearestNeighbor;
            e.Graphics.PixelOffsetMode = PixelOffsetMode.Half;
            e.Graphics.DrawImage(_frame, _dest);
        }

        private bool MapToFb(int mx, int my, out int fx, out int fy)
        {
            fx = fy = 0;
            if (_dest.Width <= 0 || _dest.Height <= 0) return false;
            if (mx < _dest.Left || mx >= _dest.Right || my < _dest.Top || my >= _dest.Bottom) return false;
            fx = (int)((mx - _dest.Left) * (double)DevW / _dest.Width);
            fy = (int)((my - _dest.Top) * (double)DevH / _dest.Height);
            if (fx < 0) fx = 0; if (fx >= DevW) fx = DevW - 1;
            if (fy < 0) fy = 0; if (fy >= DevH) fy = DevH - 1;
            return true;
        }

        private DateTime _lastMove = DateTime.MinValue;
        protected override void OnMouseDown(MouseEventArgs e)
        {
            Focus();
            int fx, fy; if (MapToFb(e.X, e.Y, out fx, out fy)) Emit(fx, fy, 1);
        }
        protected override void OnMouseUp(MouseEventArgs e)
        {
            int fx, fy; if (MapToFb(e.X, e.Y, out fx, out fy)) Emit(fx, fy, 0);
        }
        protected override void OnMouseMove(MouseEventArgs e)
        {
            // throttle moves to ~30 ms so we don't flood the link
            if ((DateTime.UtcNow - _lastMove).TotalMilliseconds < 30) return;
            _lastMove = DateTime.UtcNow;
            int fx, fy; int btn = (e.Button == MouseButtons.Left) ? 1 : 0;
            if (MapToFb(e.X, e.Y, out fx, out fy)) Emit(fx, fy, btn);
        }
        private void Emit(int x, int y, int btn)
        {
            var h = PointerEvent; if (h != null) h(x, y, btn);
        }
    }

    sealed class MainForm : Form
    {
        private readonly UrcLink _link = new UrcLink();
        private readonly Recorder _rec = new Recorder();
        private readonly ScreenView _view = new ScreenView();
        private readonly TextBox _port = new TextBox { Text = "5099", Width = 60 };
        private readonly Button _listen = new Button { Text = "Listen", Width = 70 };
        private readonly Button _record = new Button { Text = "Record", Width = 70, Enabled = false };
        private readonly CheckBox _srvCap = new CheckBox { Text = "on device", AutoSize = true, Padding = new Padding(4, 8, 2, 0) };
        private readonly ComboBox _scale = new ComboBox { Width = 90, DropDownStyle = ComboBoxStyle.DropDownList };
        private readonly Label _status = new Label { AutoSize = true, Text = "idle" };
        private readonly TextBox _log = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, WordWrap = false };
        private readonly TextBox _cmd = new TextBox();
        private readonly Button _send = new Button { Text = "Send", Width = 60 };

        private Thread _screenThread;
        private volatile bool _pumping;
        private volatile bool _needKeyframe;   // seed the canvas with a full grab
        private volatile int _scaleFactor = 1;
        private const double FpsTarget = 10.0;
        private readonly int _autoPort;
        private long _frames;
        private Bitmap _canvas;   // composited full frame (screen thread only)

        public MainForm(int autoPort)
        {
            _autoPort = autoPort;
            Text = "UnoDOS Remote Desktop";
            Width = 1000; Height = 760;
            StartPosition = FormStartPosition.CenterScreen;
            KeyPreview = true;

            // ---- top bar ----
            var bar = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 34, Padding = new Padding(4), WrapContents = false };
            bar.Controls.Add(new Label { Text = "Port:", AutoSize = true, Padding = new Padding(4, 8, 2, 0) });
            bar.Controls.Add(_port);
            bar.Controls.Add(_listen);
            _scale.Items.AddRange(new object[] { "1x (full)", "2x (half)", "3x (third)", "4x (quarter)" });
            _scale.SelectedIndex = 0;
            bar.Controls.Add(new Label { Text = "Scale:", AutoSize = true, Padding = new Padding(8, 8, 2, 0) });
            bar.Controls.Add(_scale);
            bar.Controls.Add(_record);
            bar.Controls.Add(_srvCap);
            bar.Controls.Add(_status);

            // ---- bottom: log + verb bar + command box ----
            var bottom = new Panel { Dock = DockStyle.Bottom, Height = 224 };
            var cmdRow = new Panel { Dock = DockStyle.Bottom, Height = 28 };
            _cmd.Dock = DockStyle.Fill; _send.Dock = DockStyle.Right;
            cmdRow.Controls.Add(_cmd); cmdRow.Controls.Add(_send);
            _log.Dock = DockStyle.Fill; _log.Font = new Font(FontFamily.GenericMonospace, 8.5f);
            bottom.Controls.Add(_log);      // Fill (top)
            bottom.Controls.Add(cmdRow);    // Bottom (outermost)
            BuildVerbBar(bottom);           // Bottom, stacks above the command row

            _view.Dock = DockStyle.Fill;

            Controls.Add(_view);
            Controls.Add(bottom);
            Controls.Add(bar);

            // ---- wiring ----
            _listen.Click += (s, e) => ToggleListen();
            _record.Click += (s, e) => ToggleRecord();
            _send.Click += (s, e) => SendRaw();
            _cmd.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { SendRaw(); e.SuppressKeyPress = true; } };
            _scale.SelectedIndexChanged += (s, e) => _scaleFactor = _scale.SelectedIndex + 1;

            _view.PointerEvent += (x, y, btn) => Fire(() => _link.Pointer(x, y, btn));
            KeyDown += MainForm_KeyDown;
            KeyPress += MainForm_KeyPress;

            _link.OnLog += (ch, t) => AppendLog("[" + ch + "] " + t);
            _link.OnMessage += t => AppendLog("<msg> " + t);
            _link.OnDiscovery += t => AppendLog(t);
            _link.OnConnected += () => BeginInvoke((Action)OnConnected);
            _link.OnDisconnected += () => BeginInvoke((Action)OnDisconnected);

            FormClosing += (s, e) => { _pumping = false; _rec.Stop(); _link.Dispose();
                                       var c = _canvas; _canvas = null; if (c != null) c.Dispose(); };
            Log("Put this PC's LAN ip:port in the device DEBUG.CFG:  remote=<ip>:" + _port.Text);
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            if (_autoPort > 0) { _port.Text = _autoPort.ToString(); ToggleListen(); }
        }

        // ---- listen / connection ----
        private bool _listening;
        private void ToggleListen()
        {
            if (_listening) return;   // stop-listen is a follow-up nicety
            int port; if (!int.TryParse(_port.Text, out port)) { Log("bad port"); return; }
            try { _link.Listen("0.0.0.0", port); }
            catch (Exception ex) { Log("listen failed: " + ex.Message); return; }
            _listening = true; _listen.Enabled = false; _port.Enabled = false;
            _status.Text = "listening on :" + port + " - boot the device";
            Log("listening on 0.0.0.0:" + port + "  (QEMU SLIRP guest: remote=10.0.2.2:" + port + ")");
            Log("zero-config: set 'discover' in the device DEBUG.CFG and it will find this PC automatically.");
        }

        private void OnConnected()
        {
            _status.Text = "connected";
            _record.Enabled = true;
            try { int w, h; _link.ScreenInfo(out w, out h); if (w > 0) { _view.DevW = w; _view.DevH = h; Log("device screen " + w + "x" + h); } }
            catch (Exception ex) { Log("screen info: " + ex.Message); }
            StartScreenLoop();
        }

        private void OnDisconnected()
        {
            _status.Text = "disconnected - waiting for redial";
            _pumping = false;
            if (_rec.Recording) { Log("recording stopped (link dropped): " + _rec.Stop()); _record.Text = "Record"; }
            if (_srvRecording) { _srvRecording = false; Log("device recording lost (link dropped)"); _record.Text = "Record"; }
            _record.Enabled = false; _srvCap.Enabled = true; _scale.Enabled = true;
        }

        // ---- screen pump ----
        private void StartScreenLoop()
        {
            _pumping = true;
            _needKeyframe = true;   // a fresh connection has no canvas to delta against
            _screenThread = new Thread(ScreenLoop) { IsBackground = true, Name = "screen-pump" };
            _screenThread.Start();
        }

        private void ScreenLoop()
        {
            double frameMs = 1000.0 / FpsTarget;
            while (_pumping && _link.Connected)
            {
                var t0 = DateTime.UtcNow;
                try
                {
                    int w, h, nch; bool kf;
                    Bitmap frame;
                    if (_needKeyframe)
                    {
                        // Seed the canvas with a full grab. The device keeps its
                        // delta snapshot across a TCP reconnect, so we can't start
                        // from a delta - a full grab reseeds both ends in step.
                        byte[] qoi = _link.ScreenGrab(_scaleFactor, out w, out h);
                        ReplaceCanvas(Qoi.Decode(qoi));
                        _needKeyframe = false;
                        kf = true; nch = 0;
                    }
                    else
                    {
                        var u = _link.ScreenGrabDelta(_scaleFactor);
                        ApplyUpdate(u);
                        w = u.W; h = u.H; kf = u.Keyframe; nch = u.Nch;
                    }
                    frame = (Bitmap)_canvas.Clone();
                    _rec.Frame(frame);
                    long fn = ++_frames;
                    // hand the clone to the view; recorder read from it already
                    int fw = w, fh = h, fnch = nch; bool fkf = kf;
                    if (!IsDisposed) BeginInvoke((Action<Bitmap>)(b =>
                    {
                        _view.SetFrame(b);
                        Text = "UnoDOS Remote Desktop - " + fw + "x" + fh + " - frame " + fn +
                               (fkf ? " [key]" : " [" + fnch + " tiles]") +
                               (_rec.Recording ? " - REC" : "");
                    }), frame);
                }
                catch (Exception ex)
                {
                    if (_pumping) AppendLog("screen: " + ex.Message);
                    _needKeyframe = true;   // resync after any hiccup
                    Thread.Sleep(500);
                }
                double elapsed = (DateTime.UtcNow - t0).TotalMilliseconds;
                if (elapsed < frameMs) Thread.Sleep((int)(frameMs - elapsed));
            }
        }

        // Replace the persistent canvas with a decoded full frame.
        private void ReplaceCanvas(Bitmap full)
        {
            var old = _canvas; _canvas = full;
            if (old != null) old.Dispose();
        }

        // Composite one live-view update onto the persistent canvas: a keyframe
        // replaces it; a delta blits only its changed tiles.
        private void ApplyUpdate(UrcLink.ScreenUpdate u)
        {
            if (u.Keyframe) { ReplaceCanvas(Qoi.Decode(u.Qoi)); return; }
            if (_canvas == null)                          // lost sync: force a reseed next loop
            { _canvas = new Bitmap(Math.Max(1, u.W), Math.Max(1, u.H), PixelFormat.Format32bppArgb); }
            if (u.Nch <= 0 || u.Qoi == null) return;      // static frame: nothing changed

            using (var strip = Qoi.Decode(u.Qoi))         // Tw x (Nch*Th) tile strip
                BlitTiles(_canvas, strip, u.TileIdx, u.Cols, u.Tw, u.Th, u.W, u.H);
        }

        // Blit a decoded tile strip (Tw x (n*Th)) onto a canvas at the tile
        // positions given by their row-major indices. Shared by the live-view
        // delta compositor and the server-capture reconstruction.
        private static void BlitTiles(Bitmap canvas, Bitmap strip, int[] idx,
                                      int cols, int tw, int th, int w, int h)
        {
            using (var g = Graphics.FromImage(canvas))
            {
                g.CompositingMode = CompositingMode.SourceCopy;
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                for (int i = 0; i < idx.Length; i++)
                {
                    int t = idx[i];
                    int col = t % cols, row = t / cols;
                    int dx = col * tw, dy = row * th;
                    int vw = Math.Min(tw, w - dx);        // clamp partial edge tiles
                    int vh = Math.Min(th, h - dy);
                    if (vw <= 0 || vh <= 0) continue;
                    g.DrawImage(strip, new Rectangle(dx, dy, vw, vh),
                                new Rectangle(0, i * th, vw, vh), GraphicsUnit.Pixel);
                }
            }
        }

        // ---- recording ----
        private volatile bool _srvRecording;
        private static string VideosDir()
        {
            return System.IO.Path.Combine(Environment.GetFolderPath(
                Environment.SpecialFolder.MyVideos), "UnoRemote");
        }

        private void ToggleRecord()
        {
            if (_srvCap.Checked) { ToggleServerRecord(); return; }

            if (!_rec.Recording)
            {
                string dst = _rec.Start(_view.DevW / _scaleFactor, _view.DevH / _scaleFactor, FpsTarget, VideosDir());
                _record.Text = "Stop"; _srvCap.Enabled = false;
                Log((_rec.UsingFfmpeg ? "recording -> " : "recording frames -> ") + dst);
            }
            else
            {
                string dst = _rec.Stop();
                _record.Text = "Record"; _srvCap.Enabled = true;
                Log("recording saved: " + dst);
            }
        }

        // Server-side capture: the device records on its own tick (steady fps,
        // independent of our poll rate); on stop we pull the ring, reconstruct
        // every frame, and write it out through a private Recorder (so it never
        // mixes with a client-side recording on _rec).
        private void ToggleServerRecord()
        {
            if (!_link.Connected) { Log("not connected"); return; }
            if (!_srvRecording)
            {
                _record.Enabled = false;
                Fire(() =>
                {
                    try
                    {
                        var st = _link.ScreenRecordStart(_scaleFactor, (int)FpsTarget);
                        _srvRecording = true;
                        BeginInvoke((Action)(() =>
                        {
                            _record.Text = "Stop (dev)"; _record.Enabled = true; _scale.Enabled = false;
                            Log("device recording at " + Get(st, "fps") + " fps (scale " + _scaleFactor + ")");
                        }));
                    }
                    catch (Exception ex) { AppendLog("record start: " + ex.Message);
                                           BeginInvoke((Action)(() => _record.Enabled = true)); }
                });
            }
            else
            {
                _record.Enabled = false;
                Fire(RunServerCapture);
            }
        }

        private void RunServerCapture()
        {
            try
            {
                var st = _link.ScreenRecordStop();
                _srvRecording = false;
                int nbytes = Get(st, "bytes"), ew = Get(st, "ew"), eh = Get(st, "eh");
                int cols = Get(st, "cols"), tw = Get(st, "tw"), th = Get(st, "th");
                int fps = Get(st, "fps"), nframes = Get(st, "frames"), dropped = Get(st, "dropped");
                AppendLog("device captured " + nframes + " frames (" + nbytes + " bytes" +
                          (dropped > 0 ? ", " + dropped + " dropped - ring full" : "") + "); pulling...");
                byte[] data = _link.ScreenRecordReadAll(nbytes);

                var rec = new Recorder();
                string dst = rec.Start(ew, eh, fps > 0 ? fps : FpsTarget, VideosDir());
                Bitmap canvas = null; int p = 0, emitted = 0;
                try
                {
                    while (p + 12 <= data.Length)
                    {
                        int typ = data[p];
                        int nch = data[p + 2] | (data[p + 3] << 8);
                        int strip = data[p + 4] | (data[p + 5] << 8) | (data[p + 6] << 16) | (data[p + 7] << 24);
                        int payload = data[p + 8] | (data[p + 9] << 8) | (data[p + 10] << 16) | (data[p + 11] << 24);
                        p += 12;
                        if (p + payload > data.Length) break;
                        if (typ == 0)                                    // keyframe
                        {
                            byte[] q = new byte[payload]; Array.Copy(data, p, q, 0, payload);
                            if (canvas != null) canvas.Dispose();
                            canvas = Qoi.Decode(q);
                        }
                        else if (canvas != null && nch > 0 && strip > 0)  // delta
                        {
                            byte[] sq = new byte[strip]; Array.Copy(data, p, sq, 0, strip);
                            int[] idx = new int[nch];
                            for (int i = 0; i < nch; i++)
                                idx[i] = data[p + strip + i * 2] | (data[p + strip + i * 2 + 1] << 8);
                            using (var stbmp = Qoi.Decode(sq))
                                BlitTiles(canvas, stbmp, idx, cols, tw, th, ew, eh);
                        }
                        p += payload;
                        if (canvas != null)
                        {
                            using (var c = (Bitmap)canvas.Clone()) rec.Frame(c);
                            emitted++;
                        }
                    }
                }
                finally { if (canvas != null) canvas.Dispose(); }
                string saved = rec.Stop();
                AppendLog("server recording: " + emitted + " frames -> " + saved);
            }
            catch (Exception ex) { AppendLog("server capture: " + ex.Message); }
            finally
            {
                _srvRecording = false;
                if (!IsDisposed) BeginInvoke((Action)(() =>
                { _record.Text = "Record"; _record.Enabled = true; _scale.Enabled = true; }));
            }
        }

        private static int Get(System.Collections.Generic.Dictionary<string, int> d, string k)
        { int v; return d.TryGetValue(k, out v) ? v : 0; }

        // ---- keyboard forwarding ----
        // UEFI SimpleTextInput scan codes (uefi_main.c map_key handles the arrows,
        // Esc, Delete; the rest are harmless no-ops on the device today).
        private static int ScanFor(Keys k)
        {
            switch (k)
            {
                case Keys.Up: return 0x01;   case Keys.Down: return 0x02;
                case Keys.Right: return 0x03; case Keys.Left: return 0x04;
                case Keys.Home: return 0x05; case Keys.End: return 0x06;
                case Keys.Insert: return 0x07; case Keys.Delete: return 0x08;
                case Keys.PageUp: return 0x09; case Keys.PageDown: return 0x0A;
                case Keys.Escape: return 0x17;
            }
            if (k >= Keys.F1 && k <= Keys.F12) return 0x0B + (k - Keys.F1);
            return 0;
        }

        private void MainForm_KeyDown(object sender, KeyEventArgs e)
        {
            if (!_view.Focused) return;
            int scan = ScanFor(e.KeyCode);
            if (scan != 0)
            {
                int ctrl = e.Control ? 1 : 0;
                Fire(() => _link.Key(scan, 0, ctrl));
                e.SuppressKeyPress = true;   // don't also fire KeyPress
            }
        }

        private void MainForm_KeyPress(object sender, KeyPressEventArgs e)
        {
            if (!_view.Focused) return;
            int ctrl = (Control.ModifierKeys & Keys.Control) != 0 ? 1 : 0;
            int uni = e.KeyChar;
            Fire(() => _link.Key(0, uni, ctrl));
            e.Handled = true;
        }

        // ---- raw command box ----
        private void SendRaw()
        {
            string line = _cmd.Text.Trim();
            if (line.Length == 0) return;
            _cmd.Clear();
            Execute(line);
        }

        // Run one raw URC command line (from the box or a verb button).
        private void Execute(string line)
        {
            line = line.Trim();
            if (line.Length == 0) return;
            if (!_link.Connected) { Log("not connected"); return; }
            if (line.StartsWith("/msg ")) { _link.Message(line.Substring(5)); Log("> " + line); return; }
            Log("> " + line);
            string[] parts = line.Split(' ');
            string verb = parts[0];
            object[] args = new object[parts.Length - 1];
            for (int i = 1; i < parts.Length; i++) args[i - 1] = parts[i];
            Fire(() =>
            {
                try { var r = _link.Command(verb, args); foreach (var l in r) AppendLog("  " + l); AppendLog("  ok"); }
                catch (Exception ex) { AppendLog("  ! " + ex.Message); }
            });
        }

        // ---- clickable verb bar ----
        // A verb button either runs immediately, prefills the command box for a
        // verb that needs an argument (label ends "..."), or confirms first
        // (label ends "!"). Grows from - and shares Execute() with - the raw box.
        private void BuildVerbBar(Panel host)
        {
            var bar = new FlowLayoutPanel
            { Dock = DockStyle.Bottom, Height = 52, WrapContents = true, AutoScroll = true,
              Padding = new Padding(2, 2, 2, 0), BackColor = SystemColors.Control };
            // label -> command line; "..." = prefill + focus, "!" = confirm first
            var verbs = new[]
            {
                "probe", "vols", "disks", "devices", "apps", "uptime", "disc",
                "screen info", "close",
                "launch ...", "test ...", "py ...", "guard ...", "safe",
                "arm ...", "install ...", "bootnext ...", "eth ...", "iwl ...",
                "reboot !", "poweroff !",
            };
            foreach (var spec in verbs)
            {
                bool needsArg = spec.EndsWith(" ...");
                bool confirm = spec.EndsWith(" !");
                string cmd = needsArg ? spec.Substring(0, spec.Length - 4)
                           : confirm ? spec.Substring(0, spec.Length - 2) : spec;
                var b = new Button { Text = needsArg ? cmd + "…" : cmd, AutoSize = true, Margin = new Padding(2) };
                string command = cmd;
                b.Click += (s, e) =>
                {
                    if (!_link.Connected) { Log("not connected"); return; }
                    if (needsArg) { _cmd.Text = command + " "; _cmd.Focus(); _cmd.SelectionStart = _cmd.Text.Length; return; }
                    if (confirm && MessageBox.Show(this, "Send '" + command + "' to the device?",
                            "Confirm", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK) return;
                    Execute(command);
                };
                bar.Controls.Add(b);
            }
            host.Controls.Add(bar);
        }

        // ---- helpers ----
        private static void Fire(Action a) { ThreadPool.QueueUserWorkItem(_ => { try { a(); } catch { } }); }

        private void Log(string s) { AppendLog(s); }
        private void AppendLog(string s)
        {
            if (IsDisposed) return;
            if (InvokeRequired) { BeginInvoke((Action<string>)AppendLog, s); return; }
            _log.AppendText(s + Environment.NewLine);
        }
    }
}
