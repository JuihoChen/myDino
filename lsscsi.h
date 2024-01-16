#ifndef LSSCSI_H
#define LSSCSI_H

#include <QWidget>

extern bool hba9500;

bool get_myValue(QString dir_name, QString name, QString& myvalue);
QString get_blockname(QString dir_name);
int compute_device_index(const char * device, const char * expander);
void list_sdevices(int verbose);

#endif // LSSCSI_H
