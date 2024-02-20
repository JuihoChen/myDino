#include <QWidget>

#include <dirent.h>
#include <linux/bsg.h>
#include <scsi/scsi_bsg_mpi3mr.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#include "mpi3mr.h"
#include "mpi3mr_app.h"
#include "smp_lib.h"
#include "widget.h"

/* Maximum number of Controllers is assumed to be 4 */
#define NUM_IOC 4

static int ioc_cnt;
static struct mpi3mr_ioc_facts ioc_facts[NUM_IOC];
static struct mpi3mr_bsg_in_adpinfo adpinfo[NUM_IOC];

/* To differentiate from MPI3 pass through commands */
#define DRVBSG_OPCODE (0x1 << 31)

/* make a space reserved for mpb (mpi3mr bsg packet)
 *      mpi3mr_bsg_packet -> mpi3mr_bsg_drv_cmd
 *                        -> mpi3mr_bsg_mptcmd -> mpi3mr_buf_entry_list -> mpi3mr_buf_entry
 *                                                                       + mpi3mr_buf_entry * 9
 */
#define NUM_BYTES (sizeof(struct mpi3mr_bsg_packet) + (9 * sizeof(struct mpi3mr_buf_entry)))

static char mbp_pool[NUM_BYTES];
static struct mpi3mr_bsg_packet & mbp = *(struct mpi3mr_bsg_packet *)mbp_pool;

static char request_m[1024];
static char reply_m[1024];

class mpi3_request
{
public:
    mpi3_request(int subvalue, int64_t target_sa, smp_req_resp * rr);
    ~mpi3_request() {}

    int fill_request();
    int response_len(void * mpi_reply);

public:
    int request_sz;
    int reply_sz;

private:
    int populate_adpinfo();
    int issue_iocfacts();
    int smp_passthrough();

private:
    int ioc_id;
    int64_t sas_address;
    smp_req_resp * rresp;
};

mpi3_request::mpi3_request(int subvalue, int64_t target_sa, smp_req_resp * rr)
{
    memset(mbp_pool, 0, sizeof(mbp_pool));
    memset(request_m, 0, sizeof(request_m));
    memset(reply_m, 0, sizeof(reply_m));

    ioc_id = subvalue;
    sas_address = target_sa;
    rresp = rr;
    request_sz = 0;
    reply_sz = 0;
}

int mpi3_request::fill_request()
{
    int noFunc = rresp->mpi3mr_function;

    switch (noFunc) {
    case MPI3_FUNCTION_IOC_FACTS:
        return issue_iocfacts();
    case MPI3_FUNCTION_SMP_PASSTHROUGH:
        return smp_passthrough();
    default:
        switch (noFunc - DRVBSG_OPCODE) {
        case MPI3MR_DRVBSG_OPCODE_ADPINFO:
            return populate_adpinfo();
        }
        qDebug("%s: ileegal function id=0x%x", __func__, noFunc);
        break;
    }
    return -1;
}

int mpi3_request::response_len(void * mpi_reply)
{
    int noFunc = rresp->mpi3mr_function;

    switch(noFunc) {
    case MPI3_FUNCTION_IOC_FACTS:
        return ((struct mpi3_ioc_facts_data *)reply_m)->ioc_facts_data_length * 4;
    case MPI3_FUNCTION_SMP_PASSTHROUGH:
        return ((struct mpi3_smp_passthrough_reply *)mpi_reply)->response_data_length;
    default:
        switch (noFunc - DRVBSG_OPCODE) {
        case MPI3MR_DRVBSG_OPCODE_ADPINFO:
            return rresp->max_response_l;
        }
        break;
    }
    return 0;
}

int mpi3_request::populate_adpinfo()
{
    /* This is to process a Driver Command, not a message passthrough
     *
     *  hdr.dout_xfer_len should be zero not to support BIDI
     */

    mbp.cmd_type = MPI3MR_DRV_CMD;
    mbp.cmd.drvrcmd.mrioc_id = ioc_id;       // Set the IOC number prior to issuing this command.
    mbp.cmd.drvrcmd.opcode = MPI3MR_DRVBSG_OPCODE_ADPINFO;

    return sizeof(mbp);
}

