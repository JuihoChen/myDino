#include <QWidget>

#include <dirent.h>
#include <linux/bsg.h>
#include <scsi/scsi_bsg_mpi3mr.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#include "mpi3mr.h"
#include "mpi3mr_app.h"
#include "smp_lib.h"
#include "widget.h"

struct mpi3mr_hba_sas_exp {
    uint64_t enclosure_logical_id;
    u16 sep_dev_handle;
    uint8_t scsi_io_reply[60];
    uint64_t sas_address;
    uint8_t rp_manufacturer[60];
};

#define NUM_IOC 4           /* Maximum number of Controllers is assumed to be 4 */
#define NUM_EXP_PER_HBA 4   /* Max number of SAS Expander per Controller (HBA) */

static int ioc_cnt;
static struct mpi3mr_bsg_in_adpinfo adpinfo[NUM_IOC];
static struct mpi3mr_ioc_facts ioc_facts[NUM_IOC];
static struct mpi3mr_hba_sas_exp hba_sas_exp[NUM_IOC][NUM_EXP_PER_HBA];

struct my_all_tgt_info {
    struct mpi3mr_all_tgt_info tgt_info;
    struct mpi3mr_device_map_info devmap_info[100];
};
static struct my_all_tgt_info alltgt_info[NUM_IOC];

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
    int m_requestLen;
    int m_replyLen;

private:
    int m3r_add_buf_entry(__u8 buf_type, __u32 buf_len);
    int m3r_mpt_buf_len() {
        return (long) & mbp.cmd.mptcmd.buf_entry_list.buf_entry[m_ecnt] - (long) & mbp;
    }
    int m3r_populate_adpinfo();
    int m3r_get_all_tgt_info();
    int m3r_issue_iocfacts();
    int m3r_process_cfg_req();
    int m3r_request_scsi_io();
    int m3r_smp_passthrough();

private:
    int m_ecnt;
    int m_iocId;
    int64_t m_sasAddress;
    smp_req_resp * m_rresp;
};

mpi3_request::mpi3_request(int subvalue, int64_t target_sa, smp_req_resp * rr)
{
    memset(mbp_pool, 0, sizeof(mbp_pool));
    memset(request_m, 0, sizeof(request_m));
    memset(reply_m, 0, sizeof(reply_m));

    m_ecnt = 0;
    m_iocId = subvalue;
    m_sasAddress = target_sa;
    m_rresp = rr;
    m_requestLen = 0;
    m_replyLen = 0;
}

int mpi3_request::fill_request()
{
    int noFunc = m_rresp->mpi3mr_function;

    switch (noFunc) {
    case MPI3_FUNCTION_IOC_FACTS:
        return m3r_issue_iocfacts();
    case MPI3_FUNCTION_CONFIG:
        return m3r_process_cfg_req();
    case MPI3_FUNCTION_SCSI_IO:
        return m3r_request_scsi_io();
    case MPI3_FUNCTION_SMP_PASSTHROUGH:
        return m3r_smp_passthrough();
    default:
        switch (noFunc - DRVBSG_OPCODE) {
        case MPI3MR_DRVBSG_OPCODE_ADPINFO:
            return m3r_populate_adpinfo();
        case MPI3MR_DRVBSG_OPCODE_ALLTGTDEVINFO:
            return m3r_get_all_tgt_info();
        }
        qDebug("%s: illegal function id=0x%x", __func__, noFunc);
        break;
    }
    return -1;
}

int mpi3_request::response_len(void * mpi_reply)
{
    int noFunc = m_rresp->mpi3mr_function;

    switch (noFunc) {
    case MPI3_FUNCTION_IOC_FACTS:
        return ((struct mpi3_ioc_facts_data *)reply_m)->ioc_facts_data_length * 4;
    case MPI3_FUNCTION_CONFIG:
    case MPI3_FUNCTION_SCSI_IO:
        return m_rresp->max_response_l;
    case MPI3_FUNCTION_SMP_PASSTHROUGH:
        return ((struct mpi3_smp_passthrough_reply *)mpi_reply)->response_data_length;
    default:
        switch (noFunc - DRVBSG_OPCODE) {
        case MPI3MR_DRVBSG_OPCODE_ADPINFO:
        case MPI3MR_DRVBSG_OPCODE_ALLTGTDEVINFO:
            return m_rresp->max_response_l;
        }
        break;
    }
    return 0;
}

