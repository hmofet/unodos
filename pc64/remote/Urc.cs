/*  Urc.cs - the URC (unoautomate remote channel) client, in C#.
 *
 *  A faithful port of the load-bearing half of pc64/tools/unoauto_remote.py
 *  (UnoAutoLink): pc64 dials OUT to a listener here, so this LISTENS on a TCP
 *  port, does the HELLO handshake, speaks the newline-delimited URC line
 *  protocol, and correlates CMD/RSP by id.  The flasher shares no networking
 *  code, so this is a fresh implementation - but the WIRE behaviour is kept
 *  identical to the Python reference so both drive the same device.
 *
 *  Protocol (URC), symmetric both directions:
 *      HELLO <name> <api>          handshake
 *      LOG   <chan> <text>         a log line from pc64
 *      MSG   <text>                free-form message, either direction
 *      CMD   <id> <verb> <args>    a command request, either direction
 *      RSP   <id> <ok|err|end> ..  response lines, terminated by `end`
 *      BYE                         graceful close
 *
 *  Plaintext, LAN-only by intent - do not expose the port to untrusted nets.
 */
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace UnoRemote
{
    /// <summary>The dev-PC end of the URC link. Listen(), then Command(...).</summary>
    public sealed class UrcLink : IDisposable
    {
        // ---- pending-command correlation ----
        private sealed class Pending
        {
            public readonly List<string> Lines = new List<string>();
            public readonly ManualResetEventSlim Done = new ManualResetEventSlim(false);
            public bool Err;
        }

        private readonly object _lock = new object();
        private readonly Dictionary<string, Pending> _pending = new Dictionary<string, Pending>();
        private int _idCounter;

        private TcpListener _listener;
        private TcpClient _client;
        private NetworkStream _stream;
        private Thread _acceptThread;
        private volatile bool _stop;

        // ---- events (raised on the reader thread; marshal to UI yourself) ----
        public event Action<string, string> OnLog;    // (chan, text)
        public event Action<string> OnMessage;         // (text)
        public event Action OnConnected;
        public event Action OnDisconnected;

        public bool Connected { get { return _stream != null; } }

        // ---- lifecycle -------------------------------------------------------
        public void Listen(string host, int port)
        {
            IPAddress ip = (host == "0.0.0.0" || string.IsNullOrEmpty(host))
                           ? IPAddress.Any : IPAddress.Parse(host);
            _listener = new TcpListener(ip, port);
            _listener.Server.SetSocketOption(SocketOptionLevel.Socket,
                                             SocketOptionName.ReuseAddress, true);
            _listener.Start();
            _acceptThread = new Thread(AcceptLoop) { IsBackground = true, Name = "urc-accept" };
            _acceptThread.Start();
        }

        public void Dispose()
        {
            _stop = true;
            try { if (_listener != null) _listener.Stop(); } catch { }
            CloseClient();
        }

        private void CloseClient()
        {
            var c = _client;
            try { if (c != null) c.Close(); } catch { }
            _client = null;
            _stream = null;
            // wake any blocked Command() callers
            lock (_lock)
            {
                foreach (var kv in _pending) { kv.Value.Err = true; kv.Value.Done.Set(); }
                _pending.Clear();
            }
        }

        private void AcceptLoop()
        {
            while (!_stop)
            {
                TcpClient c;
                try { c = _listener.AcceptTcpClient(); }
                catch { break; }
                _client = c;
                c.NoDelay = true;
                _stream = c.GetStream();
                Send("HELLO", "host 1");
                var h = OnConnected; if (h != null) h();
                try { Reader(_stream); } catch { }
                CloseClient();
                var d = OnDisconnected; if (d != null) d();
            }
        }

        // ---- send ------------------------------------------------------------
        private void Raw(string line)
        {
            var s = _stream;
            if (s == null) return;
            byte[] b = Encoding.UTF8.GetBytes(line + "\n");
            try { lock (s) { s.Write(b, 0, b.Length); s.Flush(); } }
            catch { }
        }

        public void Send(string type, string text)
        {
            Raw(string.IsNullOrEmpty(text) ? type : type + " " + text);
        }

        public void Message(string text) { Send("MSG", text); }

        /// <summary>Send CMD, block for the response, return its `ok` lines.
        /// Throws TimeoutException or an Exception carrying the `err` text.</summary>
        public List<string> Command(string verb, params object[] args)
        {
            return Command(5000, verb, args);
        }

        public List<string> Command(int timeoutMs, string verb, params object[] args)
        {
            string id = Interlocked.Increment(ref _idCounter).ToString();
            var rec = new Pending();
            lock (_lock) _pending[id] = rec;

            var sb = new StringBuilder();
            sb.Append("CMD ").Append(id).Append(' ').Append(verb);
            if (args != null)
                foreach (var a in args) { sb.Append(' '); sb.Append(Convert.ToString(a,
                                          System.Globalization.CultureInfo.InvariantCulture)); }
            Raw(sb.ToString());

            bool ok = rec.Done.Wait(timeoutMs);
            lock (_lock) _pending.Remove(id);
            if (!ok) throw new TimeoutException("no response to '" + verb + "'");
            if (rec.Err) throw new Exception(rec.Lines.Count > 0 ? string.Join("\n", rec.Lines) : "error");
            return rec.Lines;
        }

        // ---- receive ---------------------------------------------------------
        private void Reader(NetworkStream s)
        {
            var buf = new List<byte>(8192);
            byte[] chunk = new byte[8192];
            while (!_stop)
            {
                int n;
                try { n = s.Read(chunk, 0, chunk.Length); }
                catch { break; }
                if (n <= 0) break;
                for (int i = 0; i < n; i++)
                {
                    if (chunk[i] == (byte)'\n')
                    {
                        string line = Encoding.UTF8.GetString(buf.ToArray()).TrimEnd('\r');
                        buf.Clear();
                        Dispatch(line);
                    }
                    else buf.Add(chunk[i]);
                }
            }
        }

        private static void Split2(string s, out string a, out string rest)
        {
            int i = s.IndexOf(' ');
            if (i < 0) { a = s; rest = ""; } else { a = s.Substring(0, i); rest = s.Substring(i + 1); }
        }

        private void Dispatch(string line)
        {
            if (line.Length == 0) return;
            string type, rest; Split2(line, out type, out rest);
            switch (type)
            {
                case "LOG":
                {
                    string chan, text; Split2(rest, out chan, out text);
                    var h = OnLog; if (h != null) h(chan, text);
                    break;
                }
                case "MSG":
                {
                    var h = OnMessage; if (h != null) h(rest);
                    break;
                }
                case "RSP":
                {
                    string rid, tail; Split2(rest, out rid, out tail);
                    string status, text; Split2(tail, out status, out text);
                    OnRsp(rid, status, text);
                    break;
                }
                case "HELLO":
                case "BYE":
                default:
                    break;   // CMD (pc64->host) is a follow-up-slice feature
            }
        }

        private void OnRsp(string rid, string status, string text)
        {
            Pending rec;
            lock (_lock) { _pending.TryGetValue(rid, out rec); }
            if (rec == null) return;
            if (status == "end") rec.Done.Set();
            else if (status == "err") { rec.Err = true; if (text.Length > 0) rec.Lines.Add(text); rec.Done.Set(); }
            else rec.Lines.Add(text);
        }

        // ---- verb convenience wrappers (mirror unoauto_remote.py) -------------
        public void Key(int scan, int uni, int ctrl) { Command("key", scan, uni, ctrl); }
        public void Pointer(int x, int y, int btn)   { Command("pointer", x, y, btn); }
        public void Launch(int n)                    { Command("launch", n); }
        public void CloseTop()                       { Command("close"); }
        public void Reboot()                         { Command("reboot"); }
        public void Poweroff()                       { Command("poweroff"); }

        /// <summary>`screen info` -> (width, height).</summary>
        public void ScreenInfo(out int w, out int h)
        {
            var r = Command("screen", "info");
            w = h = 0;
            if (r.Count > 0)
            {
                var p = r[0].Split(' ');
                if (p.Length >= 2) { int.TryParse(p[0], out w); int.TryParse(p[1], out h); }
            }
        }

        private const int ScreenReadLen = 2880;   // matches SCREEN_READ_MAX on the device

        /// <summary>`screen grab [scale]` stages a frame on the device and returns
        /// its `frame &lt;w&gt; &lt;h&gt; qoi &lt;n&gt;` header; the QOI payload is then
        /// pulled in bounded `screen read &lt;off&gt; &lt;len&gt;` slices (the readsec idiom -
        /// a whole frame is far too big for one URC response). Returns the raw
        /// (still QOI-encoded) frame bytes plus its dimensions.</summary>
        public byte[] ScreenGrab(int scale, out int w, out int h)
        {
            w = h = 0; int n = 0;
            var r = Command(8000, "screen", "grab", scale);
            if (r.Count == 0) throw new Exception("empty screen reply");
            var hdr = r[0].Split(' ');   // frame W H qoi N
            if (hdr.Length >= 5 && hdr[0] == "frame")
            { int.TryParse(hdr[1], out w); int.TryParse(hdr[2], out h); int.TryParse(hdr[4], out n); }
            if (n <= 0) throw new Exception("bad frame header: " + r[0]);

            byte[] buf = new byte[n];
            int off = 0;
            while (off < n)
            {
                var rd = Command(8000, "screen", "read", off.ToString("x"), ScreenReadLen);
                var b64 = new StringBuilder();
                foreach (var l in rd) b64.Append(l);
                byte[] part = Convert.FromBase64String(b64.ToString());
                if (part.Length == 0) throw new Exception("screen read returned nothing at off " + off);
                int copy = Math.Min(part.Length, n - off);
                Array.Copy(part, 0, buf, off, copy);
                off += copy;
            }
            return buf;
        }
    }
}
