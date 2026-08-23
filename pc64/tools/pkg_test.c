/* Host gate for pc64_pkg.c: read a REAL .apk and install it into a REAL
 * directory, natively, in a second.  See tools/pkg_test.sh.
 *
 * WHY A HOST GATE AND NOT A QEMU RUN.  Everything interesting in pc64_pkg.c is
 * pure reading: a zip central directory, a binary-XML string pool, an
 * attribute walk.  All three fail on the contents of a file rather than on
 * anything about the machine, and a 138 MB APK from Mozilla exercises corners
 * no hand-written fixture would think of - thousands of entries, a deflated
 * manifest, a UTF-8 string pool, and an `android:label` that is a resource
 * reference rather than a string.  A QEMU round trip to learn any of that
 * would cost minutes per attempt and tell you no more.
 *
 * The volume layer is stood in for by the host filesystem: volume 1 is a
 * directory, and a UnoDOS path (backslashes, 8.3) maps onto it.
 */
#define _POSIX_C_SOURCE 200809L
#include "../pc64_pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL %s\n", what); fails++; } \
                            else printf("  ok   %s\n", what); } while (0)

/* ---- the stand-in volume layer ------------------------------------------ */

static char g_root[512];         /* volume 1 lives here                      */
static char g_apk[512];          /* the package under test, volume 2         */

/* A UnoDOS path onto the host: backslashes become slashes, under g_root. */
/* Volume 2 holds exactly ONE file, the package, and answers for nothing else.
 * A stub that mapped every path on it to the APK made the runtime probe read
 * the staged Android image as present, at 138 MB - which is a test that lies
 * in the direction of passing, the worst kind there is. */
static void hostpath(int vol, const char *p, char *out, size_t max)
{
    size_t i = 0, j;
    if (vol == 2) {
        snprintf(out, max, "%s",
                 strcmp(p, "app.apk") == 0 ? g_apk : "/nonexistent");
        return;
    }
    j = (size_t)snprintf(out, max, "%s/", g_root);
    while (p[i] && j + 1 < max) { out[j++] = (p[i] == '\\') ? '/' : p[i]; i++; }
    out[j] = 0;
}

long uno_fs_size(int vol, const char *name)
{
    char hp[1024];
    struct stat st;
    hostpath(vol, name, hp, sizeof hp);
    if (stat(hp, &st) != 0) return -1;
    return (long)st.st_size;
}