int mpi3_request::m3r_add_buf_entry(__u8 buf_type, __u32 buf_len)
{
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[m_ecnt].buf_type = buf_type;
    mbp.cmd.mptcmd.buf_entry_list.buf_entry[m_ecnt].buf_len = buf_len;

    return ++m_ecnt;
}

int mpi3_request::m3r_populate_adpinfo()
{
    /* This is to process a HBA Driver Command, not a message passthrough
     * (There is no data going out downward to HBA/FW)
     *
     *  hdr.dout_xfer_len (rresp->request_len + m_requestLen) must be zero to NOT support BIDI
     */
    mbp.cmd_type = MPI3MR_DRV_CMD;
    mbp.cmd.drvrcmd.mrioc_id = m_iocId;     // Set the IOC number prior to issuing this command.
    mbp.cmd.drvrcmd.opcode = MPI3MR_DRVBSG_OPCODE_ADPINFO;

    return sizeof(mbp);
}

int mpi3_request::m3r_get_all_tgt_info()
{
    /* This is to process a HBA Driver Command, not a message passthrough
     * (There is no data going out downward to HBA/FW)
     *
     *  hdr.dout_xfer_len (rresp->request_len + m_requestLen) must be zero to NOT support BIDI
     */
    mbp.cmd_type = MPI3MR_DRV_CMD;
    mbp.cmd.drvrcmd.mrioc_id = m_iocId;     // Set the IOC number prior to issuing this command.
    mbp.cmd.drvrcmd.opcode = MPI3MR_DRVBSG_OPCODE_ALLTGTDEVINFO;

    return sizeof(mbp);
}

int mpi3_request::m3r_issue_iocfacts()
{
    struct mpi3_ioc_facts_request * mpi_request;
    struct dummy_reply { char dummy[21]; };

    m_requestLen = sizeof(struct mpi3_ioc_facts_request);
    m_replyLen = sizeof(struct dummy_reply);

    mpi_request = (struct mpi3_ioc_facts_request *) request_m;
    mpi_request->function = m_rresp->mpi3mr_function;

    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_DATA_IN, m_rresp->max_response_l);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REPLY, m_replyLen);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REQUEST, m_requestLen);

    mbp.cmd_type = MPI3MR_MPT_CMD;
    mbp.cmd.mptcmd.timeout = 180;           // ?a hacked value
    mbp.cmd.mptcmd.mrioc_id = m_iocId;      // Set the IOC number prior to issuing this command.
    mbp.cmd.mptcmd.buf_entry_list.num_of_entries = m_ecnt;

    return m3r_mpt_buf_len();
}

int mpi3_request::m3r_process_cfg_req()
{
    struct mpi3_config_request * mpi_request;
    struct dummy_reply { char dummy[21]; };

    m_requestLen = sizeof(struct mpi3_config_request);
    m_replyLen = sizeof(struct dummy_reply);

    mpi_request = (struct mpi3_config_request *) request_m;
    /*
     * !!! Copy to request_m is a must to prevent driver crash
     */
    if (nullptr == m_rresp->mpi3mr_object) {
        qDebug("%s: no valid config request to process!", __func__);
        return 0;
    }
    *mpi_request = *(struct mpi3_config_request *) m_rresp->mpi3mr_object;

    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_DATA_IN, m_rresp->max_response_l);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REPLY, m_replyLen);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REQUEST, m_requestLen);

    mbp.cmd_type = MPI3MR_MPT_CMD;
    mbp.cmd.mptcmd.timeout = 180;           // ?a hacked value
    mbp.cmd.mptcmd.mrioc_id = m_iocId;      // Set the IOC number prior to issuing this command.
    mbp.cmd.mptcmd.buf_entry_list.num_of_entries = m_ecnt;

    return m3r_mpt_buf_len();
}

