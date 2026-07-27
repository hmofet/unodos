/*  Qoi.cs - a QOI image decoder, matched to the encoder in
 *  pc64/unoauto_screen.c (which is itself matched to the qoi spec /
 *  unomedia/um_qoi.c).  Decodes a `screen grab` frame into a 32bpp Bitmap.
 *
 *  The device sends channels in R,G,B,A order (fb[] pixel 0xAABBGGRR, i.e.
 *  R,G,B,A byte order in memory).  A .NET Format32bppArgb bitmap stores each
 *  pixel little-endian as B,G,R,A, so we swap R<->B when filling the buffer.
 */
using System;
using System.Drawing;
using System.Drawing.Imaging;

namespace UnoRemote
{
    public static class Qoi
    {
        private const byte OP_INDEX = 0x00; // 00xxxxxx
        private const byte OP_DIFF  = 0x40; // 01xxxxxx
        private const byte OP_LUMA  = 0x80; // 10xxxxxx
        private const byte OP_RUN   = 0xC0; // 11xxxxxx
        private const byte OP_RGB   = 0xFE;
        private const byte OP_RGBA  = 0xFF;

        /// <summary>Decode QOI bytes into a new 32bpp Bitmap. Throws on bad data.</summary>
        public static Bitmap Decode(byte[] data)
        {
            if (data == null || data.Length < 14 || data[0] != 'q' || data[1] != 'o' ||
                data[2] != 'i' || data[3] != 'f')
                throw new Exception("not a QOI stream");

            int w = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
            int h = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
            if (w <= 0 || h <= 0 || (long)w * h > 64L * 1024 * 1024)
                throw new Exception("bad QOI dimensions " + w + "x" + h);

            byte[] bgra = new byte[w * h * 4];       // B,G,R,A per pixel
            byte[] idxR = new byte[64], idxG = new byte[64], idxB = new byte[64], idxA = new byte[64];
            byte r = 0, g = 0, b = 0, a = 255;
            int p = 14, end = data.Length - 8, o = 0, total = w * h * 4;

            while (o < total)
            {
                if (p < end)
                {
                    byte op = data[p++];
                    if (op == OP_RGB) { r = data[p]; g = data[p + 1]; b = data[p + 2]; p += 3; }
                    else if (op == OP_RGBA) { r = data[p]; g = data[p + 1]; b = data[p + 2]; a = data[p + 3]; p += 4; }
                    else if ((op & 0xC0) == OP_INDEX)
                    {
                        int ix = op & 0x3F;
                        r = idxR[ix]; g = idxG[ix]; b = idxB[ix]; a = idxA[ix];
                    }
                    else if ((op & 0xC0) == OP_DIFF)
                    {
                        r = (byte)(r + ((op >> 4) & 3) - 2);
                        g = (byte)(g + ((op >> 2) & 3) - 2);
                        b = (byte)(b + (op & 3) - 2);
                    }
                    else if ((op & 0xC0) == OP_LUMA)
                    {
                        byte b2 = data[p++];
                        int vg = (op & 0x3F) - 32;
                        r = (byte)(r + vg - 8 + ((b2 >> 4) & 0x0F));
                        g = (byte)(g + vg);
                        b = (byte)(b + vg - 8 + (b2 & 0x0F));
                    }
                    else // OP_RUN: length stored with a -1 bias
                    {
                        int run = (op & 0x3F) + 1;
                        while (run-- > 0 && o < total)
                        {
                            bgra[o] = b; bgra[o + 1] = g; bgra[o + 2] = r; bgra[o + 3] = a; o += 4;
                        }
                        int hh0 = (r * 3 + g * 5 + b * 7 + a * 11) & 63;
                        idxR[hh0] = r; idxG[hh0] = g; idxB[hh0] = b; idxA[hh0] = a;
                        continue;
                    }
                    int hh = (r * 3 + g * 5 + b * 7 + a * 11) & 63;
                    idxR[hh] = r; idxG[hh] = g; idxB[hh] = b; idxA[hh] = a;
                }
                bgra[o] = b; bgra[o + 1] = g; bgra[o + 2] = r; bgra[o + 3] = a; o += 4;
            }

            var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
            var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly,
                                  PixelFormat.Format32bppArgb);
            try
            {
                // bd.Stride may exceed w*4; copy row by row to respect it.
                int rowBytes = w * 4;
                for (int y = 0; y < h; y++)
                    System.Runtime.InteropServices.Marshal.Copy(
                        bgra, y * rowBytes, bd.Scan0 + y * bd.Stride, rowBytes);
            }
            finally { bmp.UnlockBits(bd); }
            return bmp;
        }
    }
}
