// MseedConsumer 看门狗（移植自 MicroseismicMonitoringSystem/WatchdogMain.cpp）。
//
// 控制台启动时以独立进程方式拉起本程序，并把自身 PID 写入 marker 文件：
//   MseedConsumerWatchdog.exe --app <控制台exe> --marker <marker文件> --pid <PID>
//
// 监控规则：
//   * marker 文件存在：控制台应当处于运行状态，进程丢失就重新拉起；
//   * marker 文件被删除：控制台正常退出（托盘“退出”/注销），看门狗随之退出；
//   * 用 QLockFile 保证同一控制台目录只会有一个看门狗实例。
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int CheckIntervalMs = 5000;
constexpr int RestartGraceMs = 5000;

QString argumentValue(const QStringList& arguments, const QString& name)
{
    const int index = arguments.indexOf(name);
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
}

void writeLog(const QString& message)
{
    QFile log(QDir::temp().filePath(QStringLiteral("MseedConsumerWatchdog.log")));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    log.write(QStringLiteral("[%1] %2\n")
                  .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"), message)
                  .toUtf8());
}

bool processIsRunning(qint64 pid)
{
#ifdef Q_OS_WIN
    if (pid <= 0)
        return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process)
        return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

qint64 markerPid(const QString& markerPath, qint64 fallback)
{
    QFile marker(markerPath);
    if (!marker.open(QIODevice::ReadOnly | QIODevice::Text))
        return fallback;
    bool ok = false;
    const qint64 pid = QString::fromLatin1(marker.readAll()).trimmed().toLongLong(&ok);
    return ok ? pid : fallback;
}
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    const QString appPath = argumentValue(arguments, "--app");
    const QString markerPath = argumentValue(arguments, "--marker");
    qint64 monitoredPid = argumentValue(arguments, "--pid").toLongLong();

    if (appPath.isEmpty() || markerPath.isEmpty() || !QFileInfo::exists(appPath))
        return 2;

    const QByteArray appId = QCryptographicHash::hash(appPath.toUtf8(), QCryptographicHash::Sha256)
                                 .toHex().left(16);
    QLockFile lockFile(QDir::temp().filePath(
        QString("mseed_consumer_watchdog_%1.lock").arg(QString::fromLatin1(appId))));
    if (!lockFile.tryLock(100)) {
        writeLog(QStringLiteral("已有看门狗实例在运行，本次退出。app=%1").arg(appPath));
        return 0;
    }

    writeLog(QStringLiteral("看门狗启动，监控 PID %1，marker=%2").arg(monitoredPid).arg(markerPath));

    while (QFileInfo::exists(markerPath)) {
        monitoredPid = markerPid(markerPath, monitoredPid);
        if (!processIsRunning(monitoredPid)) {
            QThread::msleep(1000);
            if (!QFileInfo::exists(markerPath))
                break;

            qint64 restartedPid = 0;
            if (QProcess::startDetached(appPath, {}, QFileInfo(appPath).absolutePath(),
                                        &restartedPid)) {
                monitoredPid = restartedPid;
                writeLog(QStringLiteral("控制台已退出，重新拉起，新 PID %1").arg(restartedPid));
                // 给新进程留出启动并改写 marker 的时间，避免重复拉起。
                QThread::msleep(RestartGraceMs);
                continue;
            }
            writeLog(QStringLiteral("拉起控制台失败：%1").arg(appPath));
        }
        QThread::msleep(CheckIntervalMs);
    }

    writeLog(QStringLiteral("marker 已删除，控制台正常退出，看门狗结束。"));
    return 0;
}
