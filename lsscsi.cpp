#include <cstdio>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

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

//#define UINT64_LAST ((uint64_t)~0)

static const char * sysfsroot = "/sys";
static const char * bus_scsi_devs = "/bus/scsi/devices";

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

/* List SCSI devices (LUs). */
void
list_sdevices()
{
    int num, k;
    struct dirent ** namelist;
    char buff[LMAX_DEVPATH];
    char name[LMAX_NAME];

    snprintf(buff, sizeof(buff), "%s%s", sysfsroot, bus_scsi_devs);

    num = scandir(buff, &namelist, sdev_dir_scan_select, alphasort);
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
        //one_sdev_entry(buff, name, op);
        printf("%s\n", name);
        free(namelist[k]);
    }
    free(namelist);
}
