#ifndef LSSCSI_H
#define LSSCSI_H

#include "widget.h"

int compute_device_index(const char * device, const char * expander);
void list_sdevices(Widget*);

#endif // LSSCSI_H