int mpi3_request::m3r_request_scsi_io()
{
    struct mpi3_scsi_io_request * mpi_request;
    struct dummy_reply { char dummy[61]; };
    struct err_response { char dummy[252]; };

    m_requestLen = sizeof(struct mpi3_scsi_io_request);
    m_replyLen = sizeof(struct dummy_reply) + sizeof(struct err_response);

    mpi_request = (struct mpi3_scsi_io_request *) request_m;
    /*
     * !!! Copy to request_m is a must to prevent driver crash
     */
    if (nullptr == m_rresp->mpi3mr_object) {
        qDebug("%s: no valid config request to process!", __func__);
        return 0;
    }
    *mpi_request = *(struct mpi3_scsi_io_request *) m_rresp->mpi3mr_object;

    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_DATA_IN, m_rresp->max_response_l);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REPLY, sizeof(struct dummy_reply));
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_ERR_RESPONSE, sizeof(struct err_response));
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REQUEST, m_requestLen);

    mbp.cmd_type = MPI3MR_MPT_CMD;
    mbp.cmd.mptcmd.timeout = 180;           // ?a hacked value
    mbp.cmd.mptcmd.mrioc_id = m_iocId;      // Set the IOC number prior to issuing this command.
    mbp.cmd.mptcmd.buf_entry_list.num_of_entries = m_ecnt;

    return m3r_mpt_buf_len();
}

int mpi3_request::m3r_smp_passthrough()
{
    struct mpi3_smp_passthrough_request * mpi_request;

    m_requestLen = sizeof(struct mpi3_smp_passthrough_request);
    m_replyLen = sizeof(struct mpi3_smp_passthrough_reply);

    memcpy(request_m, m_rresp->request, m_rresp->request_len);
    mpi_request = (struct mpi3_smp_passthrough_request *)(request_m + m_rresp->request_len);
    mpi_request->function = m_rresp->mpi3mr_function;
    mpi_request->io_unit_port = 0xFF;       // ?invalid port number (rphy)
    mpi_request->sas_address = m_sasAddress;

    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_DATA_OUT, m_rresp->request_len);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_DATA_IN, m_rresp->max_response_l);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REPLY, m_replyLen);
    m3r_add_buf_entry(MPI3MR_BSG_BUFTYPE_MPI_REQUEST, m_requestLen);

    mbp.cmd_type = MPI3MR_MPT_CMD;
    mbp.cmd.mptcmd.timeout = 180;           // ?a hacked value
    mbp.cmd.mptcmd.mrioc_id = m_iocId;      // Set the IOC number prior to issuing this command.
    mbp.cmd.mptcmd.buf_entry_list.num_of_entries = m_ecnt;

    return m3r_mpt_buf_len();
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

    hdr.dout_xfer_len = rresp->request_len + mpi3rq.m_requestLen;
    hdr.dout_xferp = (uintptr_t) request_m;

    hdr.din_xfer_len = rresp->max_response_l + mpi3rq.m_replyLen;
    hdr.din_xferp = (uintptr_t) reply_m;

    hdr.timeout = DEF_TIMEOUT_MS;

    if (vb > 1) {
        qDebug("%s: dout_xfer_len=%u, din_xfer_len=%u, timeout=%u ms",
               __func__, hdr.dout_xfer_len, hdr.din_xfer_len, hdr.timeout);
    }

    int res = ioctl(fd, SG_IO, &hdr);
    if (res) {
        perror("send_req_mpi3mr_bsg: SG_IO ioctl");
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
        hex2stdout(mpi_reply, mpi3rq.m_replyLen, 1);
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

    num = scandir(dev_bsg, &namelist, mpi3mrdev_scan_select, alphasort);
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
        res = smp_initiator_open(device_name, I_SGV4_MPI, &tobj, vb);
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
    int res;
    smp_target_obj tobj;
    /**
     * Explore HBA and expander firstly
     */
    mpi3mr_iocfacts(vb);
    /**
     * Then, explore slots as usual
     */
    if (vb)
        qDebug("Slot discovering...");

    for (int k = 0; k < NEXPDR; ++k) {
        if (false == gControllers.bsgPath(k).isEmpty()) {
            // assign the IOC number for multiple adapters case
            res = smp_initiator_open(gControllers.bsgPath(k), I_SGV4_MPI, &tobj, vb);
            if (res < 0) {
                qDebug() << "Failed to open driver " << gControllers.bsgPath(k);
                continue;
            }
            // assign sas address for path-through
            tobj.sas_addr64 = gControllers.wwid64(k);
            if (vb) {
                qDebug("----> exploring SAS address=0x%lx", tobj.sas_addr64);
            }
            res = do_multiple_slot(&tobj, vb);
            if (res) {
                qDebug("Exit status %d indicates error detected", res);
            }
            smp_initiator_close(&tobj);
        }
    }
}

