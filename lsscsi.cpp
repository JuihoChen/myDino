#include <byteswap.h>
#include <cstdio>
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef PATH_MAX
#define LMAX_PATH PATH_MAX
#else
#define LMAX_PATH 2048
#endif

#ifdef NAME_MAX
#define LMAX_NAME (NAME_MAX + 1)
#else
#define LMAX_NAME 256
#endif

#define LMAX_DEVPATH (LMAX_NAME + 128)

#define UINT64_LAST ((uint64_t)~0)

static const char * sysfsroot = "/sys";
static const char * bus_scsi_devs = "/bus/scsi/devices";

/* For SCSI 'h' is host_num, 'c' is channel, 't' is target, 'l' is LUN is
 * uint64_t and lun_arr[8] is LUN as 8 byte array. For NVMe, h=0x7fff
 * (NVME_HOST_NUM) and displayed as 'N'; 'c' is Linux's NVMe controller
 * number, 't' is NVMe Identify controller CTNLID field, and 'l' is
 * namespace id (1 to (2**32)-1) rendered as a little endian 4 byte sequence
 * in lun_arr, last 4 bytes are zeros. invalidate_hctl() puts -1 in
 * integers, 0xff in bytes */
struct addr_hctl {
    int h;                 /* if h==0x7fff, display as 'N' for NVMe */
    int c;
    int t;
    uint64_t l;           /* SCSI: Linux word flipped; NVME: uint32_t */
    uint8_t lun_arr[8];   /* T10, SAM-5 order; NVME: little endian */
};

#ifdef __GNUC__
static int pr2serr(const char * fmt, ...)
    __attribute__ ((format (printf, 1, 2)));
#else
static int pr2serr(const char * fmt, ...);
#endif

static int
pr2serr(const char * fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    n = vfprintf(stderr, fmt, args);
    va_end(args);
    return n;
}

static inline void sg_put_unaligned_be16(uint16_t val, void *p)
{
    uint16_t u = bswap_16(val);

    memcpy(p, &u, 2);
}

/* Copies (dest_maxlen - 1) or less chars from src to dest. Less chars are
 * copied if '\0' char found in src. As long as dest_maxlen > 0 then dest
 * will be '\0' terminated on exit. If dest_maxlen < 1 then does nothing. */
static void
my_strcopy(char *dest, const char *src, int dest_maxlen)
{
    const char * lp;

    if (dest_maxlen < 1)
        return;
    lp = (const char *)memchr(src, 0, dest_maxlen);
    if (NULL == lp) {
        memcpy(dest, src, dest_maxlen - 1);
        dest[dest_maxlen - 1] = '\0';
    } else
        memcpy(dest, src, (lp  - src) + 1);
}

/* Parse colon_list into host/channel/target/lun ("hctl") array, return true
 * if successful, else false. colon_list should point at first character of
 * hctl (i.e. a digit) and yields a new value in *outp when true returned. */
static bool
parse_colon_list(const char * colon_list, struct addr_hctl * outp)
{
    int k;
    uint64_t z;
    const char * elem_end;

    if ((! colon_list) || (! outp))
        return false;

    if (1 != sscanf(colon_list, "%d", &outp->h))
            return false;
    if (NULL == (elem_end = strchr(colon_list, ':')))
        return false;
    colon_list = elem_end + 1;
    if (1 != sscanf(colon_list, "%d", &outp->c))
        return false;
    if (NULL == (elem_end = strchr(colon_list, ':')))
        return false;
    colon_list = elem_end + 1;
    if (1 != sscanf(colon_list, "%d", &outp->t))
        return false;
    if (NULL == (elem_end = strchr(colon_list, ':')))
        return false;
    colon_list = elem_end + 1;
    if (1 != sscanf(colon_list, "%" SCNu64 , &outp->l))
        return false;
    z = outp->l;
    for (k = 0; k < 8; k += 2, z >>= 16)
        sg_put_unaligned_be16((uint16_t)z, outp->lun_arr + k);
    return true;
}

/* Compare <host:controller:target:lun> tuples (aka <h:c:t:l> or hctl) */
static int
cmp_hctl(const struct addr_hctl * le, const struct addr_hctl * ri)
{
    if (le->h == ri->h) {
        if (le->c == ri->c) {
            if (le->t == ri->t)
                return ((le->l == ri->l) ? 0 :
                            ((le->l < ri->l) ? -1 : 1));
            else
                return (le->t < ri->t) ? -1 : 1;
        } else
            return (le->c < ri->c) ? -1 : 1;
    } else
        return (le->h < ri->h) ? -1 : 1;
}

static int
sdev_dir_scan_select(const struct dirent * s)
{
    /* Following no longer needed but leave for early lk 2.6 series */
    if (strstr(s->d_name, "mt"))
        return 0;       /* st auxiliary device names */
    if (strstr(s->d_name, "ot"))
        return 0;       /* osst auxiliary device names */
    if (strstr(s->d_name, "gen"))
        return 0;
    /* Above no longer needed but leave for early lk 2.6 series */
    if (!strncmp(s->d_name, "host", 4)) /* SCSI host */
        return 0;
    if (!strncmp(s->d_name, "target", 6)) /* SCSI target */
        return 0;
    if (strchr(s->d_name, ':')) {
        return 1;
    }
    /* Still need to filter out "." and ".." */
    return 0;
}

/* This is a compare function for numeric sort based on hctl tuple.
 * Returns -1 if (a->d_name < b->d_name) ; 0 if they are equal
 * and 1 otherwise. */
static int
sdev_scandir_sort(const struct dirent ** a, const struct dirent ** b)
{
    const char * lnam = (*a)->d_name;
    const char * rnam = (*b)->d_name;
    struct addr_hctl left_hctl;
    struct addr_hctl right_hctl;

    if (! parse_colon_list(lnam, &left_hctl)) {
        pr2serr("%s: left parse failed: %.20s\n", __func__,
                (lnam ? lnam : "<null>"));
        return -1;
    }
    if (! parse_colon_list(rnam, &right_hctl)) {
        pr2serr("%s: right parse failed: %.20s\n", __func__,
                (rnam ? rnam : "<null>"));
        return 1;
    }
    return cmp_hctl(&left_hctl, &right_hctl);
}

/* List one SCSI device (LU) on a line. */
static void
one_sdev_entry(const char * dir_name, const char * devname)
{
    printf("%s/%s\n", dir_name, devname);
}

/* List SCSI devices (LUs). */
void
list_sdevices()
{
    int num, k;
    struct dirent ** namelist;
    char buff[LMAX_DEVPATH];
    char name[LMAX_NAME];

    snprintf(buff, sizeof(buff), "%s%s", sysfsroot, bus_scsi_devs);

    num = scandir(buff, &namelist, sdev_dir_scan_select, sdev_scandir_sort);
    if (num < 0) {  /* scsi mid level may not be loaded */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(name, sizeof(name), "%s: scandir: %s", __func__, buff);
#pragma GCC diagnostic pop
        perror(name);
        printf("SCSI mid level module may not be loaded\n");
    }

    for (k = 0; k < num; ++k) {
        my_strcopy(name, namelist[k]->d_name, sizeof(name));
        one_sdev_entry(buff, name);
        free(namelist[k]);
    }
    free(namelist);
}
