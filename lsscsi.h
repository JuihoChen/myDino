#ifndef LSSCSI_H
#define LSSCSI_H

#include "widget.h"

bool get_myValue(QString dir_name, QString name, QString& myvalue);
QString get_blockname(QString dir_name);
int compute_device_index(const char * device, const char * expander);
void list_sdevices(Widget*);

#endif // LSSCSI_H
