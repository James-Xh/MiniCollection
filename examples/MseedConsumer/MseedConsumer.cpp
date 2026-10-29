#include "MseedConsumer.h"

#include "NetworkReplyTimeout.h"

#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrlQuery>
#include <QDebug>

namespace {
// 查询下一个文件是轻量请求；超时后必须让 m_busy 复位，否则消费会永久停住。
const int kPollTimeoutMs = 20000;
// 单个事件文件下载的兜底超时，避免半开连接把消费卡死。
const int kDownloadTimeoutMs = 120000;
}

MseedConsumer::MseedConsumer(const QUrl& serverUrl,
                             const QByteArray& downloadToken,
                             const QString& deviceId,
                             const QString& outputDir,
                             const QString& stateFile,
                             int pollSeconds,
                             QObject* parent)
    : QObject(parent)
    , m_serverUrl(serverUrl)
    , m_downloadToken(downloadToken)
    , m_deviceId(deviceId)
    , m_outputDir(QDir(outputDir).absolutePath())
    , m_stateFile(QFileInfo(stateFile).absoluteFilePath())
    , m_pollMs(qMax(1, pollSeconds) * 1000)
{
    while (m_serverUrl.path().endsWith('/')) {
        QString path = m_serverUrl.path();
        path.chop(1);
        m_serverUrl.setPath(path);
    }
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &MseedConsumer::poll);
}

bool MseedConsumer::start()
{
    if (m_started)
        return true;
    if (!m_serverUrl.isValid() || m_serverUrl.scheme().isEmpty()) {
        qCritical() << "Invalid server URL:" << m_serverUrl;
        emit logMessage(QStringLiteral("启动失败：服务器地址无效 %1").arg(m_serverUrl.toString()));
        return false;
    }
    if (!QDir().mkpath(m_outputDir)) {
        qCritical() << "Cannot create output directory:" << m_outputDir;
        emit logMessage(QStringLiteral("启动失败：无法创建输出目录 %1").arg(m_outputDir));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(m_stateFile).absolutePath())) {
        qCritical() << "Cannot create state directory:" << m_stateFile;
        emit logMessage(QStringLiteral("启动失败：无法创建游标目录 %1").arg(m_stateFile));
        return false;
    }
    if (!loadCursor()) {
        emit logMessage(QStringLiteral("启动失败：无法读取游标文件 %1").arg(m_stateFile));
        return false;
    }

    qInfo() << "MseedConsumer started, cursor=" << m_cursor
            << "output=" << m_outputDir;
    m_started = true;
    emit cursorChanged(m_cursor);
    emit statusChanged(QStringLiteral("运行中，等待服务器文件"));
    emit logMessage(QStringLiteral("数据获取服务启动，当前游标：%1，服务器：%2，设备：%3，游标文件：%4")
                        .arg(m_cursor).arg(m_serverUrl.toString()).arg(m_deviceId).arg(m_stateFile));
    scheduleNext(0);
    return true;
}

void MseedConsumer::stop()
{
    m_started = false;
    m_timer.stop();
    emit statusChanged(QStringLiteral("已停止"));
    emit logMessage(QStringLiteral("设备 %1 数据获取服务已停止").arg(m_deviceId));
}

void MseedConsumer::poll()
{
    if (m_started && !m_busy)
        requestNext();
}

void MseedConsumer::requestNext()
{
    QUrl url = m_serverUrl;
    url.setPath(url.path() + "/coalmine/mseed/" + m_deviceId + "/next");
    QUrlQuery query;
    query.addQueryItem("after_id", QString::number(m_cursor));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("X-Download-Token", m_downloadToken);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    m_busy = true;
    QNetworkReply* reply = m_network.get(request);
    NetTimeout::arm(reply, kPollTimeoutMs);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString failure = NetTimeout::describeFailure(reply);
        reply->deleteLater();

        if (status == 204) {
            m_busy = false;
            emit statusChanged(QStringLiteral("暂无新文件，%1秒后重试").arg(m_pollMs / 1000));
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastNoFileLogMs >= 60000) {
                m_lastNoFileLogMs = now;
                emit logMessage(QStringLiteral("暂无新文件，当前游标：%1").arg(m_cursor));
            }
            scheduleNext();
            return;
        }
        if (error != QNetworkReply::NoError || status != 200) {
            fail(QStringLiteral("查询失败：%1").arg(failure));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("服务器返回的JSON无效"));
            return;
        }

        const QJsonObject object = document.object();
        if (object.value("device_id").toString() != m_deviceId) {
            fail(QStringLiteral("服务器返回了其它设备的文件"));
            return;
        }
        const qint64 id = object.value("id").toVariant().toLongLong();
        const QString name = object.value("name").toString();
        const qint64 size = object.value("size").toVariant().toLongLong();
        const QByteArray sha256 = object.value("sha256").toString().toLatin1().toLower();
        const QUrl advertisedUrl(object.value("download_url").toString());
        // The server may expose an internal download URL (for example
        // http://127.0.0.1:9001/...).  The consumer must always connect to
        // the configured public endpoint and only use the returned path.
        QUrl downloadUrl = m_serverUrl;
        if (advertisedUrl.isValid() && !advertisedUrl.path().isEmpty()) {
            downloadUrl.setPath(advertisedUrl.path());
            downloadUrl.setQuery(advertisedUrl.query());
        }
        if (id <= m_cursor || size < 0 || sha256.size() != 64 ||
            !validateName(name) || !advertisedUrl.isValid() ||
            advertisedUrl.path().isEmpty() || !downloadUrl.isValid()) {
            fail(QStringLiteral("服务器返回的文件元数据无效"));
            return;
        }
        beginDownload(id, name, size, sha256, downloadUrl);
    });
}

