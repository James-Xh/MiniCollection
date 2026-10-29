#include "ConsumerWindow.h"
#include "UiStyle.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("MseedConsumer");
    app.setApplicationVersion("1.0.0");
    app.setStyleSheet(UiStyle::consoleSheet());

    ConsumerWindow window;
    window.showMaximized();
    return app.exec();
}
