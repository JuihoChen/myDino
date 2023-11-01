#include "widget.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    Widget w;
    w.setWindowTitle("myDino");
    w.show();

    w.appendMessage("Here lists the messages:");

    return a.exec();
}