int mpi3_request::issue_iocfacts()
{
    struct dummy_reply { char dummy[21]; };
    struct mpi3_ioc_facts_request * mpi_request;

    request_sz = sizeof(struct mpi3_ioc_facts_request);
    reply_sz = sizeof(struct dummy_reply);

    mpi_request = (struct mpi3_ioc_facts_request *)request_m;
    mpi_request->function = rresp->mpi3mr_function;

    const int ecnt = 3;
    mbp.cmd_type = MPI3MR_MPT_CMD;
    mbp.cmd.mptcmd.timeout = 180;           // ?a hacked value
    mbp.cmd.mptcmd.mrioc_id = ioc_id;       // Set the IOC number prior to issuing this command.
    mbp.cmd.mptcmd.buf_entry_list.num_of_entries = ecnt;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[0].buf_type = MPI3MR_BSG_BUFTYPE_DATA_IN;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[0].buf_len = rresp->max_response_l;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[1].buf_type = MPI3MR_BSG_BUFTYPE_MPI_REPLY;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[1].buf_len = reply_sz;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[2].buf_type = MPI3MR_BSG_BUFTYPE_MPI_REQUEST;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[2].buf_len = request_sz;

    return offsetof(struct mpi3mr_bsg_packet, cmd.mptcmd.buf_entry_list.buf_entry[ecnt]);
}

int mpi3_request::smp_passthrough()
{
    struct mpi3_smp_passthrough_request * mpi_request;

    request_sz = sizeof(struct mpi3_smp_passthrough_request);
    reply_sz = sizeof(struct mpi3_smp_passthrough_reply);

    memcpy(request_m, rresp->request, rresp->request_len);
    mpi_request = (struct mpi3_smp_passthrough_request *)(request_m + rresp->request_len);
    mpi_request->function = rresp->mpi3mr_function;
    mpi_request->io_unit_port = 0xFF;       // ?invalid port number (rphy)
    mpi_request->sas_address = sas_address;

    const int ecnt = 4;
    mbp.cmd_type = MPI3MR_MPT_CMD;
    mbp.cmd.mptcmd.timeout = 180;           // ?a hacked value
    mbp.cmd.mptcmd.mrioc_id = ioc_id;       // Set the IOC number prior to issuing this command.
    mbp.cmd.mptcmd.buf_entry_list.num_of_entries = ecnt;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[0].buf_type = MPI3MR_BSG_BUFTYPE_DATA_OUT;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[0].buf_len = rresp->request_len;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[1].buf_type = MPI3MR_BSG_BUFTYPE_DATA_IN;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[1].buf_len = rresp->max_response_l;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[2].buf_type = MPI3MR_BSG_BUFTYPE_MPI_REPLY;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[2].buf_len = reply_sz;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[3].buf_type = MPI3MR_BSG_BUFTYPE_MPI_REQUEST;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[3].buf_len = request_sz;

    return offsetof(struct mpi3mr_bsg_packet, cmd.mptcmd.buf_entry_list.buf_entry[ecnt]);
}

/* Returns 0 on success else -1 . */
int send_req_mpi3mr_bsg(int fd, int subvalue, int64_t target_sa, smp_req_resp * rresp, int vb)
{
    mpi3_request mpi3rq(subvalue, target_sa, rresp);
    int request_l = mpi3rq.fill_request();
    if (request_l <= 0) {    // command not processed?
        return -1;
    }

    if (vb > 2) {
        qDebug() << "mpi3mr_bsg_packet:";
        hex2stdout(mbp_pool, request_l, 0);
    }

    struct sg_io_v4 hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.guard = 'Q';
    hdr.protocol = BSG_PROTOCOL_SCSI;
    hdr.subprotocol = BSG_SUB_PROTOCOL_SCSI_TRANSPORT;

    hdr.request_len = request_l;
    hdr.request = (uintptr_t) mbp_pool;

    hdr.max_response_len = sizeof(reply_m);
    hdr.response = (uintptr_t) reply_m;

    hdr.dout_xfer_len = rresp->request_len + mpi3rq.request_sz;
    hdr.dout_xferp = (uintptr_t) request_m;

    hdr.din_xfer_len = rresp->max_response_l + mpi3rq.reply_sz;
    hdr.din_xferp = (uintptr_t) reply_m;

    hdr.timeout = DEF_TIMEOUT_MS;

    if (vb > 1) {
        qDebug("%s: dout_xfer_len=%u, din_xfer_len=%u, timeout=%u ms",
               __func__, hdr.dout_xfer_len, hdr.din_xfer_len, hdr.timeout);
    }

    int res = ioctl(fd, SG_IO, &hdr);
    if (res) {
        perror("mpi3_request::fill_request: SG_IO ioctl");
        return -1;
    }

    memcpy(rresp->response, reply_m, rresp->max_response_l);
    void * mpi_reply = reply_m + rresp->max_response_l;

    /* was: rresp->act_response_l = -1; */
    rresp->act_response_l = mpi3rq.response_len(mpi_reply);
    if (vb > 1) {
        qDebug("%s: driver_status=%u, transport_status=%u", __func__, hdr.driver_status, hdr.transport_status);
        qDebug("    device_status=%u, duration=%u, info=%u", hdr.device_status, hdr.duration, hdr.info);
        qDebug("    din_resid=%d, dout_resid=%d", hdr.din_resid, hdr.dout_resid);
        qDebug("  smp_req_resp::max_response_len=%d act_response_len=%d", rresp->max_response_l, rresp->act_response_l);
        hex2stdout(rresp->response, rresp->act_response_l, 0);
        hex2stdout(mpi_reply, mpi3rq.reply_sz, 1);
    }

    if (hdr.driver_status)
        rresp->transport_err = hdr.driver_status;
    else if (hdr.transport_status)
        rresp->transport_err = hdr.transport_status;
    else if (hdr.device_status)
        rresp->transport_err = hdr.device_status;
    return 0;
}

