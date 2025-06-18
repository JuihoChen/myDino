#ifndef MPI3MR_H
#define MPI3MR_H

#include <linux/types.h>

#include "smp_mptctl_glue.h"
#include "mpi30/mpi30_transport.h"
#include "mpi30/mpi30_image.h"
#include "mpi30/mpi30_ioc.h"
#include "mpi30/mpi30_sas.h"
#include "mpi30/mpi30_cnfg.h"
#include "mpi30/mpi30_init.h"

/**
 * struct mpi3mr_compimg_ver - replica of component image
 * version defined in mpi30_image.h in host endianness
 *
 */
struct mpi3mr_compimg_ver {
    u16 build_num;
    u16 cust_id;
    u8 ph_minor;
    u8 ph_major;
    u8 gen_minor;
    u8 gen_major;
};

/**
 * struct mpi3mr_ioc_facts - replica of IOC facts data defined
 * in mpi30_ioc.h in host endianness
 *
 */
struct mpi3mr_ioc_facts {
    u32 ioc_capabilities;
    struct mpi3mr_compimg_ver fw_ver;
    u32 mpi_version;
    u32 diag_trace_sz;
    u32 diag_fw_sz;
    u32 diag_drvr_sz;
    u16 max_reqs;
    u16 product_id;
    u16 op_req_sz;
    u16 reply_sz;
    u16 exceptions;
    u16 max_perids;
    u16 max_sasexpanders;
    u32 max_data_length;
    u16 max_sasinitiators;
    u16 max_enclosures;
    u16 max_pcie_switches;
    u16 max_nvme;
    u16 max_vds;
    u16 max_hpds;
    u16 max_advhpds;
    u16 max_raid_pds;
    u16 min_devhandle;
    u16 max_devhandle;
    u16 max_op_req_q;
    u16 max_op_reply_q;
    u16 shutdown_timeout;
    u16 max_msix_vectors;
    u8 ioc_num;
    u8 who_init;
    u8 personality;
    u8 dma_mask;
    u8 protocol_flags;
    u8 sge_mod_mask;
    u8 sge_mod_value;
    u8 sge_mod_shift;
    u8 max_dev_per_tg;
    u16 max_io_throttle_group;
    u16 io_throttle_data_length;
    u16 io_throttle_low;
    u16 io_throttle_high;
    // The following is a 64-bit value porting the card information of RAID9660
    u32 card_info[2];
};

#endif // MPI3MR_H
