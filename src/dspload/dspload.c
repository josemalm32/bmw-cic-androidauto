/* dspload: helper for the DM642 video DSP and the installer.
 *
 *   dspload patch <out>        read the car's own DSP image, check it is the known
 *                              version, write a copy with the 800-px patch to <out>
 *   dspload on <patched>       pathmgr_symlink(patched -> original) + DSP reboot
 *   dspload off                remove the link + DSP reboot (stock image)
 *   dspload linked <patched>   exit 0 if the link is active
 *   dspload verify <a> <b>     exit 0 if files are identical
 *   dspload sysreboot          sync + clean CIC reboot (installer)
 *
 * The link lives in RAM (procnto prefix): a CIC reboot always restores the stock
 * image; flash is never modified. dev-dspipc reloads the firmware from its stored
 * filename= on DSPIPC_REBOOT, so the link redirects it to the patched copy.
 *
 * DSP patch, E9x image (dvd_dm642.bin, 2363004 bytes, TI boot table, big-endian words;
 * the E90 C13 image, 2356132 bytes, gets the same three patches at its own offsets):
 *   file 0x094518: 0x02016829 -> 0x02019029  vddSetupDecoder width check 720 -> 800
 *                                            (C64x "mvk 0x2d0,a4" at 0x81f147e4)
 *   file 0x1ddcfc: 0x02801728 -> 0x028019a8  H.264 SPS pic_width_in_mbs limit 46 -> 51
 *                                            (C64x "mvk 0x2e,a5" at 0x819527ec)
 *   file 0x09ba68: 0x000c0275 -> 0x00000001  late-frame skip counter: the store
 *                                            "stw A0,*A3" at 0x81f1bd34 becomes a NOP
 *                                            (p-bit kept, so the execute packet is unchanged)
 *
 * Why the third patch: after one late frame the DVD firmware computes how many
 * frames it is behind and then silently drops that many following frames (it saw
 * values like 39). A DVD always delivers new frames, Android Auto does not: it only
 * sends a frame when the screen changes. So after e.g. fast scrolling the picture
 * froze until N more frames had arrived (tilting the controller "helped" because it
 * produces new frames). With the counter store removed a late frame is simply shown
 * late and the next frames are shown normally.
 * Output only to stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/stat.h>
#include <sys/pathmgr.h>
#include <sys/sysmgr.h>
#include "aap_paths.h"

#define ORIG AAP_DSP_ORIG
#define DEV  "/dev/dspipc/dm642.0/ctrl"   /* libdspipc opens "%s/ctrl" O_RDWR */
#define DSPIPC_REBOOT 0x8004050c
struct DspPatch { long off; unsigned old_w, new_w; };
static const struct DspPatch kPatchE9x[] = {
    { 0x094518L, 0x02016829u, 0x02019029u },
    { 0x1ddcfcL, 0x02801728u, 0x028019a8u },
    { 0x09ba68L, 0x000c0275u, 0x00000001u },   /* no frame dropping after a late frame */
};
/* E90 C13 (CIC_L6_1009 B1009-500-1019C): same firmware, relocated.
 *   0x81f199c4 width check, 0x8195216c SPS limit, 0x81f16084 "stw A0,*A5" (skip counter
 *   0x8190871c; p-bit 0 here, so the NOP is 0x00000000). Found by context match and
 *   disassembly (04_analysis/dsp_dm642/c13); untested in a car. */
static const struct DspPatch kPatchC13[] = {
    { 0x0996f8L, 0x02016829u, 0x02019029u },
    { 0x1dc2c4L, 0x02801728u, 0x028019a8u },
    { 0x095db8L, 0x00140274u, 0x00000000u },
};
#define N_OF(a) ((int)(sizeof a / sizeof a[0]))
static const struct { const char *name; long size; const struct DspPatch *p; int n; } kImages[] = {
    { "E9x 9283417A", 2363004L, kPatchE9x, N_OF(kPatchE9x) },
    { "E90 C13 B1009", 2356132L, kPatchC13, N_OF(kPatchC13) },
};

