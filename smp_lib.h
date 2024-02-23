#ifndef SMP_LIB_H
#define SMP_LIB_H

#include <QString>

#ifdef __cplusplus
extern "C" {
#endif

/* SAS transport frame types associated with SMP */
#define SMP_FRAME_TYPE_REQ                  0x40
#define SMP_FRAME_TYPE_RESP                 0x41

/* SMP function codes */
#define SMP_FN_REPORT_GENERAL               0x0
#define SMP_FN_REPORT_MANUFACTURER          0x1
#define SMP_FN_READ_GPIO_REG                0x2
#define SMP_FN_REPORT_SELF_CONFIG           0x3
#define SMP_FN_REPORT_ZONE_PERMISSION_TBL   0x4
#define SMP_FN_REPORT_ZONE_MANAGER_PASS     0x5
#define SMP_FN_REPORT_BROADCAST             0x6
#define SMP_FN_READ_GPIO_REG_ENH            0x7
#define SMP_FN_DISCOVER                     0x10
#define SMP_FN_REPORT_PHY_ERR_LOG           0x11
#define SMP_FN_REPORT_PHY_SATA              0x12
#define SMP_FN_REPORT_ROUTE_INFO            0x13
#define SMP_FN_REPORT_PHY_EVENT             0x14
/* #define SMP_FN_REPORT_PHY_BROADCAST 0x15  removed in sas2r13 */
#define SMP_FN_DISCOVER_LIST                0x20  /* was 0x16 in sas2r10 */
#define SMP_FN_REPORT_PHY_EVENT_LIST        0x21
#define SMP_FN_REPORT_EXP_ROUTE_TBL_LIST    0x22  /* was 0x17 in sas2r10 */
#define SMP_FN_CONFIG_GENERAL               0x80
#define SMP_FN_ENABLE_DISABLE_ZONING        0x81
#define SMP_FN_WRITE_GPIO_REG               0x82
#define SMP_FN_WRITE_GPIO_REG_ENH           0x83
#define SMP_FN_ZONED_BROADCAST              0x85
#define SMP_FN_ZONE_LOCK                    0x86
#define SMP_FN_ZONE_ACTIVATE                0x87
#define SMP_FN_ZONE_UNLOCK                  0x88
#define SMP_FN_CONFIG_ZONE_MANAGER_PASS     0x89
#define SMP_FN_CONFIG_ZONE_PHY_INFO         0x8a
#define SMP_FN_CONFIG_ZONE_PERMISSION_TBL   0x8b
#define SMP_FN_CONFIG_ROUTE_INFO            0x90
#define SMP_FN_PHY_CONTROL                  0x91
#define SMP_FN_PHY_TEST_FUNCTION            0x92
#define SMP_FN_CONFIG_PHY_EVENT             0x93

/* SMP function result values */
#define SMP_FRES_FUNCTION_ACCEPTED          0x0
#define SMP_FRES_UNKNOWN_FUNCTION           0x1
#define SMP_FRES_FUNCTION_FAILED            0x2
#define SMP_FRES_INVALID_REQUEST_LEN        0x3
#define SMP_FRES_INVALID_EXP_CHANGE_COUNT   0x4
#define SMP_FRES_BUSY                       0x5
#define SMP_FRES_INCOMPLETE_DESCRIPTOR_LIST 0x6
#define SMP_FRES_NO_PHY                     0x10
#define SMP_FRES_NO_INDEX                   0x11
#define SMP_FRES_NO_SATA_SUPPORT            0x12
#define SMP_FRES_UNKNOWN_PHY_OP             0x13
#define SMP_FRES_UNKNOWN_PHY_TEST_FN        0x14
#define SMP_FRES_PHY_TEST_IN_PROGRESS       0x15
#define SMP_FRES_PHY_VACANT                 0x16
#define SMP_FRES_UNKNOWN_PHY_EVENT_SRC      0x17
#define SMP_FRES_UNKNOWN_DESCRIPTOR_TYPE    0x18
#define SMP_FRES_UNKNOWN_PHY_FILTER         0x19
#define SMP_FRES_AFFILIATION_VIOLATION      0x1a
#define SMP_FRES_SMP_ZONE_VIOLATION         0x20
#define SMP_FRES_NO_MANAGEMENT_ACCESS       0x21
#define SMP_FRES_UNKNOWN_EN_DIS_ZONING_VAL  0x22
#define SMP_FRES_ZONE_LOCK_VIOLATION        0x23
#define SMP_FRES_NOT_ACTIVATED              0x24
#define SMP_FRES_ZONE_GROUP_OUT_OF_RANGE    0x25
#define SMP_FRES_NO_PHYSICAL_PRESENCE       0x26
#define SMP_FRES_SAVING_NOT_SUPPORTED       0x27
#define SMP_FRES_SOURCE_ZONE_GROUP          0x28
#define SMP_FRES_DIS_PASSWORD_NOT_SUPPORTED 0x29
#define SMP_FRES_INVALID_FIELD_IN_REQUEST   0x2a

/* Utilities can use these process status values for syntax errors and
 * file (device node) problems (e.g. not found or permissions). Numbers
 * between 1 and 32 are reserved for SMP function result values
 */
#define SMP_LIB_SYNTAX_ERROR                91
#define SMP_LIB_FILE_ERROR                  92
#define SMP_LIB_RESOURCE_ERROR              93
#define SMP_LIB_CAT_MALFORMED               97

/* ioctl 20 seconds timout */
#define DEF_TIMEOUT_MS                      20000

typedef enum {
    I_MPT,
    I_SGV4,
    I_SGV4_MPI
} IntfEnum;

typedef struct _smp_target_obj {
    QString device_name;
    int subvalue;               /* adapter number (opt) */
    uint64_t sas_addr64;        /* target SMP (opt) */
    IntfEnum selector;
    int opened;
    int fd;
} smp_target_obj;

/* SAS standards include a 4 byte CRC at the end of each SMP request
   and response frames. All current pass-throughs calculate and check
   the CRC in the driver, but some pass-throughs want the space allocated.
 */
typedef struct _smp_req_resp {
    int request_len;            /* [i] in bytes, includes space for 4 byte CRC */
    unsigned char * request;    /* [*i], includes space for CRC */
    int max_response_l;         /* [i] in bytes, includes space for CRC */
    unsigned char * response;   /* [*o] */
    int act_response_l;         /* [o] -1 implies don't know */
    int transport_err;          /* [o] 0 implies no error */
    unsigned int mpi3mr_function;
} smp_req_resp;

extern const char * dev_bsg;
extern const char * dev_mpt;

/* Open device_name and if successful places context information in the object pointed
 * to by tobj . Returns 0 on success, else -1 . */
int smp_initiator_open(QString device_name, int subvalue, IntfEnum sel, smp_target_obj * tobj, int verbose);
/* Closes the context to the SMP target referred to by tobj. Returns 0
 * on success, else -1 . */
int smp_initiator_close(smp_target_obj * tobj);
/* The difference is the type of the first of
 * argument: uint8_t instead of char. The name of the argument is changed
 * to b_str to stress it is a pointer to the start of a binary string. */
void hex2stdout(void * str, int len, int no_ascii);

#ifdef __cplusplus
}
#endif

#endif // SMP_LIB_H
