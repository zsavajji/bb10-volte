#include "ims_patch.h"
#include "sys_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/pathmgr.h>

#define SYSTEM_PATH "/radio/bin/ims_rrm"
#define IMS_RRM_SIZE 4787852 /* size of ims_rrm on the only rooted build, patched or not */
#define PATH_BUF (512 + 16)

/* The home-domain site is only wanted when a carrier uses its own learned domain (sip-learned).
   With this off it is reverted to the stock bytes. */
int g_ims_patch_home_domain = 0;

/* The RTCP-bandwidth site is only wanted for carriers that ship data/rtcp-bw-<id>; otherwise it is reverted to the stock bytes. */
int g_ims_patch_rtcp_bw = 0;

enum
{
    PATCH_ALWAYS,
    PATCH_HOME,   /* only with g_ims_patch_home_domain */
    PATCH_RTCP,   /* only with g_ims_patch_rtcp_bw */
    PATCH_RETIRED /* shipped by 2.34-2.37, now always reverted to stock */
};

typedef struct
{
    size_t off;
    unsigned char from[2];
    unsigned char to[2];
    int use;
} patch_t;

static const patch_t patches[] = {
    /* SDP parser: an e= line it can't parse aborted the whole answer and hung up the call
       (Telus sends e=imsstirshaken@telus.com, valid per RFC 4566).
       `cbnz r0` becomes `b`, so the line is skipped. */
    {0x2086ae, {0x68, 0xb9}, {0x0d, 0xe0}, PATCH_ALWAYS},
    /* Remote numbers without a '+' were shown as user@host (Viettel national format).
       `beq` becomes `nop`, so the user part is always used. */
    {0xd9ffa, {0x08, 0xd0}, {0x00, 0xbf}, PATCH_ALWAYS},
    /* The SDP answer carried our own AMR/AMR-WB mode-set instead of the offered one.
       Telus offers mode-set=7 in an UPDATE and rejects 0,2,4,7 with 406, killing the call.
       ims_rrm can copy the offered mode-set, gated on AMR[WB].CopyRemoteModeSet, but only
       the '3GPP' scheme sets those to 1, and onMobileTechnologyChanged renames an unknown
       scheme ('3GPP' included) to 'Vodafone' first, so the gate always read its default 0.
       `movs r1, #0` becomes `movs r1, #1`: the default for AMR-WB, then AMR. */
    {0x24c6d8, {0x00, 0x21}, {0x01, 0x21}, PATCH_ALWAYS},
    {0x24c700, {0x00, 0x21}, {0x01, 0x21}, PATCH_ALWAYS},
    /* SDP b=RR/b=RS. Every SDP says b=RS:0 b=RR:0 (RTCP off); Telekom DE ends calls with
       503 (1:223) as soon as it sees our SDP, and its own offers carry b=RS:600 b=RR:2000.
       calcRtcpBandwidth (0x1d9ac8) first checks the RTCP period at this+0x608 and writes
       RR = RS = 0 when it is <= 0. The period is getInt("MediaDialog.PeriodSR", 1000) only if
       the media check at vtable +0x120 (0x1db26c) passes, else 0, and on these calls it is 0.
       `ble` becomes `nop`, so the scheme-3GPP formula always runs: RR = 3.75% and RS = 1.25%
       of AudioBandwidth * 1000. The non-3GPP branch divides by the period, so this stays
       per carrier (data/rtcp-bw-<id>), on carriers known to use scheme 3GPP. */
    {0x1d9b16, {0x55, 0xdd}, {0x00, 0xbf}, PATCH_RTCP},
    /* The formula reads "Updated.MediaDialog.AudioBandwidth", 0 at offer time. The literal
       read at 0x1d9b6e is moved 8 bytes on, to the string's own suffix
       "MediaDialog.AudioBandwidth" (41 from startup), giving b=RR:1537 b=RS:512. */
    {0x1d9c1c, {0xe7, 0x15}, {0xef, 0x15}, PATCH_RTCP},
    /* Per-carrier home domain. The setter (0x8d0ea) keeps the domain in the base slot +0x300
       with its source; while that source is 9 (IMSI/IMPU-derived) or 2 (@ims
       Home_network_domain_name), it only accepts a new value from source 7 or 9, so the
       carrier config always lost to the IMSI domain. `sub.w lr, r4, #9` becomes `#2`: the
       accepted pair is now 7 or 2, so the config replaces the IMSI domain whichever comes
       first and the IMSI domain can't take it back. The value stays in +0x300 with the stock
       write, so every reader agrees (the dial getter, the HomeDomain dump, registration). */
    {0x8d176, {0x09, 0x0e}, {0x02, 0x0e}, PATCH_HOME},
    /* 2.34-2.37 moved source 2 into the override slot +0x320 as source 7 instead. That left
       +0x300 empty, which code reading it directly can't handle: CoopVoce logged
       `HomeDomain[0]:` and looped on `route: INVALID to CELL` with no REGISTER. */
    {0x8d218, {0x07, 0x2c}, {0x02, 0x2c}, PATCH_RETIRED},
    {0x8d220, {0x22, 0x46}, {0x07, 0x22}, PATCH_RETIRED},
};

