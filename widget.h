#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QCheckBox>
#include <QGroupBox>

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class DeviceFunc;
class ExpanderFunc;

extern DeviceFunc gDevices;
extern ExpanderFunc gControllers;

typedef struct
{
    QCheckBox *cb_slot;
    QString d_name;
    QString wwid;
    QString block;
} _ST_SLOTINFO;

class DeviceFunc
{
public:
    DeviceFunc(_ST_SLOTINFO * gs) : pSlotInfo(gs)
    {}
    ~DeviceFunc()
    {}

    void clear();
    void setSlot(QString path, QString device, QString expander, int iexp);
    int count() { return myCount; }

private:
    _ST_SLOTINFO *pSlotInfo;
    int myCount;
};

typedef struct
{
    QGroupBox *gbox;
    QString d_name;
    QString wwid;
} _ST_GBOXINFO;

class ExpanderFunc
{
public:
    ExpanderFunc(_ST_GBOXINFO * ge) : pGboxInfo(ge)
    {}
    ~ExpanderFunc()
    {}

    void clear();
    void setController(QString path, QString expander, int iexp);
    int count() { return myCount; }

private:
    _ST_GBOXINFO *pGboxInfo;
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
    void on_pushButton_clicked();
    void on_comboBox_currentIndexChanged(int index);

private:
    void refreshSlots();

    Ui::Widget *ui;
};

#endif // WIDGET_H
