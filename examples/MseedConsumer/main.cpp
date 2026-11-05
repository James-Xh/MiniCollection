#include "ConsumerWindow.h"
#include "UiStyle.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("地面微震采集控制台"));
    app.setApplicationVersion("1.0.0");
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));
    app.setStyleSheet(UiStyle::consoleSheet());
    // 关闭主窗口只是隐藏到托盘，不能因此退出进程。
    app.setQuitOnLastWindowClosed(false);

    ConsumerWindow window;
    window.showMaximized();
    return app.exec();
}
