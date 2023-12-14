#include <dirent.h>

#include "./smp_discover.h"

static const char * dev_bsg = "/dev/bsg";

static int
smp_initiator_open()
{
    return 0;
}

static int
bsgdev_scan_select(const struct dirent * s)
{
    if (strstr(s->d_name, "expander-") && strchr(s->d_name, ':')) {
        return 1;
    }
    /* Still need to filter out "." and ".." */
    return 0;
}

void doDiscover(Widget* pw)
{
    int num, k;
    struct dirent ** namelist;

    printf("discovering...\n");

    num = scandir(dev_bsg, &namelist, bsgdev_scan_select, NULL);
    if (num < 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        pw->appendMessage("HBA mid level module may not be loaded.");
        return;
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}
