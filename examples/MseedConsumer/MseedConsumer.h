#ifndef MSEEDCONSUMER_H
#define MSEEDCONSUMER_H

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

class MseedConsumer : public QObject
{
    Q_OBJECT

public:
    MseedConsumer(const QUrl& serverUrl,
                  const QByteArray& downloadToken,
                  const QString& deviceId,
                  const QString& outputDir,
                  const QString& stateFile,
                  int pollSeconds,
                  QObject* parent = nullptr);

    bool start();
    void stop();

signals:
    void statusChanged(const QString& status);
    void cursorChanged(qint64 cursor);
    void fileDownloaded(qint64 id, const QString& name, const QString& path);
    void logMessage(const QString& message);

private slots:
    void poll();

private:
    void requestNext();
    void beginDownload(qint64 id,
                       const QString& name,
                       qint64 size,
                       const QByteArray& sha256,
                       const QUrl& downloadUrl);
    void finishDownload(QNetworkReply* reply);
    bool loadCursor();
    bool saveCursor(qint64 id);
    bool validateName(const QString& name) const;
    void scheduleNext(int delayMs = -1);
    void fail(const QString& message);

    QUrl m_serverUrl;
    QByteArray m_downloadToken;
    QString m_deviceId;
    QString m_outputDir;
    QString m_stateFile;
    int m_pollMs;
    qint64 m_cursor = 0;
    bool m_busy = false;
    bool m_started = false;

    qint64 m_pendingId = 0;
    qint64 m_expectedSize = 0;
    QByteArray m_expectedSha256;
    QString m_pendingName;
    QString m_partPath;
    QString m_finalPath;
    QFile m_output;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};

    QNetworkAccessManager m_network;
    QTimer m_timer;
};

#endif