static int mpi3mrdev_scan_select(const struct dirent * s)
{
    if (DT_LNK != s->d_type && DT_DIR != s->d_type && strstr(s->d_name, "mpi3mrctl")) {
        return 1;
    }
    /* Still need to filter out "." and ".." */
    return 0;
}

void mpi3mr_discover(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("Discovering...");

    num = scandir(dev_bsg, &namelist, mpi3mrdev_scan_select, nullptr);
    if (num <= 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        gAppendMessage("HBA mid level module may not be loaded.");
        return;
    }

    for (k = 0; k < num; ++k) {
        device_name = QString("%1/%2").arg(dev_bsg, namelist[k]->d_name);
        gAppendMessage(device_name);
        if (vb) {
            qDebug() << "----> exploring " << device_name;
        }

        // assign the IOC number for multiple adapters case
        res = smp_initiator_open(device_name, k, I_SGV4_MPI, &tobj, vb);
        if (res < 0) {
            continue;
        }

        for (int i = 0; i < 4; ++i) {
            // assign sas address for path-through
            tobj.sas_addr64 = gControllers.wwid64(i);
            if (0 != tobj.sas_addr64) {
                if (vb) {
                    qDebug("   -> with SAS address=0x%lx", tobj.sas_addr64);
                }
                res = do_multiple(&tobj, vb);
                if (res) {
                    qDebug("Exit status %d indicates error detected", res);
                }
            }
        }

        smp_initiator_close(&tobj);
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}

void mpi3mr_slot_discover(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("Slot discovering...");

    num = scandir(dev_bsg, &namelist, mpi3mrdev_scan_select, alphasort);
    if (num <= 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        gAppendMessage("HBA mid level module may not be loaded.");
        return;
    }

    for (k = 0; k < num; ++k) {
        device_name = QString("%1/%2").arg(dev_bsg, namelist[k]->d_name);
        if (vb) {
            qDebug() << "----> exploring " << device_name;
        }

        // assign the IOC number for multiple adapters case
        res = smp_initiator_open(device_name, k, I_SGV4_MPI, &tobj, vb);
        if (res < 0) {
            continue;
        }

        for (int i = 0; i < 4; ++i) {
            // check if the next expander is to be discovered
            if (true == gControllers.bsgPath(i).isEmpty()) {
                // assign sas address for path-through
                tobj.sas_addr64 = gControllers.wwid64(i);
                if (0 != tobj.sas_addr64) {
                    if (vb) {
                        qDebug("   -> with SAS address=0x%lx", tobj.sas_addr64);
                    }
                    res = do_multiple_slot(&tobj, vb);
                    if (res) {
                        qDebug("Exit status %d indicates error detected", res);
                    }
                }
            }
        }

        smp_initiator_close(&tobj);
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}

/* Get adapter info command handler */
static int do_adpinfo(smp_target_obj * top, int vb)
{
    smp_req_resp smp_rr;

    memset(&(adpinfo[ioc_cnt]), 0, sizeof(adpinfo[ioc_cnt]));
    memset(&smp_rr, 0, sizeof(smp_rr));

    smp_rr.mpi3mr_function = DRVBSG_OPCODE + MPI3MR_DRVBSG_OPCODE_ADPINFO;
    smp_rr.max_response_l = sizeof(adpinfo[ioc_cnt]);
    smp_rr.response = (u8*) &(adpinfo[ioc_cnt]);

    int res = send_req_mpi3mr_bsg(top->fd, top->subvalue, 0, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != sizeof(adpinfo[ioc_cnt])) {
        qDebug("[send_req_mpi3mr_bsg] ioc_facts data length mismatch");
        return -1;
    }

    return 0;
}

/* Returns count of ioc collected, or -1 on failure */
static int do_iocfacts(smp_target_obj * top, int vb)
{
    struct mpi3_ioc_facts_data facts_data;
    smp_req_resp smp_rr;

    if (ioc_cnt >= NUM_IOC) {
        qDebug("[%s] ioc_cnt overflow!", __func__);
        return ioc_cnt;
    }

    memset(&facts_data, 0, sizeof(facts_data));
    memset(&smp_rr, 0, sizeof(smp_rr));

    smp_rr.mpi3mr_function = MPI3_FUNCTION_IOC_FACTS;
    smp_rr.max_response_l = sizeof(facts_data);
    smp_rr.response = (u8*) &facts_data;

    int res = send_req_mpi3mr_bsg(top->fd, top->subvalue, top->sas_addr64, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != sizeof(facts_data)) {
        qDebug("[send_req_mpi3mr_bsg] ioc_facts data length mismatch");
        return -1;
    }

    /**
     * process_factsdata - Process IOC facts data
     */
    ioc_facts[ioc_cnt].ioc_num = facts_data.ioc_number;
    ioc_facts[ioc_cnt].personality = (facts_data.flags & MPI3_IOCFACTS_FLAGS_PERSONALITY_MASK);
    ioc_facts[ioc_cnt].mpi_version = facts_data.mpi_version.word;
    ioc_facts[ioc_cnt].product_id = facts_data.product_id;
    ioc_facts[ioc_cnt].reply_sz = facts_data.reply_frame_size * 4;
    ioc_facts[ioc_cnt].max_sasexpanders = facts_data.max_sas_expanders;
    ioc_facts[ioc_cnt].max_enclosures = facts_data.max_enclosures;
    ioc_facts[ioc_cnt].max_data_length = facts_data.max_data_length;
    ioc_facts[ioc_cnt].fw_ver.build_num = facts_data.fw_version.build_num;
    ioc_facts[ioc_cnt].fw_ver.cust_id = facts_data.fw_version.customer_id;
    ioc_facts[ioc_cnt].fw_ver.ph_minor = facts_data.fw_version.phase_minor;
    ioc_facts[ioc_cnt].fw_ver.ph_major = facts_data.fw_version.phase_major;
    ioc_facts[ioc_cnt].fw_ver.gen_minor = facts_data.fw_version.gen_minor;
    ioc_facts[ioc_cnt].fw_ver.gen_major = facts_data.fw_version.gen_major;

    return ++ioc_cnt;
}

void mpi3mr_iocfacts(int vb)
{
    int num, k, res;
    struct dirent ** namelist;
    smp_target_obj tobj;
    QString device_name;

    if (vb)
        qDebug("MPI function: get IOC FACTS...");

    num = scandir(dev_bsg, &namelist, mpi3mrdev_scan_select, nullptr);
    if (num <= 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        gAppendMessage("HBA mid level module may not be loaded.");
        return;
    }

    ioc_cnt = 0;
    for (k = 0; k < num; ++k) {
        device_name = QString("%1/%2").arg(dev_bsg, namelist[k]->d_name);
        //gAppendMessage(device_name);
        if (vb) {
            qDebug() << "----> exploring " << device_name;
        }

        // assign the IOC number for multiple adapters case
        res = smp_initiator_open(device_name, k, I_SGV4_MPI, &tobj, vb);
        if (res < 0) {
            continue;
        }

        res = do_adpinfo(&tobj, vb);
        if (res < 0) {
            qDebug("Exit status %d indicates error detected", res);
        }
        res = do_iocfacts(&tobj, vb);
        if (res < 0) {
            qDebug("Exit status %d indicates error detected", res);
        }

        smp_initiator_close(&tobj);
    }

    for (k = 0; k < num; ++k) {
        free(namelist[k]);
    }
    free(namelist);
}

QString get_infofacts()
{
    QString s;

    for (int i = 0; i < ioc_cnt; i++) {
        s += QString::asprintf("%s (PCIAddr %02X:%02X:%02X.%02X), Driver Version: %s\n",
                adpinfo[i].driver_info.driver_name,
                adpinfo[i].pci_seg_id, adpinfo[i].pci_bus, adpinfo[i].pci_dev, adpinfo[i].pci_func,
                adpinfo[i].driver_info.driver_version);

        struct mpi3mr_compimg_ver *fwver = &ioc_facts[i].fw_ver;
        struct mpi3_version_struct *mpiver = (struct mpi3_version_struct *) &ioc_facts[i].mpi_version;
        s += QString::asprintf("HBA (%x), Firmware Version: %d.%d.%d.%d-%05d-%05d, MPI Version: %d.%d\n",
                ioc_facts[i].product_id,
                fwver->gen_major, fwver->gen_minor, fwver->ph_major, fwver->ph_minor,
                fwver->cust_id, fwver->build_num,
                mpiver->major, mpiver->minor);
    }
    return s;
}
