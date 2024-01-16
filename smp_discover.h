#ifndef SMP_DISCOVER_H
#define SMP_DISCOVER_H

#include <QString>
#include "smp_lib.h"

#define SMP_FN_DISCOVER_RESP_LEN            124
#define SMP_FN_REPORT_GENERAL_RESP_LEN      76

/* Hack to cope with MPT2 controllers which use a different
 * magic number. One one ioctl based on it is used.
 */
#define MPT2_MAGIC_NUMBER                   'L'
#define MPT2COMMAND                         _IOWR(MPT2_MAGIC_NUMBER,20,struct mpt_ioctl_command)

#define MPT_DEV_MAJOR                       10
#define MPT_DEV_MINOR                       220
#define MPT2_DEV_MINOR                      221
#define MPT3_DEV_MINOR                      222

void smp_discover(int verbose);
void mpt_discover(int verbose);
void slot_discover(int verbose);
int do_multiple_slot(smp_target_obj * top, int verbose);
void phy_control(smp_target_obj * top, int phy_id, bool disable, int verbose);

#endif // SMP_DISCOVER_H
