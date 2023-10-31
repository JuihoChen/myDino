#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QCheckBox>

#define NSLOT   112

extern QCheckBox *wwn[NSLOT];

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void appendMessage(QString message);

private:
    Ui::Widget *ui;
};
#endif // WIDGET_H