/* Get adapter info command handler */
static int populate_adpinfo(smp_target_obj * top, int vb)
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
        qDebug("[send_req_mpi3mr_bsg] adpinfo data length mismatch");
        return -1;
    }

    return 0;
}

/* Get all target information */
static int get_all_tgt_info(smp_target_obj * top, int vb)
{
    smp_req_resp smp_rr;

    memset(&(alltgt_info[ioc_cnt]), 0, sizeof(alltgt_info[ioc_cnt]));
    memset(&smp_rr, 0, sizeof(smp_rr));

    smp_rr.mpi3mr_function = DRVBSG_OPCODE + MPI3MR_DRVBSG_OPCODE_ALLTGTDEVINFO;
    smp_rr.max_response_l = sizeof(alltgt_info[ioc_cnt]);
    smp_rr.response = (u8*) &(alltgt_info[ioc_cnt]);

    int res = send_req_mpi3mr_bsg(top->fd, top->subvalue, 0, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != sizeof(alltgt_info[ioc_cnt])) {
        qDebug("[send_req_mpi3mr_bsg] alltgt_info data length mismatch");
        return -1;
    }

    return 0;
}

/* Return: 0 on success, non-zero on failure. */
static int issue_iocfacts(smp_target_obj * top, int vb)
{
    struct mpi3_ioc_facts_data facts_data;
    smp_req_resp smp_rr;

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

    return 0;
}

/**
 *  @form: The form to be used for addressing the page
 *
 *  Return: 0 on success, non-zero on failure.
 */
static int cfg_get_enclosure_pg0(smp_target_obj * top, struct mpi3_enclosure_page0 & encl_pg0, u32 form, int vb)
{
    struct mpi3_config_page_header cfg_hdr;
    struct mpi3_config_request cfg_req;
    smp_req_resp smp_rr;

    memset(&encl_pg0, 0, sizeof(encl_pg0));
    memset(&cfg_hdr, 0, sizeof(cfg_hdr));
    memset(&cfg_req, 0, sizeof(cfg_req));
    memset(&smp_rr, 0, sizeof(smp_rr));

    cfg_req.function = MPI3_FUNCTION_CONFIG;
    cfg_req.action = MPI3_CONFIG_ACTION_PAGE_HEADER;
    cfg_req.page_type = MPI3_CONFIG_PAGETYPE_ENCLOSURE;
    cfg_req.page_number = 0;
    cfg_req.page_address = form;   // 0xffff ?a hacked value (not to config???)
    cfg_req.page_length = 0;    // page length == 0 to get page header??

    smp_rr.mpi3mr_function = MPI3_FUNCTION_CONFIG;
    smp_rr.mpi3mr_object = (void*) &cfg_req;
    smp_rr.max_response_l = sizeof(cfg_hdr);
    smp_rr.response = (u8*) &cfg_hdr;

    int res = send_req_mpi3mr_bsg(top->fd, top->subvalue, top->sas_addr64, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] Enclosure page0 header read failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != sizeof(cfg_hdr)) {
        qDebug("[send_req_mpi3mr_bsg] cfg_hdr data length mismatch");
        return -1;
    }

    cfg_req.action = MPI3_CONFIG_ACTION_READ_CURRENT;
    cfg_req.page_length = sizeof(encl_pg0);

    smp_rr.mpi3mr_object = (void*) &cfg_req;
    smp_rr.max_response_l = sizeof(encl_pg0);
    smp_rr.response = (u8*) &encl_pg0;

    res = send_req_mpi3mr_bsg(top->fd, top->subvalue, top->sas_addr64, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] Enclosure page0 read failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != sizeof(encl_pg0)) {
        qDebug("[send_req_mpi3mr_bsg] encl_pg0 data length mismatch");
        return -1;
    }

    /**
     * exp_pg0.dev_handle turns to be the handle (form's value) of the next sas expander page0 request
     */
    if (vb) {
        qDebug("found enclosure logical id=%llx with enclosure_handle=0x%04x, sep_dev_handle=0x%04x",
            encl_pg0.enclosure_logical_id, encl_pg0.enclosure_handle, encl_pg0.sep_dev_handle);
    }

    return 0;
}