static int dsp_reboot(void)
{
    int fd, rc, arg = 0;
    fd = open(DEV, O_RDWR);
    if (fd < 0) { printf("REBOOT open %s err=%d\n", DEV, errno); return 2; }
    rc = devctl(fd, DSPIPC_REBOOT, &arg, sizeof arg, NULL);
    printf("DSP_REBOOT rc=%d\n", rc);
    close(fd);
    return rc == EOK ? 0 : 3;
}

/* 0 if identical */
static int same(const char *a, const char *b)
{
    static unsigned char x[4096], y[4096];
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY), na, nb, r = 1;
    if (fa >= 0 && fb >= 0) {
        for (;;) {
            na = read(fa, x, sizeof x); nb = read(fb, y, sizeof y);
            if (na != nb || na < 0) break;
            if (na == 0) { r = 0; break; }
            if (memcmp(x, y, na)) break;
        }
    }
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return r;
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }
static void put_be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int patch(const char *out)
{
    unsigned char *img; long n; int fd, i, k; char tmp[256];
    const struct DspPatch *kPatch = 0; int npatch = 0;
    fd = open(ORIG, O_RDONLY);
    if (fd < 0) { printf("PATCH open %s err=%d\n", ORIG, errno); return 1; }
    n = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
    for (k = 0; k < N_OF(kImages); k++)
        if (n == kImages[k].size) { kPatch = kImages[k].p; npatch = kImages[k].n; printf("PATCH image %s\n", kImages[k].name); break; }
    if (!kPatch) { printf("PATCH unknown DSP image size %ld\n", n); close(fd); return 1; }
    img = malloc(n);
    if (!img || read(fd, img, n) != n) { printf("PATCH read failed\n"); close(fd); free(img); return 1; }
    close(fd);
    for (i = 0; i < npatch; i++) {
        unsigned w = be32(img + kPatch[i].off);
        if (w == kPatch[i].new_w) continue;          /* already patched (source was an older patched copy) */
        if (w != kPatch[i].old_w) { printf("PATCH unknown DSP image (0x%lx = 0x%08x)\n", kPatch[i].off, w); free(img); return 1; }
        put_be32(img + kPatch[i].off, kPatch[i].new_w);
    }
    /* Written in place: /tmp on the CIC is shmem, where rename() fails with
       EXDEV (err=18). The installer verifies and atomically installs the result. */
    (void)tmp;
    unlink(out);
    fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, img, n) != n) { printf("PATCH write %s failed err=%d\n", out, errno); if (fd >= 0) close(fd); unlink(out); free(img); return 1; }
    fsync(fd); close(fd); free(img);
    printf("PATCH_OK %s\n", out);
    return 0;
}

static int linked(const char *img)
{
    char b[256]; int n = readlink(ORIG, b, sizeof b - 1);
    if (n <= 0) return 1;
    b[n] = 0;
    return strcmp(b, img) != 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    int rc;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(mode, "verify") && argc > 3) return same(argv[2], argv[3]);
    if (!strcmp(mode, "patch") && argc > 2) return patch(argv[2]);
    if (!strcmp(mode, "linked") && argc > 2) return linked(argv[2]);
    if (!strcmp(mode, "on") && argc > 2) {
        struct stat a, b;
        if (!linked(argv[2])) { printf("DSP_ALREADY_LINKED\n"); return 0; }
        if (stat(argv[2], &a) || stat(ORIG, &b) || a.st_size != b.st_size) { printf("DSP_IMAGE_MISSING_OR_SIZE\n"); return 1; }
        rc = pathmgr_symlink(argv[2], ORIG);
        printf("DSP_LINK rc=%d errno=%d\n", rc, rc ? errno : 0);
        if (rc) return 4;
        return dsp_reboot();
    }
    if (!strcmp(mode, "off")) {
        rc = pathmgr_unlink(ORIG);
        printf("DSP_UNLINK rc=%d\n", rc);
        return dsp_reboot();
    }
    if (!strcmp(mode, "sysreboot")) {
        printf("SYSREBOOT in 3 s\n"); sync(); sleep(3); sync();
        return sysmgr_reboot();
    }
    printf("usage: dspload patch|on|off|linked|verify|sysreboot ...\n");
    return 1;
}
