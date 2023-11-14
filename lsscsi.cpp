#include <byteswap.h>
#include <cstdio>
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "widget.h"
#include <QString>

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

static char errpath[LMAX_PATH];

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

static const char * scsi_device_types[] =
{
    "Direct-Access",
    "Sequential-Access",
    "Printer",
    "Processor",
    "Write-once",
    "CD-ROM",
    "Scanner",
    "Optical memory",
    "Medium Changer",
    "Communications",
    "Unknown (0xa)",
    "Unknown (0xb)",
    "Storage array",
    "Enclosure",
    "Simplified direct-access",
    "Optical card read/writer",
    "Bridge controller",
    "Object based storage",
    "Automation Drive interface",
    "Security manager",
    "Zoned Block",
    "Reserved (0x15)", "Reserved (0x16)", "Reserved (0x17)",
    "Reserved (0x18)", "Reserved (0x19)", "Reserved (0x1a)",
    "Reserved (0x1b)", "Reserved (0x1c)", "Reserved (0x1e)",
    "Well known LU",
    "No device",
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

/* Returns true if dirent entry is either a symlink or a directory
 * starting_with given name. If starting_with is NULL choose all that are
 * either symlinks or directories other than . or .. (own directory or
 * parent) . Can be tricked cause symlink could point to .. (parent), for
 * example. Otherwise return false. */
static bool
dir_or_link(const struct dirent * s, const char * starting_with)
{
    if (DT_LNK == s->d_type) {
        if (starting_with)
            return 0 == strncmp(s->d_name, starting_with, strlen(starting_with));
        return true;
    } else if (DT_DIR != s->d_type)
        return false;
    else {  /* Assume can't have zero length directory name */
        size_t len = strlen(s->d_name);

        if (starting_with)
            return 0 == strncmp(s->d_name, starting_with, strlen(starting_with));
        if (len > 2)
            return true;
        if ('.' == s->d_name[0]) {
            if (1 == len)
                return false;   /* this directory: '.' */
            else if ('.' == s->d_name[1])
                return false;   /* parent: '..' */
        }
        return true;
    }
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

static int
enclosure_dir_scan_select(const struct dirent * s)
{
    if (dir_or_link(s, "enclosure")) {
        //my_strcopy(enclosure_device.name, s->d_name, LMAX_NAME);
        //enclosure_device.ft = FT_CHAR;  /* dummy */
        //enclosure_device.d_type =  s->d_type;
        return 1;
    }
    return 0;
}

/* Return true for directory entry that is link or directory (other than a
 * directory name starting with dot) that contains "enclosure_device".
 * Else return false.  */
static bool
enclosure_scan(const char * dir_name)
{
    int num, k;
    struct dirent ** namelist;

    num = scandir(dir_name, &namelist, enclosure_dir_scan_select, NULL);
    if (num < 0) {
        snprintf(errpath, LMAX_PATH, "%s: scandir: %s", __func__, dir_name);
        perror(errpath);
        return false;
    }
    for (k = 0; k < num; ++k)
        free(namelist[k]);
    free(namelist);
    return !! num;
}

/* If 'dir_name'/'base_name' is a directory chdir to it. If that is successful
   return true, else false */
static bool
if_directory_chdir(const char * dir_name, const char * base_name)
{
    char b[LMAX_PATH];
    struct stat a_stat;

    snprintf(b, sizeof(b), "%s/%s", dir_name, base_name);
    if (stat(b, &a_stat) < 0)
        return false;
    if (S_ISDIR(a_stat.st_mode)) {
        if (chdir(b) < 0)
            return false;
        return true;
    }
    return false;
}

/* If 'dir_name'/'base_name' is found places corresponding value in 'value'
 * and returns true . Else returns false.
 */
static bool
get_value(const char * dir_name, const char * base_name, char * value,
          int max_value_len)
{
    int len;
    FILE * f;
    char b[LMAX_PATH];

    snprintf(b, sizeof(b), "%s/%s", dir_name, base_name);
    if (NULL == (f = fopen(b, "r"))) {
        return false;
    }
    if (NULL == fgets(value, max_value_len, f)) {
        /* assume empty */
        value[0] = '\0';
        fclose(f);
        return true;
    }
    len = strlen(value);
    if ((len > 0) && (value[len - 1] == '\n'))
        value[len - 1] = '\0';
    fclose(f);
    return true;
}

static int
index_expander(const char * dir_name, const char * devname)
{
    int vlen;
    char wd[LMAX_PATH];
    char value[LMAX_NAME];

    snprintf(wd, sizeof(wd), "%s/%s/enclosure/%s", dir_name, devname, devname);

    vlen = sizeof(value);
    if (get_value(wd, "id", value, vlen)) {
        printf("Found an enclosure wwid: %s\n", value);
        int len = strlen(value) - 2;
        if (QString(value + len).toInt(0, 16) <= 0x3f)
            return 0;
        else if (QString(value + len).toInt(0, 16) <= 0x7f)
            return 1;
        else if (QString(value + len).toInt(0, 16) <= 0xbf)
            return 2;
        else
            return 3;
    }
    return -1;
}

/* This is a function to determine the distance between device and the expander */
int
compute_device_index(const char * device, const char * expander)
{
    struct addr_hctl dev_hctl;
    struct addr_hctl exp_hctl;

    if (! parse_colon_list(device, &dev_hctl)) {
        return -1;
    }
    if (! parse_colon_list(expander, &exp_hctl)) {
        return -1;
    }
    if (dev_hctl.h != exp_hctl.h || dev_hctl.c != exp_hctl.c) {
        return -1;
    }
    return exp_hctl.t - dev_hctl.t;
}

/* List SCSI devices (LUs). */
void
list_sdevices(Widget* pw)
{
    int num, k, prev;
    struct dirent ** namelist;
    char buff[LMAX_DEVPATH];
    char name[LMAX_NAME];
    QString path;

    printf("listing...\n");

    snprintf(buff, sizeof(buff), "%s%s", sysfsroot, bus_scsi_devs);

    num = scandir(buff, &namelist, sdev_dir_scan_select, sdev_scandir_sort);
    if (num < 0) {  /* scsi mid level may not be loaded */
        path = QString("%1: scandir: %2").arg(__func__, buff);
        perror(path.toStdString().c_str());
        printf("SCSI mid level module may not be loaded\n");
    }

    for (prev = k = 0; k < num; ++k) {
        my_strcopy(name, namelist[k]->d_name, sizeof(name));
        path = QString("%1/%2").arg(buff, name);
        if (enclosure_scan(path.toStdString().c_str())) {
            int e = index_expander(buff, name);
            if (e < 0) {
                pw->appendMessage(QString("error: cannot get expander[%1] wwid!").arg(name));
            } else {
                for (; prev < k; ++prev) {
                    gDevices.setSlot(buff, namelist[prev]->d_name, namelist[k]->d_name, e);
                }
                gControllers.setController(buff, namelist[k]->d_name, e);
                prev = k + 1;
            }
        }
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}