void MseedConsumer::beginDownload(qint64 id,
                                  const QString& name,
                                  qint64 size,
                                  const QByteArray& sha256,
                                  const QUrl& downloadUrl)
{
    m_pendingId = id;
    m_pendingName = name;
    m_expectedSize = size;
    m_expectedSha256 = sha256;
    m_finalPath = QDir(m_outputDir).absoluteFilePath(name);
    m_partPath = m_finalPath + ".part";
    m_hash.reset();
    emit statusChanged(QStringLiteral("正在下载：%1").arg(name));
    emit logMessage(QStringLiteral("发现新文件，开始下载：ID=%1，名称=%2，大小=%3 字节")
                        .arg(id).arg(name).arg(size));

    QFile::remove(m_partPath);
    m_output.setFileName(m_partPath);
    if (!m_output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("无法创建临时文件: %1").arg(m_partPath));
        return;
    }

    QNetworkRequest request(downloadUrl);
    request.setRawHeader("X-Download-Token", m_downloadToken);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    QNetworkReply* reply = m_network.get(request);
    NetTimeout::arm(reply, kDownloadTimeoutMs);

    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        if (!data.isEmpty()) {
            if (m_output.write(data) != data.size()) {
                reply->abort();
                return;
            }
            m_hash.addData(data);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        finishDownload(reply);
    });
}

void MseedConsumer::finishDownload(QNetworkReply* reply)
{
    const QByteArray remaining = reply->readAll();
    if (!remaining.isEmpty()) {
        m_output.write(remaining);
        m_hash.addData(remaining);
    }
    m_output.flush();
    m_output.close();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError error = reply->error();
    const QString errorText = reply->errorString();
    const bool timedOut = NetTimeout::timedOut(reply);
    const QString downloadUrl = reply->url().toString();
    reply->deleteLater();

    const qint64 actualSize = QFileInfo(m_partPath).size();
    const QByteArray actualHash = m_hash.result().toHex();

    if (error != QNetworkReply::NoError || status != 200 ||
        actualSize != m_expectedSize || actualHash != m_expectedSha256) {
        QFile::remove(m_partPath);
        fail(QStringLiteral("下载校验失败: %1 HTTP=%2 size=%3/%4 hash=%5/%6 error=%7 url=%8 timeout=%9")
                 .arg(m_pendingName).arg(status)
                 .arg(actualSize).arg(m_expectedSize)
                 .arg(QString::fromLatin1(actualHash))
                 .arg(QString::fromLatin1(m_expectedSha256))
                 .arg(errorText)
                 .arg(downloadUrl)
                 .arg(timedOut ? QStringLiteral("yes") : QStringLiteral("no")));
        return;
    }

    if (QFile::exists(m_finalPath) && !QFile::remove(m_finalPath)) {
        fail(QStringLiteral("无法替换已有文件: %1").arg(m_finalPath));
        return;
    }
    if (!QFile::rename(m_partPath, m_finalPath)) {
        fail(QStringLiteral("无法发布完整文件: %1").arg(m_finalPath));
        return;
    }

    // 在这里调用业务处理逻辑。只有业务成功后才能保存游标。
    qInfo() << "Downloaded complete mseed: id=" << m_pendingId
            << "file=" << m_finalPath;
    if (!saveCursor(m_pendingId)) {
        fail(QStringLiteral("无法保存数据获取游标"));
        return;
    }
    m_cursor = m_pendingId;
    emit cursorChanged(m_cursor);
    emit fileDownloaded(m_pendingId, m_pendingName, m_finalPath);
    emit statusChanged(QStringLiteral("下载完成：%1").arg(m_pendingName));
    emit logMessage(QStringLiteral("下载完成：ID=%1，文件=%2，游标已推进到 %3，保存路径=%4")
                        .arg(m_pendingId).arg(m_pendingName).arg(m_cursor).arg(m_finalPath));
    m_busy = false;
    scheduleNext(0);
}

bool MseedConsumer::loadCursor()
{
    QFile file(m_stateFile);
    if (!file.exists()) {
        m_cursor = 0;
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical() << "Cannot read cursor file:" << m_stateFile;
        return false;
    }
    bool ok = false;
    const qint64 value = QString::fromUtf8(file.readAll()).trimmed().toLongLong(&ok);
    if (!ok || value < 0) {
        qCritical() << "Invalid cursor file:" << m_stateFile;
        return false;
    }
    m_cursor = value;
    return true;
}

bool MseedConsumer::saveCursor(qint64 id)
{
    QSaveFile file(m_stateFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    if (file.write(QByteArray::number(id) + "\n") < 0)
        return false;
    return file.commit();
}

bool MseedConsumer::validateName(const QString& name) const
{
    return !name.isEmpty() && name == QFileInfo(name).fileName()
        && name.toLower().endsWith(".mseed");
}

void MseedConsumer::scheduleNext(int delayMs)
{
    if (m_started)
        m_timer.start(delayMs >= 0 ? delayMs : m_pollMs);
}

void MseedConsumer::fail(const QString& message)
{
    if (m_output.isOpen())
        m_output.close();
    qWarning().noquote() << message;
    emit statusChanged(QStringLiteral("请求失败，稍后重试"));
    emit logMessage(message);
    m_busy = false;
    scheduleNext();
}