#define PATCH_COUNT (sizeof(patches) / sizeof(patches[0]))

static int patch_wanted(size_t i)
{
    return patches[i].use == PATCH_ALWAYS ||
           (patches[i].use == PATCH_HOME && g_ims_patch_home_domain) ||
           (patches[i].use == PATCH_RTCP && g_ims_patch_rtcp_bw);
}

/* A site may already hold its patched bytes, since other addons copy and stack on our copy.
   Anything else at a site means a different build, and nothing is touched. */
int ims_patch_buffer(unsigned char *buf, size_t len)
{
    int changed = 0;
    size_t i;

    for (i = 0; i < PATCH_COUNT; i++)
    {
        if (patches[i].off + 2 > len ||
            (memcmp(buf + patches[i].off, patches[i].from, 2) != 0 &&
             memcmp(buf + patches[i].off, patches[i].to, 2) != 0))
            return -1;
    }

    for (i = 0; i < PATCH_COUNT; i++)
    {
        int want_to = patch_wanted(i);
        const unsigned char *target = want_to ? patches[i].to : patches[i].from;
        const unsigned char *other = want_to ? patches[i].from : patches[i].to;

        if (memcmp(buf + patches[i].off, target, 2) != 0 &&
            memcmp(buf + patches[i].off, other, 2) == 0)
        {
            memcpy(buf + patches[i].off, target, 2);
            changed = 1;
        }
    }

    return changed;
}

/* The whole file in memory, or NULL unless it is exactly IMS_RRM_SIZE bytes. */
static unsigned char *read_exact(const char *path)
{
    unsigned char *buf = malloc(IMS_RRM_SIZE + 1);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t len = 0;
    ssize_t n = 0;

    if (!buf || fd < 0)
    {
        free(buf);
        if (fd >= 0)
            close(fd);
        return NULL;
    }

    /* One byte more than expected, so a longer file shows up as a size mismatch. */
    while (len <= IMS_RRM_SIZE)
    {
        n = read(fd, buf + len, IMS_RRM_SIZE + 1 - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += n;
    }
    close(fd);

    if (n < 0 || len != IMS_RRM_SIZE)
    {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Writes <dst>.tmp, then renames it over dst, so dst is never half written. */
static int write_replace(const char *dst, const unsigned char *buf, size_t len)
{
    char tmp[PATH_BUF];
    size_t done = 0;
    int ok;
    int fd;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", dst) >= (int)sizeof(tmp))
        return -1;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (fd < 0)
        return -1;

    while (done < len)
    {
        ssize_t n = write(fd, buf + done, len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += n;
    }

    ok = (close(fd) == 0 && done == len && rename(tmp, dst) == 0);
    if (!ok)
        unlink(tmp);
    return ok ? 0 : -1;
}

/* src and dst may be the same file: src is read fully first and dst is replaced by a rename. */
int ims_patch_file(const char *src, const char *dst)
{
    unsigned char *buf = read_exact(src);
    int rc;

    if (!buf)
        return -1;

    rc = ims_patch_buffer(buf, IMS_RRM_SIZE);
    if (rc == 1 && write_replace(dst, buf, IMS_RRM_SIZE) != 0)
        rc = -1;

    free(buf);
    return rc;
}

/* The installer links SYSTEM_PATH to <init>/ims_rrm at boot, and another addon installed later can
   link its own patched copy over ours. A reinstall copies the file onto itself, which leaves it
   empty (or stale, if ims_rrm was running). So check whatever SYSTEM_PATH resolves to before each
   start and rebuild our copy if it is unusable. */
const char *ims_rrm_binary(void)
{
    static char path[PATH_BUF];
    char trust[PATH_BUF + 1];
    int rc;

    snprintf(path, sizeof(path), "%s/ims_rrm", g_init_path);

    rc = ims_patch_file(SYSTEM_PATH, path);
    if (rc < 0)
    {
        /* Drops a link to a broken copy. The real file on the read-only rcfs can't be deleted. */
        unlink(SYSTEM_PATH);
        rc = ims_patch_file(SYSTEM_PATH, path);
    }
    if (rc < 0)
    {
        log_msg("[BBE] IMS_RRM: unpatched (unknown build)");
        return SYSTEM_PATH;
    }

    /* Already carries our bytes, maybe in another addon's copy stacked on ours. */
    if (rc == 0)
    {
        log_msg("[BBE] IMS_RRM: patched");
        return SYSTEM_PATH;
    }

    /* Files written at runtime are untrusted and can't be executed.
       The addon installer trusts its own files the same way. */
    snprintf(trust, sizeof(trust), "!%s", path);
    char *argv[] = {"/proc/boot/pathtrust", trust, NULL};
    if (run_cmd_spawn(argv, NULL) != 0)
    {
        log_msg("[BBE] IMS_RRM: unpatched (trust failed)");
        return SYSTEM_PATH;
    }

    /* Same link the installer makes at boot. */
    unlink(SYSTEM_PATH);
    if (pathmgr_symlink(path, SYSTEM_PATH) != 0)
    {
        log_msg("[BBE] IMS_RRM: link failed: %s", strerror(errno));
        return path;
    }

    log_msg("[BBE] IMS_RRM: repaired");
    return SYSTEM_PATH;
}