/**
 *  @form: The form to be used for addressing the page
 *
 *  Return: 0 on success, non-zero on failure.
 */
static int cfg_get_sas_exp_pg0(smp_target_obj * top, struct mpi3_sas_expander_page0 & exp_pg0, u32 form, int vb)
{
    struct mpi3_config_page_header cfg_hdr;
    struct mpi3_config_request cfg_req;
    smp_req_resp smp_rr;

    memset(&exp_pg0, 0, sizeof(exp_pg0));
    memset(&cfg_hdr, 0, sizeof(cfg_hdr));
    memset(&cfg_req, 0, sizeof(cfg_req));
    memset(&smp_rr, 0, sizeof(smp_rr));

    cfg_req.function = MPI3_FUNCTION_CONFIG;
    cfg_req.action = MPI3_CONFIG_ACTION_PAGE_HEADER;
    cfg_req.page_type = MPI3_CONFIG_PAGETYPE_SAS_EXPANDER;
    cfg_req.page_number = 0;
    cfg_req.page_address = form;   // 0xffff ?a hacked value (not to config???)
    cfg_req.page_length = 0;    // page length == 0 to get page header??

    smp_rr.mpi3mr_function = MPI3_FUNCTION_CONFIG;
    smp_rr.mpi3mr_object = (void*) &cfg_req;
    smp_rr.max_response_l = sizeof(cfg_hdr);
    smp_rr.response = (u8*) &cfg_hdr;

    int res = send_req_mpi3mr_bsg(top->fd, top->subvalue, top->sas_addr64, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] SAS Expander page0 header read failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != sizeof(cfg_hdr)) {
        qDebug("[send_req_mpi3mr_bsg] cfg_hdr data length mismatch");
        return -1;
    }

    cfg_req.action = MPI3_CONFIG_ACTION_READ_CURRENT;
    cfg_req.page_length = sizeof(exp_pg0);

    smp_rr.mpi3mr_object = (void*) &cfg_req;
    smp_rr.max_response_l = sizeof(exp_pg0);
    smp_rr.response = (u8*) &exp_pg0;

    res = send_req_mpi3mr_bsg(top->fd, top->subvalue, top->sas_addr64, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] SAS Expander page0 read failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != sizeof(exp_pg0)) {
        qDebug("[send_req_mpi3mr_bsg] exp_pg0 data length mismatch");
        return -1;
    }

    /**
     * exp_pg0.dev_handle turns to be the handle (form's value) of the next sas expander page0 request
     */
    if (vb) {
        qDebug("found sas expander wwid=%llx with dev_handle=0x%04x", exp_pg0.sas_address, exp_pg0.dev_handle);
    }

    return 0;
}

/**
 *  Issues the SCSI Command as an MPI3 request.
 *
 *  Return: 0 on success, non-zero on failure.
 */
#define MODE 1
#define BUFFER_ID 0xe6

