#include "ConsumerWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("MseedConsumer");
    app.setApplicationVersion("1.0.0");

    ConsumerWindow window;
    window.show();
    return app.exec();
}
