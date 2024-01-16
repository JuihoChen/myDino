#ifndef MPI3MR_APP_H
#define MPI3MR_APP_H

#include "smp_lib.h"

int send_req_mpi3mr_bsg(int fd, int64_t target_sa, smp_req_resp * rresp, int verbose);
void mpi3mr_discover(int verbose);
void mpi3mr_slot_discover(int verbose);

#endif // MPI3MR_APP_H
