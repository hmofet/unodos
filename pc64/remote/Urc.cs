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

        // ---- zero-config discovery responder (netdisc, UDP :5400) ----
        private Socket _disc;
        private Thread _discThread;
        private int _urcPort;
        private const int DiscPort = 5400;

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
            StartDiscovery(port);   // answer UNODISC probes so `discover` devices find us
        }

        public void Dispose()
        {
            _stop = true;
            try { if (_listener != null) _listener.Stop(); } catch { }
            try { if (_disc != null) _disc.Close(); } catch { }
            _disc = null;
            CloseClient();
        }

        /// <summary>Dial INTO a box running in URC listen mode (`listen=<port>` in
        /// its DEBUG.CFG) - the reverse of Listen(): here WE open the connection.
        /// The URC protocol is identical once connected. If this link was
        /// listening, that is stopped first. Throws on connect failure.</summary>
        public void Connect(string ip, int port)
        {
            _stop = false;
            try { if (_listener != null) { _listener.Stop(); _listener = null; } } catch { }
            CloseClient();
            var c = new TcpClient();
            c.Connect(ip, port);       // throws if the box isn't listening / unreachable
            c.NoDelay = true;
            _client = c;
            _stream = c.GetStream();
            Send("HELLO", "host 1");
            var h = OnConnected; if (h != null) h();
            _acceptThread = new Thread(() =>
            {
                try { Reader(_stream); } catch { }
                CloseClient();
                var d = OnDisconnected; if (d != null) d();
            }) { IsBackground = true, Name = "urc-dialin" };
            _acceptThread.Start();
        }

        // ---- zero-config discovery (netdisc) ---------------------------------
        // A device booted with `discover` in DEBUG.CFG (instead of a static
        // `remote=<ip>:<port>`) broadcasts a UNODISC PROBE on the LAN; we answer
        // with an OFFER carrying THIS listener's ip:port, and the device dials us
        // with no address configured. Protocol: UDP :5400, one ASCII datagram
        // (see pc64/netdisc.h). Best-effort: if the port is taken, TCP listen
        // still works - you just have to use a static remote= address.
        public event Action<string> OnDiscovery;   // (human-readable note), UI thread marshals itself

        private void StartDiscovery(int urcPort)
        {
            _urcPort = urcPort;
            try
            {
                _disc = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                _disc.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                _disc.EnableBroadcast = true;
                _disc.Bind(new IPEndPoint(IPAddress.Any, DiscPort));
            }
            catch { _disc = null; return; }   // discovery is optional; don't fail Listen
            _discThread = new Thread(DiscoveryLoop) { IsBackground = true, Name = "urc-disc" };
            _discThread.Start();
        }

        private void DiscoveryLoop()
        {
            var buf = new byte[512];
            var sep = new[] { ' ', '\r', '\n', '\t' };
            while (!_stop && _disc != null)
            {
                int n; EndPoint any = new IPEndPoint(IPAddress.Any, 0);
                try { n = _disc.ReceiveFrom(buf, ref any); }
                catch { break; }
                if (n <= 0) continue;
                string msg;
                try { msg = Encoding.ASCII.GetString(buf, 0, n); } catch { continue; }
                var t = msg.Split(sep, StringSplitOptions.RemoveEmptyEntries);
                // UNODISC 1 PROBE <role> <name> <api>  -> reply with our OFFER
                if (t.Length >= 3 && t[0] == "UNODISC" && t[2] == "PROBE")
                {
                    var src = (IPEndPoint)any;
                    string localIp = LocalIpToward(src.Address);
                    string offer = "UNODISC 1 OFFER host " + SafeHostName() + " 1 " + localIp + " " + _urcPort;
                    try { _disc.SendTo(Encoding.ASCII.GetBytes(offer), src); } catch { }
                    var h = OnDiscovery;
                    if (h != null)
                    {
                        string who = (t.Length >= 5 ? t[4] : t.Length >= 4 ? t[3] : "a device");
                        h("discovery: offered " + localIp + ":" + _urcPort + " to " + who + " (" + src.Address + ")");
                    }
                }
            }
        }

        // netdisc parses space-separated tokens, so the advertised name must be a
        // single token.
        private static string SafeHostName()
        {
            string n = Environment.MachineName;
            var sb = new StringBuilder();
            foreach (char c in n) sb.Append(char.IsWhiteSpace(c) ? '-' : c);
            return sb.Length > 0 ? sb.ToString() : "host";
        }

        // The local IP the device can reach us on: pick the egress interface
        // toward the prober (a connect on a UDP socket sends nothing, it just
        // resolves the route). Falls back to loopback.
        private static string LocalIpToward(IPAddress dst)
        {
            try
            {
                using (var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
                {
                    s.Connect(new IPEndPoint(dst, 9));
                    return ((IPEndPoint)s.LocalEndPoint).Address.ToString();
                }
            }
            catch { return "127.0.0.1"; }
        }

        // ---- scan: find boxes in LISTEN mode you can dial into ---------------
        /// <summary>A UnoDOS box discovered on the LAN that is in listen mode
        /// (advertises a URC port you can dial into).</summary>
        public sealed class DiscoveredBox
        {
            public string Name;
            public string Ip;
            public int Port;
            public override string ToString() { return Name + "   " + Ip + ":" + Port; }
        }

        /// <summary>Broadcast a UNODISC PROBE and collect the boxes that answer.
        /// Only boxes in LISTEN mode advertise a non-zero port (something to dial
        /// into); dial-out (`discover`) boxes advertise port 0 and are skipped.
        /// Blocks up to ~timeoutMs. Uses its own ephemeral socket, so it coexists
        /// with the discovery responder on :5400.</summary>
        public List<DiscoveredBox> Scan(int timeoutMs)
        {
            var found = new Dictionary<string, DiscoveredBox>();
            var sep = new[] { ' ', '\r', '\n', '\t' };
            Socket s = null;
            try
            {
                s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                s.EnableBroadcast = true;
                s.Bind(new IPEndPoint(IPAddress.Any, 0));
                s.ReceiveTimeout = 400;
                byte[] probe = Encoding.ASCII.GetBytes("UNODISC 1 PROBE host " + SafeHostName() + " 1");
                try { s.SendTo(probe, new IPEndPoint(IPAddress.Broadcast, DiscPort)); } catch { }
                int end = Environment.TickCount + timeoutMs;
                var buf = new byte[512];
                while (Environment.TickCount < end)
                {
                    EndPoint any = new IPEndPoint(IPAddress.Any, 0);
                    int n;
                    try { n = s.ReceiveFrom(buf, ref any); }
                    catch (SocketException) { continue; }   // recv timeout - keep waiting
                    catch { break; }
                    if (n <= 0) continue;
                    var t = Encoding.ASCII.GetString(buf, 0, n).Split(sep, StringSplitOptions.RemoveEmptyEntries);
                    // UNODISC 1 OFFER pc64 <name> <api> <ip> <port>
                    if (t.Length >= 8 && t[0] == "UNODISC" && t[2] == "OFFER" && t[3] == "pc64")
                    {
                        int port;
                        if (!int.TryParse(t[7], out port) || port <= 0) continue;   // 0 = not listening
                        found[t[6] + ":" + t[7]] = new DiscoveredBox { Name = t[4], Ip = t[6], Port = port };
                    }
                }
            }
            catch { }
            finally { try { if (s != null) s.Close(); } catch { } }
            return new List<DiscoveredBox>(found.Values);
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

        /// <summary>Pull `n` staged bytes with bounded `screen read &lt;off&gt; &lt;len&gt;`
        /// slices (the readsec idiom - a whole frame is far too big for one URC
        /// response, which caps at the device's 8 KB TX buffer).</summary>
        private byte[] PullStaged(int n)
        {
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

        /// <summary>`screen grab [scale]` stages a full frame on the device and
        /// returns the raw (still QOI-encoded) frame bytes plus its dimensions.
        /// This is the whole-frame path; prefer <see cref="ScreenGrabDelta"/> for
        /// the live view (it sends only changed tiles).</summary>
        public byte[] ScreenGrab(int scale, out int w, out int h)
        {
            w = h = 0; int n = 0;
            var r = Command(8000, "screen", "grab", scale);
            if (r.Count == 0) throw new Exception("empty screen reply");
            var hdr = r[0].Split(' ');   // frame W H qoi N
            if (hdr.Length >= 5 && hdr[0] == "frame")
            { int.TryParse(hdr[1], out w); int.TryParse(hdr[2], out h); int.TryParse(hdr[4], out n); }
            if (n <= 0) throw new Exception("bad frame header: " + r[0]);
            return PullStaged(n);
        }

        /// <summary>One live-view update: a full keyframe or a set of changed
        /// tiles. See <see cref="UrcLink.ScreenGrabDelta"/>.</summary>
        public sealed class ScreenUpdate
        {
            public bool Keyframe;     // true: Qoi is a whole frame; false: a delta
            public int W, H;          // emitted frame dimensions
            public int Cols, Tw, Th;  // delta tile grid (delta only)
            public int Nch;           // number of changed tiles (0 = nothing changed)
            public int[] TileIdx;     // row-major changed tile indices (delta only)
            public byte[] Qoi;        // keyframe: the frame; delta: the tile strip (null if Nch==0)
        }

        /// <summary>`screen grab delta [scale]`: the device compares against the
        /// previous grab and returns either a full `frame` keyframe (first grab,
        /// after a scale change, or when too much changed) or a `delta` of just
        /// the changed tiles. The payload (keyframe QOI, or the delta's
        /// [strip][manifest] blob) is pulled with the same bounded reader.</summary>
        public ScreenUpdate ScreenGrabDelta(int scale)
        {
            var r = Command(8000, "screen", "grab", "delta", scale);
            if (r.Count == 0) throw new Exception("empty screen reply");
            var hdr = r[0].Split(' ');
            var u = new ScreenUpdate();

            if (hdr[0] == "frame")            // frame W H qoi N
            {
                int n = 0;
                if (hdr.Length >= 5) { int.TryParse(hdr[1], out u.W); int.TryParse(hdr[2], out u.H); int.TryParse(hdr[4], out n); }
                if (n <= 0) throw new Exception("bad frame header: " + r[0]);
                u.Keyframe = true;
                u.Qoi = PullStaged(n);
                return u;
            }
            if (hdr[0] == "delta")            // delta ew eh cols tw th nch strip total
            {
                if (hdr.Length < 9) throw new Exception("bad delta header: " + r[0]);
                int strip = 0, total = 0;
                int.TryParse(hdr[1], out u.W);   int.TryParse(hdr[2], out u.H);
                int.TryParse(hdr[3], out u.Cols); int.TryParse(hdr[4], out u.Tw);
                int.TryParse(hdr[5], out u.Th);  int.TryParse(hdr[6], out u.Nch);
                int.TryParse(hdr[7], out strip); int.TryParse(hdr[8], out total);
                u.Keyframe = false;
                if (u.Nch <= 0 || total <= 0) { u.Nch = 0; return u; }   // static frame

                byte[] blob = PullStaged(total);
                u.Qoi = new byte[strip];
                Array.Copy(blob, 0, u.Qoi, 0, strip);
                u.TileIdx = new int[u.Nch];
                for (int i = 0; i < u.Nch; i++)
                    u.TileIdx[i] = blob[strip + i * 2] | (blob[strip + i * 2 + 1] << 8);   // u16 LE
                return u;
            }
            throw new Exception("unknown screen reply: " + r[0]);
        }

        // ---- server-side session capture -------------------------------------
        private static Dictionary<string, int> ParseStat(List<string> r)
        {
            var d = new Dictionary<string, int>();
            if (r.Count > 0)
            {
                var t = r[0].Split(' ');
                for (int i = 0; i + 1 < t.Length; i += 2)
                { int v; if (int.TryParse(t[i + 1], out v)) d[t[i]] = v; }
            }
            return d;
        }

        /// <summary>`screen record start [scale] [fps]` -> status dict.</summary>
        public Dictionary<string, int> ScreenRecordStart(int scale, int fps)
        { return ParseStat(Command(10000, "screen", "record", "start", scale, fps)); }

        /// <summary>`screen record stop` -> final status dict (ring kept for reading).</summary>
        public Dictionary<string, int> ScreenRecordStop()
        { return ParseStat(Command(10000, "screen", "record", "stop")); }

        /// <summary>`screen record status` -> live status dict.</summary>
        public Dictionary<string, int> ScreenRecordStatus()
        { return ParseStat(Command(5000, "screen", "record", "status")); }

        /// <summary>Pull the whole recorded ring (`nbytes` from the stop/status
        /// stat) with bounded `screen record read` slices.</summary>
        public byte[] ScreenRecordReadAll(int nbytes)
        {
            byte[] buf = new byte[nbytes];
            int off = 0;
            while (off < nbytes)
            {
                var rd = Command(20000, "screen", "record", "read", off.ToString("x"), ScreenReadLen);
                var sb = new StringBuilder();
                foreach (var l in rd) sb.Append(l);
                byte[] part = Convert.FromBase64String(sb.ToString());
                if (part.Length == 0) break;
                int copy = Math.Min(part.Length, nbytes - off);
                Array.Copy(part, 0, buf, off, copy);
                off += copy;
            }
            return buf;
        }
    }
}
