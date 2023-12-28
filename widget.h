#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QCheckBox>
#include <QGroupBox>

#include "smp_discover.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

#define NSLOT   112
#define NEXPDR  4

typedef struct ST_SLOTINFO {
    QCheckBox *cb_slot;
    QString d_name;
    QString wwid;
    QString block;
    uchar discover_resp[SMP_FN_DISCOVER_RESP_LEN];
    int resp_len;
} _ST_SLOTINFO;

class DeviceFunc
{
public:
    DeviceFunc() {}
    ~DeviceFunc() {};

    void clear();
    void setSlot(QString path, QString device, QString expander, int iexp);
    void setSlot(QString path, QString device, QString enclosure_device_name);
    void setDiscoverResp(int dsn, uchar * src, int len);
    void setSlotLabel(int sl);
    int count() { return myCount; }

    QCheckBox *& cbSlot(int sl) { return SlotInfo[sl].cb_slot; }

private:
    void setSlot(QString path, QString device, int sl);

    _ST_SLOTINFO SlotInfo[NSLOT];
    int myCount;
};

typedef struct ST_GBOXINFO {
    QGroupBox *gbox;
    QString d_name;
    QString wwid;
    uchar discover_resp[SMP_FN_DISCOVER_RESP_LEN];
    int resp_len;
} _ST_GBOXINFO;

class ExpanderFunc
{
public:
    ExpanderFunc() {}
    ~ExpanderFunc() {}

    void clear();
    void setController(QString path, QString expander, int iexp);
    void setDiscoverResp(uint64_t ull, uint64_t sa, uchar * src, int len);
    int count() { return myCount; }

    QGroupBox *& gbThe(int gr) { return GboxInfo[gr].gbox; }

private:
    _ST_GBOXINFO GboxInfo[NEXPDR];
    int myCount;
};

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void appendMessage(QString message);

private slots:
    void btnRefreshClicked();
    void cbxSlotIndexChanged(int index);
    void btnSmpDoitClicked();
    void btnClearTBClicked();

private:
    void refreshSlots();

    Ui::Widget *ui;
};

extern DeviceFunc gDevices;
extern ExpanderFunc gControllers;

void gAppendMessage(QString message);

#endif // WIDGET_H