static int mpi3mr_qcmd(smp_target_obj * top, u16 handle, uint8_t * rp, int rp_len, int vb)
{
    uint8_t scmd[] = {READ_BUFFER, MODE, BUFFER_ID, 0xff, 0x0f, 0x00, (uint8_t)rp_len, (uint8_t)(rp_len>>8), 0};
    struct mpi3_scsi_io_request scsiio_req;
    smp_req_resp smp_rr;

    if (vb) {
        QString msg = "    SCSI IO request: ";
        for (int k = 0; k < (int)sizeof(scmd); ++k)
            msg += QString::asprintf("%02x ", scmd[k]);
        qDebug() << msg;
    }
    memset(&scsiio_req, 0, sizeof(scsiio_req));
    memset(&smp_rr, 0, sizeof(smp_rr));

    scsiio_req.function = MPI3_FUNCTION_SCSI_IO;
    scsiio_req.dev_handle = handle;
    scsiio_req.flags = MPI3_SCSIIO_FLAGS_DATADIRECTION_READ;
    scsiio_req.data_length = rp_len;
    memcpy(scsiio_req.cdb.cdb32, scmd, sizeof(scmd));

    smp_rr.mpi3mr_function = MPI3_FUNCTION_SCSI_IO;
    smp_rr.mpi3mr_object = (void*) &scsiio_req;
    smp_rr.max_response_l = rp_len;
    smp_rr.response = rp;

    int res = send_req_mpi3mr_bsg(top->fd, top->subvalue, top->sas_addr64, &smp_rr, vb);
    if (res) {
        qDebug("[send_req_mpi3mr_bsg] SCSI Command failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("[send_req_mpi3mr_bsg] transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    if (smp_rr.act_response_l != rp_len) {
        qDebug("[send_req_mpi3mr_bsg] scsiio_reply data length mismatch");
        return -1;
    }

    return 0;
}

static int smp_report_manufacturer(smp_target_obj * top, uint8_t * rp, int rp_len, int vb)
{
    uint8_t smp_req[] = {SMP_FRAME_TYPE_REQ, SMP_FN_REPORT_MANUFACTURER, 0, 0};
    smp_req_resp smp_rr;

    if (vb) {
        QString msg = "    Report manufacturer request: ";
        for (int k = 0; k < (int)sizeof(smp_req); ++k)
            msg += QString::asprintf("%02x ", smp_req[k]);
        qDebug() << msg;
    }
    memset(&smp_rr, 0, sizeof(smp_rr));
    smp_rr.mpi3mr_function = MPI3_FUNCTION_SMP_PASSTHROUGH;
    smp_rr.request = smp_req;
    smp_rr.request_len = sizeof(smp_req);
    smp_rr.max_response_l = rp_len;
    smp_rr.response = rp;

    int res = send_req_mpi3mr_bsg(top->fd, top->subvalue, top->sas_addr64, &smp_rr, vb);
    if (res) {
        qDebug("RM send_req_mpi3mr_bsg failed, res=%d", res);
        return -1;
    }
    if (smp_rr.transport_err) {
        qDebug("RM send_req_mpi3mr_bsg transport_error=%d", smp_rr.transport_err);
        return -1;
    }
    int act_resplen = smp_rr.act_response_l;
    if ((act_resplen >= 0) && (act_resplen < 4)) {
        qDebug("RM response too short, len=%d", act_resplen);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    int len = rp[3];
    if ((0 == len) && (0 == rp[2])) {
        len = smp_get_func_def_resp_len(rp[1]);
        if (len < 0) {
            len = 0;
            if (vb)
                qDebug("unable to determine RM response length");
        }
    }
    len = 4 + (len * 4);        /* length in bytes, excluding 4 byte CRC */
    if ((act_resplen >= 0) && (len > act_resplen)) {
        if (vb)
            qDebug("actual RM response length [%d] less than deduced length [%d]", act_resplen, len);
    }
    /* ignore --hex and --raw */
    if (SMP_FRAME_TYPE_RESP != rp[0]) {
        qDebug("RM expected SMP frame response type, got=0x%x", rp[0]);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    if (rp[1] != smp_req[1]) {
        qDebug("RM Expected function code=0x%x, got=0x%x", smp_req[1], rp[1]);
        return -4 - SMP_LIB_CAT_MALFORMED;
    }
    if (rp[2]) {
        if (vb) {
            char b[256];
            char * cp = smp_get_func_res_str(rp[2], sizeof(b), b);
            qDebug("Report Manufacturer result: %s", cp);
        }
        return -4 - rp[2];
    }

    if (vb) {
        qDebug("FwVersion 0%c.0%c.0%c.0%c", rp[36], rp[37], rp[38], rp[39]);
    }

    return 0;
}

void mpi3mr_iocfacts(int vb)
{
    int num, k, res, len;
    u16 handle;
    struct dirent ** namelist;
    smp_target_obj tobj;
    QString device_name;

    if (vb) {
        qDebug("MPI function: get IOC FACTS...");
    }
    /**
     * Clear information before query on HBA
     */
    memset(adpinfo, 0, sizeof(adpinfo));
    memset(ioc_facts, 0, sizeof(ioc_facts));
    memset(hba_sas_exp, 0, sizeof(hba_sas_exp));
    memset(alltgt_info, 0, sizeof(alltgt_info));

    num = scandir(dev_bsg, &namelist, mpi3mrdev_scan_select, alphasort);
    if (num <= 0) {  /* HBA mid level may not be loaded */
        perror("scandir");
        gAppendMessage("HBA mid level module may not be loaded.");
        return;
    }

    ioc_cnt = 0;
    for (k = 0; k < num; ++k) {
        if (ioc_cnt >= NUM_IOC) {
            qDebug("[%s] ioc_cnt overflow!", __func__);
            break;
        }

        device_name = QString("%1/%2").arg(dev_bsg, namelist[k]->d_name);
        //gAppendMessage(device_name);
        if (vb) {
            qDebug() << "----> exploring " << device_name;
        }

        // assign the IOC number for multiple adapters case
        res = smp_initiator_open(device_name, I_SGV4_MPI, &tobj, vb);
        if (res < 0) {
            continue;
        }

        res = populate_adpinfo(&tobj, vb);
        if (res < 0) {
            qDebug("Exit status %d indicates error detected", res);
        }
#if 0  /// remark due to being unused
        res = get_all_tgt_info(&tobj, vb);
        if (res < 0) {
            qDebug("Exit status %d indicates error detected", res);
        }
#endif
        res = issue_iocfacts(&tobj, vb);
        if (res < 0) {
            qDebug("Exit status %d indicates error detected", res);
        }
        /**
         * By hacking, 'form' value gets started with a value 0xffff
         */
        handle = 0xffff;
        struct mpi3_enclosure_page0 encl_pg0;
        len = sizeof(hba_sas_exp[0][0].scsi_io_reply);
        int iexp = 0;
        /**
         * Add one more loop for 1st try on logical id of hba??
         */
        for (int e = 0; e < NUM_EXP_PER_HBA+1; ++e) {
            res = cfg_get_enclosure_pg0(&tobj, encl_pg0, handle, vb);
            if (res < 0) {
                qDebug("Exit status %d indicates error detected", res);
                break;
            }
            // stop exploring if enclosure logical id becomes 0
            if (encl_pg0.enclosure_logical_id == 0) {
                break;
            }
            /**
             * Ok, got one more enclosure attached
             *
             * save SEP(SCSI Enclosure Processor) device handle if !0
             */
            if (encl_pg0.sep_dev_handle != 0 && iexp < NUM_EXP_PER_HBA)
            {
                /**
                 * The first entry is not an expander enclosure logical id (seems hba's)
                 */
                hba_sas_exp[ioc_cnt][iexp].enclosure_logical_id = encl_pg0.enclosure_logical_id;
                hba_sas_exp[ioc_cnt][iexp].sep_dev_handle = encl_pg0.sep_dev_handle;
                res = mpi3mr_qcmd(&tobj, encl_pg0.sep_dev_handle, hba_sas_exp[ioc_cnt][iexp].scsi_io_reply, len, vb);
                if (res < 0) {
                    qDebug("Exit status %d indicates error detected", res);
                }
                iexp++;
            }
            /**
             * By hacking, new 'form' value is derived from encl_pg0.enclosure_handle
             */
            handle = encl_pg0.enclosure_handle;
        }
        /**
         * By hacking, 'form' value gets started with a value 0xffff
         */
        handle = 0xffff;
        struct mpi3_sas_expander_page0 exp_pg0;
        len = sizeof(hba_sas_exp[0][0].rp_manufacturer);
        for (int e = 0; e < NUM_EXP_PER_HBA; ++e) {
            res = cfg_get_sas_exp_pg0(&tobj, exp_pg0, handle, vb);
            if (res < 0) {
                qDebug("Exit status %d indicates error detected", res);
                break;
            }
            // stop exploring if sas address becomes 0
            if (exp_pg0.sas_address == 0) {
                break;
            }
            // ok, got one more sas expander attached
            tobj.sas_addr64 = hba_sas_exp[ioc_cnt][e].sas_address = exp_pg0.sas_address;
            res = smp_report_manufacturer(&tobj, hba_sas_exp[ioc_cnt][e].rp_manufacturer, len, vb);
            if (res < 0) {
                qDebug("Exit status %d indicates error detected", res);
            }
            /**
             * Register the BSG path in the expander just explored
             */
            gControllers.setBsgPath(tobj.device_name, exp_pg0.sas_address);
            /**
             * By hacking, new 'form' value is derived from exp_pg0.dev_handle
             */
            handle = exp_pg0.dev_handle;
        }

        smp_initiator_close(&tobj);

        // Add 1 to Number of Controllers
        ++ioc_cnt;
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
        if (0 == i) {
            s += QString::asprintf("%s, Driver Version: %s\n",
                    adpinfo[i].driver_info.driver_name, adpinfo[i].driver_info.driver_version);
        }

        struct mpi3mr_compimg_ver *fwver = &ioc_facts[i].fw_ver;
        struct mpi3_version_struct *mpiver = (struct mpi3_version_struct *) &ioc_facts[i].mpi_version;
        s += QString::asprintf("HBA (PCIAddr %02X:%02X:%02X.%02X), Firmware Version: %d.%d.%d.%d-%05d-%05d, MPI Version: %d.%d\n",
                adpinfo[i].pci_seg_id, adpinfo[i].pci_bus, adpinfo[i].pci_dev, adpinfo[i].pci_func,
                fwver->gen_major, fwver->gen_minor, fwver->ph_major, fwver->ph_minor, fwver->cust_id, fwver->build_num,
                mpiver->major, mpiver->minor);

        for (int e = 0; e < NUM_EXP_PER_HBA; e += 2) {
            if (0 != hba_sas_exp[i][e].sas_address) {
                s += QString::asprintf("ELI (%lX) FW: 0%c.0%c.0%c.0%c MFG: %02X:%02X",
                        hba_sas_exp[i][e].enclosure_logical_id,
                        hba_sas_exp[i][e].rp_manufacturer[36], hba_sas_exp[i][e].rp_manufacturer[37],
                        hba_sas_exp[i][e].rp_manufacturer[38], hba_sas_exp[i][e].rp_manufacturer[39],
                        hba_sas_exp[i][e].scsi_io_reply[12], hba_sas_exp[i][e].scsi_io_reply[13]);
                /* The right part outputs or not depending on the left part */
                if (0 != hba_sas_exp[i][e+1].sas_address) {
                    s += QString::asprintf(",  ELI (%lX) FW: 0%c.0%c.0%c.0%c MFG: %02X:%02X\n",
                            hba_sas_exp[i][e+1].enclosure_logical_id,
                            hba_sas_exp[i][e+1].rp_manufacturer[36], hba_sas_exp[i][e+1].rp_manufacturer[37],
                            hba_sas_exp[i][e+1].rp_manufacturer[38], hba_sas_exp[i][e+1].rp_manufacturer[39],
                            hba_sas_exp[i][e+1].scsi_io_reply[12], hba_sas_exp[i][e+1].scsi_io_reply[13]);
                } else s += "\n";
            }
        }
    }
    /**
     * Remove '\n' character from the end of the string.
     */
    if (false == s.isEmpty()) {
        s.chop(1);
    }
    return s;
}
