/*  Recorder.cs - session screen recording ("land video").
 *
 *  Client-side recording of the live remote-desktop stream.  If ffmpeg is
 *  available (next to the exe as ffmpeg.exe, or on PATH) frames are piped to it
 *  as raw BGRA and muxed to an .mp4 (H.264).  If not, each frame is written as a
 *  PNG into a timestamped folder, which the user can convert later - so
 *  recording always produces something ("still land video").  Server-side
 *  capture is a later slice; this is the client end.
 */
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

namespace UnoRemote
{
    public sealed class Recorder
    {
        private bool _recording;
        private int _w, _h;
        private Process _ff;
        private Stream _ffIn;
        private byte[] _row;            // reused BGRA extraction buffer
        private string _frameDir;
        private int _frameIx;

        public bool Recording { get { return _recording; } }
        public bool UsingFfmpeg { get; private set; }
        public string OutputPath { get; private set; }

        /// <summary>Locate ffmpeg: bundled beside the exe first, then PATH.</summary>
        private static string FindFfmpeg()
        {
            try
            {
                string beside = Path.Combine(
                    Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location),
                    "ffmpeg.exe");
                if (File.Exists(beside)) return beside;
            }
            catch { }
            // probe PATH
            try
            {
                var psi = new ProcessStartInfo("ffmpeg", "-version")
                { UseShellExecute = false, CreateNoWindow = true,
                  RedirectStandardOutput = true, RedirectStandardError = true };
                using (var p = Process.Start(psi)) { p.WaitForExit(3000); if (p.ExitCode == 0) return "ffmpeg"; }
            }
            catch { }
            return null;
        }

        /// <summary>Begin recording w x h frames at the given fps into outDir.
        /// Returns a human-readable description of where output is going.</summary>
        public string Start(int w, int h, double fps, string outDir)
        {
            if (_recording) return OutputPath;
            _w = w; _h = h; _frameIx = 0;
            Directory.CreateDirectory(outDir);
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");

            string ff = FindFfmpeg();
            if (ff != null)
            {
                OutputPath = Path.Combine(outDir, "unodos-" + stamp + ".mp4");
                string a = "-y -f rawvideo -pixel_format bgra -video_size " + w + "x" + h +
                           " -framerate " + fps.ToString(System.Globalization.CultureInfo.InvariantCulture) +
                           " -i - -an -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -c:v libx264 " +
                           "-pix_fmt yuv420p -movflags +faststart \"" + OutputPath + "\"";
                var psi = new ProcessStartInfo(ff, a)
                { UseShellExecute = false, CreateNoWindow = true,
                  RedirectStandardInput = true, RedirectStandardError = true };
                _ff = Process.Start(psi);
                _ff.BeginErrorReadLine();     // drain stderr so ffmpeg never blocks
                _ffIn = _ff.StandardInput.BaseStream;
                UsingFfmpeg = true;
            }
            else
            {
                _frameDir = Path.Combine(outDir, "unodos-" + stamp + "-frames");
                Directory.CreateDirectory(_frameDir);
                OutputPath = _frameDir;
                UsingFfmpeg = false;
            }
            _recording = true;
            return OutputPath;
        }

        /// <summary>Hand one decoded frame to the recorder. Frames whose size does
        /// not match the recording dimensions are skipped (e.g. a mid-session F9
        /// resolution change on the device).</summary>
        public void Frame(Bitmap bmp)
        {
            if (!_recording || bmp == null || bmp.Width != _w || bmp.Height != _h) return;
            if (UsingFfmpeg)
            {
                int rowBytes = _w * 4;
                if (_row == null || _row.Length != rowBytes) _row = new byte[rowBytes];
                var bd = bmp.LockBits(new Rectangle(0, 0, _w, _h), ImageLockMode.ReadOnly,
                                      PixelFormat.Format32bppArgb);
                try
                {
                    for (int y = 0; y < _h; y++)
                    {
                        Marshal.Copy(bd.Scan0 + y * bd.Stride, _row, 0, rowBytes);
                        _ffIn.Write(_row, 0, rowBytes);
                    }
                }
                catch { /* ffmpeg went away; Stop() will surface it */ }
                finally { bmp.UnlockBits(bd); }
            }
            else
            {
                try { bmp.Save(Path.Combine(_frameDir, "frame-" + (_frameIx++).ToString("D6") + ".png"),
                               ImageFormat.Png); }
                catch { }
            }
        }

        /// <summary>Stop recording and finalize. Returns the output path/folder.</summary>
        public string Stop()
        {
            if (!_recording) return OutputPath;
            _recording = false;
            if (UsingFfmpeg)
            {
                try { _ffIn.Flush(); _ffIn.Close(); } catch { }
                try { if (!_ff.WaitForExit(15000)) _ff.Kill(); } catch { }
                try { _ff.Dispose(); } catch { }
                _ff = null; _ffIn = null;
            }
            return OutputPath;
        }
    }
}
