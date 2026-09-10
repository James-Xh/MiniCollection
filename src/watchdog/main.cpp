#include <QCoreApplication>
#include <QCommandLineParser>
#include "watchdog.h"
#include <csignal>

// 看门狗（从原项目 src/watchdog 移植）：
// 启动并守护同目录下的 MiniCollection.exe，
// 崩溃退出(CrashExit)时自动重启（默认最多100次，间隔2秒），
// 可选 UDP 心跳检测（默认关闭，与原项目一致）。
static WatchDog *g_watchdog = nullptr;

void signalHandler(int sig)
{
    Q_UNUSED(sig)
    if (g_watchdog) {
        qInfo() << "Received termination signal, stopping watchdog...";
        g_watchdog->stop();
    }
    QCoreApplication::quit();
}

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("MiniCollection Watchdog"));

    // 安装信号处理，确保优雅停止
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#ifndef Q_OS_WIN
    signal(SIGHUP, signalHandler);
#endif

    // 被守护的程序路径（主程序和看门狗在同一目录）
    QString program = QCoreApplication::applicationDirPath() + "/MiniCollection.exe";

    // 配置与原项目一致：最大重启100次，重启间隔2秒，心跳超时20秒
    WatchDog dog(program, QStringList(), 100, 2000, 20000);
    g_watchdog = &dog;

    // 心跳检测（需要主程序发送UDP心跳，默认关闭）
    // dog.setHeartbeatEnabled(true, 12345);

    QObject::connect(&dog, &WatchDog::processStopped, &a, &QCoreApplication::quit, Qt::QueuedConnection);

    dog.start();
    return a.exec();
}
