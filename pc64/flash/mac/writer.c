/*  uno-writer  —  the privileged raw-disk writer for the UnoDOS/pc64 flasher.
 *
 *  Run as root (via Authorization Services from the GUI) with:
 *      uno-writer <image.img.gz> <raw-device>   e.g.  uno-writer u.img.gz /dev/rdisk4
 *
 *  Streams the gzip-decompressed image straight to the raw device in 1 MiB,
 *  sector-aligned blocks and prints machine-readable progress to stdout that the
 *  GUI parses:
 *      P <bytesDone> <bytesTotal>     after every block
 *      DONE                           on success
 *      ERR <message>                  on any failure
 *
 *  Compiled by build-app.sh:  cc -O2 writer.c -lz -o .../Resources/uno-writer
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>

#define BLK (1024 * 1024)   /* 1 MiB, a multiple of 512 */

int main(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: uno-writer <image.gz> <device>\n"); return 2; }
    const char *gzpath = argv[1];
    const char *device = argv[2];

    /* Uncompressed size lives in the gzip footer (ISIZE, last 4 bytes, mod 2^32) —
     * the same trick the Windows flasher uses to size the progress bar. */
    long total = 0;
    FILE *fp = fopen(gzpath, "rb");
    if (!fp) { printf("ERR cannot open image: %s\n", strerror(errno)); return 3; }
    if (fseek(fp, -4, SEEK_END) == 0) {
        unsigned char b[4];
        if (fread(b, 1, 4, fp) == 4)
            total = (long)b[0] | ((long)b[1] << 8) | ((long)b[2] << 16) | ((long)b[3] << 24);
    }
    fclose(fp);

    gzFile gz = gzopen(gzpath, "rb");
    if (!gz) { printf("ERR cannot open image (gzopen)\n"); return 3; }

    int fd = open(device, O_WRONLY);
    if (fd < 0) {
        printf("ERR cannot open %s: %s\n", device, strerror(errno));
        gzclose(gz);
        return 4;
    }

    unsigned char *buf = malloc(BLK);
    if (!buf) { printf("ERR out of memory\n"); close(fd); gzclose(gz); return 6; }

    long done = 0;
    int n;
    while ((n = gzread(gz, buf, BLK)) > 0) {
        int wlen = n;
        if (n % 512) {                       /* pad the final short block to a sector */
            int pad = 512 - (n % 512);
            memset(buf + n, 0, pad);
            wlen = n + pad;
        }
        int off = 0;
        while (off < wlen) {
            ssize_t w = write(fd, buf + off, wlen - off);
            if (w < 0) {
                printf("ERR write failed: %s\n", strerror(errno));
                free(buf); close(fd); gzclose(gz);
                return 5;
            }
            off += (int)w;
        }
        done += n;
        printf("P %ld %ld\n", done, total > 0 ? total : done);
        fflush(stdout);
    }
    if (n < 0) {                              /* gzread error mid-stream */
        printf("ERR decompression failed\n");
        free(buf); close(fd); gzclose(gz);
        return 7;
    }

    fsync(fd);
    close(fd);
    gzclose(gz);
    free(buf);
    printf("DONE\n");
    fflush(stdout);
    return 0;
}
