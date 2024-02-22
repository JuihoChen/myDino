#include <QApplication>
#include <getopt.h>

#include "widget.h"

int verbose = 0;

static struct option long_options[] = {
    { "verbose", no_argument, 0, 'v' },
    };

int main(int argc, char *argv[])
{
    int c;
    while((c = getopt_long(argc, argv, "v", long_options, NULL)) != -1) {
        switch(c) {
        case 'v':
            ++verbose;
            break;
        }
    }

    QApplication a(argc, argv);
    Widget w;
    w.setWindowTitle("myDino [0.08]");
    w.show();

    return a.exec();
}
