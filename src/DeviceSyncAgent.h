#ifndef DEVICESYNCAGENT_H
#define DEVICESYNCAGENT_H

#include <QObject>
#include <QMap>
#include <QVector>

class MDataPickder;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class DeviceSyncAgent : public QObject
{
    Q_OBJECT
public:
    explicit DeviceSyncAgent(MDataPickder* pickder, QObject* parent = nullptr);
    void start();

private slots:
    void fetchConfiguration();
    void sendHeartbeat();
    void onRemoteParametersApplied(int version, bool success, const QString& message);
    void onStationConnection(int stationId, bool connected);
    void onChannelStatus(int stationId, int channel, int state, int typeBit);
    void onGpsStatus(int stationId, int gps);
    void onApplyTimeout();

private:
    struct StationState {
        bool connected = false;
        QVector<bool> channels;
        int gps = 0;
        QString ip;
        quint16 port = 0;
    };

    QString serverUrl() const;
    QString endpoint(const QString& suffix) const;
    void postApplyResult(int version, bool success, const QString& message);
    void log(const QString& message);
    void logError(const QString& operation, QNetworkReply* reply);
    QString localAddress() const;

    MDataPickder* m_pickder = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    QTimer* m_configTimer = nullptr;
    QTimer* m_heartbeatTimer = nullptr;
    QTimer* m_applyTimer = nullptr;
    QMap<int, StationState> m_stations;
    int m_appliedVersion = 0;
    int m_pendingVersion = 0;
    bool m_configRequestActive = false;
    bool m_heartbeatRequestActive = false;
    bool m_networkOnline = false;
};

#endif // DEVICESYNCAGENT_H
