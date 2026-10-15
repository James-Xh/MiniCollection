#include "DeviceSyncAgent.h"

#include "AppConfig.h"
#include "CallManage.h"
#include "Commdef.h"
#include "DeviceIdentity.h"
#include "MDataPickder.h"
#include "MseedUploader.h"
#include "TcpDataManager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {
const int kSyncIntervalMs = 10000;
}

DeviceSyncAgent::DeviceSyncAgent(MDataPickder* pickder, QObject* parent)
    : QObject(parent), m_pickder(pickder), m_network(new QNetworkAccessManager(this)),
      m_configTimer(new QTimer(this)), m_heartbeatTimer(new QTimer(this))
{
    m_appliedVersion = AppConfig::Instance()
        ->getConfig("Remote", "appliedConfigVersion").toInt();
    const QList<TcpDataManager::StationCfg> stations = TcpDataManager::loadStationConfigs();
    for (const TcpDataManager::StationCfg& station : stations) {
        StationState state;
        state.channels.fill(false, CHANNEL_COUNT);
        m_stations.insert(station.id, state);
    }

    connect(m_configTimer, &QTimer::timeout, this, &DeviceSyncAgent::fetchConfiguration);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &DeviceSyncAgent::sendHeartbeat);
    connect(m_pickder, &MDataPickder::remoteParametersApplied,
        this, &DeviceSyncAgent::onRemoteParametersApplied, Qt::QueuedConnection);
    connect(CallManage::getInstance(), &CallManage::sig_staConnectStatus,
        this, &DeviceSyncAgent::onStationConnection, Qt::QueuedConnection);
    connect(CallManage::getInstance(), &CallManage::sig_devStatus,
        this, &DeviceSyncAgent::onChannelStatus, Qt::QueuedConnection);
    connect(CallManage::getInstance(), &CallManage::sig_gpsStatus,
        this, &DeviceSyncAgent::onGpsStatus, Qt::QueuedConnection);
}

void DeviceSyncAgent::start()
{
    m_configTimer->start(kSyncIntervalMs);
    m_heartbeatTimer->start(kSyncIntervalMs);
    QTimer::singleShot(0, this, &DeviceSyncAgent::fetchConfiguration);
    QTimer::singleShot(1000, this, &DeviceSyncAgent::sendHeartbeat);
}

QString DeviceSyncAgent::serverUrl() const
{
    QString url = MseedUploader::Instance()->serverUrl().trimmed();
    while (url.endsWith('/')) url.chop(1);
    return url;
}

QString DeviceSyncAgent::endpoint(const QString& suffix) const
{
    return serverUrl() + "/coalmine/devices/" + DeviceIdentity::id() + suffix;
}

void DeviceSyncAgent::fetchConfiguration()
{
    if (m_configRequestActive || m_pendingVersion > 0 || serverUrl().isEmpty()) return;
    m_configRequestActive = true;
    QUrl url(endpoint("/config/latest"));
    QUrlQuery query(url);
    query.addQueryItem("after_version", QString::number(m_appliedVersion));
    url.setQuery(query);
    QNetworkReply* reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_configRequestActive = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 204) {
            m_networkOnline = true;
            reply->deleteLater();
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            logError(QStringLiteral("拉取配置"), reply);
            reply->deleteLater();
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject object = document.object();
        const int version = object.value("version").toInt();
        const QVariantMap parameters = object.value("parameters").toObject().toVariantMap();
        if (version <= m_appliedVersion || parameters.isEmpty()) {
            reply->deleteLater();
            return;
        }
        m_networkOnline = true;
        m_pendingVersion = version;
        const QString uploadUrl = object.value("upload_server_url").toString().trimmed();
        if (!uploadUrl.isEmpty())
            MseedUploader::Instance()->applySettings(true, uploadUrl);
        QMetaObject::invokeMethod(m_pickder, "applyRemoteParameters", Qt::QueuedConnection,
            Q_ARG(QVariantMap, parameters), Q_ARG(int, version));
        reply->deleteLater();
    });
}

void DeviceSyncAgent::onRemoteParametersApplied(int version, bool success,
    const QString& message)
{
    if (version != m_pendingVersion) return;
    if (success) {
        m_appliedVersion = version;
        AppConfig::Instance()->setConfig("Remote", "appliedConfigVersion", version);
        AppConfig::Instance()->syncConfig();
    }
    m_pendingVersion = 0;
    postApplyResult(version, success, message);
}

void DeviceSyncAgent::postApplyResult(int version, bool success, const QString& message)
{
    QJsonObject body{{"version", version}, {"success", success}, {"message", message}};
    QNetworkRequest request(QUrl(endpoint("/config/applied")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, version, success, message]() {
        if (reply->error() != QNetworkReply::NoError) {
            logError(QStringLiteral("上报参数应用结果"), reply);
            QTimer::singleShot(kSyncIntervalMs, this,
                [this, version, success, message]() {
                    postApplyResult(version, success, message);
                });
        }
        reply->deleteLater();
    });
}

void DeviceSyncAgent::sendHeartbeat()
{
    if (m_heartbeatRequestActive || serverUrl().isEmpty()) return;
    QJsonArray stations;
    for (auto it = m_stations.constBegin(); it != m_stations.constEnd(); ++it) {
        int onlineChannels = 0;
        for (bool online : it->channels) if (online) ++onlineChannels;
        stations.append(QJsonObject{{"station_number", it.key() + 1},
            {"connected", it->connected}, {"online_channels", onlineChannels},
            {"total_channels", CHANNEL_COUNT}, {"gps", it->gps}});
    }
    QJsonObject body{{"program_running", true},
        {"program_version", QCoreApplication::applicationVersion()},
        {"network_state", m_networkOnline ? "online" : "unknown"},
        {"local_ip", localAddress()}, {"applied_config_version", m_appliedVersion},
        {"stations", stations}};
    QNetworkRequest request(QUrl(endpoint("/heartbeat")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    m_heartbeatRequestActive = true;
    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_heartbeatRequestActive = false;
        m_networkOnline = reply->error() == QNetworkReply::NoError;
        if (!m_networkOnline) logError(QStringLiteral("发送心跳"), reply);
        reply->deleteLater();
    });
}

void DeviceSyncAgent::onStationConnection(int stationId, bool connected)
{
    StationState& state = m_stations[stationId];
    if (state.channels.isEmpty()) state.channels.fill(false, CHANNEL_COUNT);
    state.connected = connected;
    if (!connected) state.channels.fill(false);
}

void DeviceSyncAgent::onChannelStatus(int stationId, int channel, int state, int typeBit)
{
    Q_UNUSED(typeBit);
    StationState& station = m_stations[stationId];
    if (station.channels.isEmpty()) station.channels.fill(false, CHANNEL_COUNT);
    if (channel >= 0 && channel < station.channels.size()) station.channels[channel] = state == 1;
}

void DeviceSyncAgent::onGpsStatus(int stationId, int gps)
{
    StationState& state = m_stations[stationId];
    if (state.channels.isEmpty()) state.channels.fill(false, CHANNEL_COUNT);
    state.gps = gps;
}

QString DeviceSyncAgent::localAddress() const
{
    for (const QHostAddress& address : QNetworkInterface::allAddresses())
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            return address.toString();
    return QString();
}

void DeviceSyncAgent::logError(const QString& operation, QNetworkReply* reply)
{
    emit CallManage::getInstance()->sig_addLog("DeviceSync",
        QStringLiteral("%1失败：HTTP=%2 %3").arg(operation)
            .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
            .arg(reply->errorString()));
}