long uno_fs_read_at(int vol, const char *name, long off,
                    unsigned char *buf, long max)
{
    char hp[1024];
    FILE *f;
    long n;
    hostpath(vol, name, hp, sizeof hp);
    f = fopen(hp, "rb");
    if (!f) return -1;
    if (fseek(f, off, SEEK_SET) != 0) { fclose(f); return 0; }
    n = (long)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

long uno_fs_read(int vol, const char *name, unsigned char *buf, long max)
{ return uno_fs_read_at(vol, name, 0, buf, max); }

int uno_fs_write(int vol, const char *name, const unsigned char *b, long n)
{
    char hp[1024];
    FILE *f;
    hostpath(vol, name, hp, sizeof hp);
    f = fopen(hp, "wb");
    if (!f) return 0;
    fwrite(b, 1, (size_t)n, f);
    fclose(f);
    return 1;
}

int uno_fat_write(int vol, const char *p, const unsigned char *b, long n)
{ return uno_fs_write(1, p, b, n); (void)vol; }

int uno_fat_delete(int vol, const char *p)
{
    char hp[1024];
    (void)vol;
    hostpath(1, p, hp, sizeof hp);
    return remove(hp) == 0;
}

int uno_fs_volumes(void)          { return 3; }
int uno_fs_kind(int vol)          { return vol == 1 ? 1 : 2; }
int uno_fs_fat_index(int vol)     { return vol; }
int uno_fs_writable(int vol)      { return vol == 1; }
int uno_fs_pref_vol(void)         { return 1; }
int uno_fs_isdir(int vol, const char *p)
{
    char hp[1024];
    struct stat st;
    hostpath(vol, p, hp, sizeof hp);
    return stat(hp, &st) == 0 && S_ISDIR(st.st_mode);
}
int uno_fs_mkdir(int vol, const char *p)
{
    char hp[1024];
    hostpath(vol, p, hp, sizeof hp);
    return mkdir(hp, 0755) == 0 || uno_fs_isdir(vol, p);
}

/* ---- the rest of the kernel, stubbed ------------------------------------ */

int  uno_mod_present(const char *f)  { return uno_fs_size(1, f) > 0; }
void pc64_shell_apps_rescan(void)    { }

/* The runtime probe's inputs.  Defaults describe a machine that COULD host an
 * appliance, so the interesting refusals can be dialled in one at a time. */
int      g_elig = 1;
unsigned g_carve = 1536;
int uno_vmm_eligible(unsigned *b)    { if (b) *b = 0; return g_elig; }
const char *uno_vmm_blocker_str(unsigned m) { (void)m; return "virtualization is off in firmware setup"; }
unsigned uno_vmm_carve_mb(void)      { return g_carve; }

/* ---- reading back what the installer wrote ------------------------------ */

/* Find and print the descriptor block of a .UNO, exactly as the shell's
 * uno_mod_desc_read would locate it: by its own magic. */
static int desc_of(const char *path, char *body, int max)
{
    static unsigned char img[2 << 20];
    long n = uno_fs_read_at(1, path, 0, img, (long)sizeof img), i;
    if (n <= 0) return 0;
    for (i = 0; i + 8 <= n; i++)
        if (!memcmp(img + i, "UAPP", 4)) {
            int len = img[i + 6] | (img[i + 7] << 8), k;
            if (len <= 8 || i + len > n) return 0;
            for (k = 0; k < len - 8 && k < max - 1; k++) body[k] = (char)img[i + 8 + k];
            body[k] = 0;
            return len;
        }
    return 0;
}

static int blob_of(const char *path, char *tgt, int tmax, char *nm, int nmax)
{
    static unsigned char img[2 << 20];
    long n = uno_fs_read_at(1, path, 0, img, (long)sizeof img), i;
    if (n <= 0) return 0;
    for (i = 0; i + 16 <= n; i++)
        if (!memcmp(img + i, "UNOPKG-TARGET-v1", 16)) {
            const char *p = (const char *)img + i + 16;
            snprintf(tgt, (size_t)tmax, "%s", p);
            snprintf(nm, (size_t)nmax, "%s", p + strlen(p) + 1);
            return 1;
        }
    return 0;
}

static int has(const char *hay, const char *needle)
{ return strstr(hay, needle) != NULL; }

/* THE CHECK THIS GATE WAS MISSING, and the reason it is worth a comment.
 *
 * A .UNO is SEALED: mod_instantiate recomputes a CRC-32 over everything after
 * the 48-byte header and refuses the image if it disagrees.  Rewriting fields
 * inside a module without re-sealing it therefore produces an app that
 * installs, registers, and shows the right name - and whose window says "the
 * loader refused the image".  This gate passed the whole time, because a host
 * gate runs no loader; it took a QEMU boot to see it.
 *
 * So the gate now checks the one invariant the loader checks.  Written out
 * here rather than shared with pc64_pkg.c on purpose: a test that reuses the
 * implementation's own arithmetic cannot catch the implementation getting the
 * arithmetic wrong. */
static int seal_ok(const char *path)
{
    static unsigned char img[2 << 20];
    long n = uno_fs_read_at(1, path, 0, img, (long)sizeof img), i;
    unsigned long c = 0xFFFFFFFFul, want;
    int k;
    if (n <= 48) return 0;
    if (!(img[0] == 'U' && img[1] == 'N' && img[2] == 'O' && img[3] == '1'))
        return 0;
    want = (unsigned long)img[40] | ((unsigned long)img[41] << 8)
         | ((unsigned long)img[42] << 16) | ((unsigned long)img[43] << 24);
    for (i = 48; i < n; i++) {
        c ^= img[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320ul & (0ul - (c & 1ul)));
    }
    return (((~c) & 0xFFFFFFFFul) == want);
}

int main(int argc, char **argv)
{
    uno_pkg_info info;
    int r;

    if (argc != 4) {
        fprintf(stderr, "usage: pkg_test <root-dir> <app.apk> <FSHIM.UNO>\n");
        return 2;
    }
    snprintf(g_root, sizeof g_root, "%s", argv[1]);
    snprintf(g_apk,  sizeof g_apk,  "%s", argv[2]);
    { /* stage the template where find_template() looks for it */
        static unsigned char t[2 << 20];
        FILE *f = fopen(argv[3], "rb");
        long n;
        char hp[1024];
        if (!f) { fprintf(stderr, "no template %s\n", argv[3]); return 2; }
        n = (long)fread(t, 1, sizeof t, f);
        fclose(f);
        uno_fs_mkdir(1, "PKG");
        uno_fs_mkdir(1, "APPS");
        uno_fs_write(1, "PKG\\FSHIM.UNO", t, n);
        hostpath(1, "PKG\\FSHIM.UNO", hp, sizeof hp);
        printf("template: %ld bytes\n", n);
    }

    printf("\n[1] probe a real APK\n");
    r = uno_pkg_probe(2, "app.apk", &info);
    printf("  kind=%d id='%s' name='%s'\n  target='%s'\n  version='%s' arch='%s' size=%ld\n",
           info.kind, info.id, info.name, info.target, info.version,
           info.arch, info.size);
    CHECK(r == 1, "probe accepts the package");
    CHECK(info.kind == UNO_PKG_APK, "kind is APK");
    CHECK(info.id[0] != 0, "an id was derived");
    CHECK(has(info.target, "android:"), "target names the android runtime");
    CHECK(has(info.target, "."), "target carries a package name");
    CHECK(has(info.target, "/"), "a launcher activity was found");
    CHECK(info.arch_ok == 1, "x86_64 code is present");
    CHECK(strcmp(info.arch, "x86_64") == 0, "arch is reported as x86_64");

    printf("\n[2] a non-package is refused, not guessed at\n");
    CHECK(uno_pkg_probe(1, "PKG\\FSHIM.UNO", &info) == -1,
          "a .UNO is not mistaken for a package");
    { uno_pkg_info i2;
      CHECK(uno_pkg_probe(1, "NOSUCH.APK", &i2) == -1, "a missing file is refused"); }

    printf("\n[3] install it\n");
    r = uno_pkg_probe(2, "app.apk", &info);
    {
        char err[100];
        err[0] = 0;
        r = uno_pkg_install(2, "app.apk", &info, 0, err, sizeof err);
        if (!r) printf("  install said: %s\n", err);
        CHECK(r == 1, "install reports success");
    }

    printf("\n[4] what landed on the volume\n");
    {
        char body[1024], tgt[200], nm[64];
        char shim[64], side[96];
        int i, o = 0;
        /* the 8.3 name the installer derives from the id */
        for (i = 0; info.id[i] && o < 8; i++) {
            char c = info.id[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) shim[o++] = c;
        }
        shim[o] = 0;
        snprintf(side, sizeof side, "PKG\\%s.PKG", shim);
        snprintf(shim + o, sizeof shim - (size_t)o, ".UNO");
        {
            char full[80];
            snprintf(full, sizeof full, "APPS\\%s", shim);
            printf("  shim:    %s (%ld bytes)\n", full, uno_fs_size(1, full));
            printf("  sidecar: %s (%ld bytes)\n", side, uno_fs_size(1, side));
            CHECK(uno_fs_size(1, full) > 1000, "the shim was written");
            CHECK(uno_fs_size(1, side) > 20, "the record was written");
            CHECK(seal_ok("PKG\\FSHIM.UNO"), "the template itself is sealed");
            CHECK(seal_ok(full),
                  "THE INSTALLED SHIM IS RE-SEALED (the loader checks this)");

            CHECK(desc_of(full, body, sizeof body) > 8, "the shim has a descriptor");
            printf("  --- descriptor ---\n%s  ------------------\n", body);
            CHECK(has(body, "id: "), "descriptor carries an id");
            CHECK(has(body, info.name), "descriptor carries the display name");
            CHECK(has(body, "kind: foreign"), "descriptor is marked foreign");
            CHECK(has(body, info.target), "descriptor carries the target");
            CHECK(!has(body, "fshim"), "the template's own id is gone");

            CHECK(blob_of(full, tgt, sizeof tgt, nm, sizeof nm), "the blob is present");
            printf("  blob target='%s' name='%s'\n", tgt, nm);
            CHECK(strcmp(tgt, info.target) == 0, "blob target matches the package");
            CHECK(strcmp(nm, info.name) == 0, "blob name matches the package");
        }

        printf("\n[5] the runtime probe answers each refusal differently\n");
        {
            char s[160];
            g_elig = 1; g_carve = 1536;
            uno_pkg_runtime_str(info.target, s, sizeof s);
            printf("  eligible, no image: %s\n", s);
            CHECK(has(s, "image"), "a missing runtime image is named as such");

            g_carve = 768;
            uno_pkg_runtime_str(info.target, s, sizeof s);
            printf("  small carve:        %s\n", s);
            CHECK(has(s, "768") && has(s, "1536"),
                  "a too-small carve reports both numbers");

            g_elig = 0; g_carve = 1536;
            uno_pkg_runtime_str(info.target, s, sizeof s);
            printf("  not eligible:       %s\n", s);
            CHECK(has(s, "firmware"), "the blocker's own sentence survives");

            uno_pkg_runtime_str("linux:gimp", s, sizeof s);
            printf("  unknown runtime:    %s\n", s);
            CHECK(has(s, "No runtime"), "an unhosted target says so");

            g_elig = 1;
            CHECK(uno_pkg_launch(info.target, s, sizeof s) == 0,
                  "launch fails while no runtime is connected");
            CHECK(s[0] != 0, "and says why");
            printf("  launch:             %s\n", s);
        }

        {   /* Keep the installed shim where a human can look at it.  The
             * loader is not exercised here, so when a shim loads on the host
             * gate and is refused by the real one, this file diffed against
             * the template is the first place to look. */
            static unsigned char keep[2 << 20];
            char full[80];
            long n;
            snprintf(full, sizeof full, "APPS\\%s", shim);
            n = uno_fs_read_at(1, full, 0, keep, (long)sizeof keep);
            if (n > 0) uno_fs_write(1, "INSTALLED.UNO", keep, n);
            printf("  kept a copy as INSTALLED.UNO (%ld bytes)\n", n);
        }

        printf("\n[6] uninstall is a delete\n");
        {
            char full[80];
            snprintf(full, sizeof full, "APPS\\%s", shim);
            CHECK(uno_pkg_installed(shim) || uno_fs_size(1, full) > 0,
                  "the app is installed before removal");
            CHECK(uno_pkg_remove(info.id) == 1, "remove reports success");
            CHECK(uno_fs_size(1, full) < 0, "the shim is gone");
            CHECK(uno_fs_size(1, side) < 0, "the record is gone");
        }
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
