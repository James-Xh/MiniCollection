#include "DeviceSyncAgent.h"

#include "CallManage.h"
#include "Commdef.h"
#include "DeviceIdentity.h"
#include "MDataPickder.h"
#include "MseedUploader.h"
#include "NetworkReplyTimeout.h"
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

namespace {
const int kSyncIntervalMs = 10000;
// 单次 HTTP 请求的最长等待时间。Qt 5.12 不会自动超时，请求悬挂会让
// m_configRequestActive / m_heartbeatRequestActive 永久为 true，此后不再发请求。
const int kRequestTimeoutMs = 15000;
// 参数交给算法线程后等待应用回执的最长时间，超时就放弃本次版本重新拉取。
const int kApplyTimeoutMs = 60000;
}

DeviceSyncAgent::DeviceSyncAgent(MDataPickder* pickder, QObject* parent)
    : QObject(parent), m_pickder(pickder), m_network(new QNetworkAccessManager(this)),
      m_configTimer(new QTimer(this)), m_heartbeatTimer(new QTimer(this))
{
    const QList<TcpDataManager::StationCfg> stations = TcpDataManager::loadStationConfigs();
    for (const TcpDataManager::StationCfg& station : stations) {
        StationState state;
        state.channels.fill(false, CHANNEL_COUNT);
        state.ip = station.ip;
        state.port = station.port;
        m_stations.insert(station.id, state);
    }

    connect(m_configTimer, &QTimer::timeout, this, &DeviceSyncAgent::fetchConfiguration);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &DeviceSyncAgent::sendHeartbeat);
    m_applyTimer = new QTimer(this);
    m_applyTimer->setSingleShot(true);
    connect(m_applyTimer, &QTimer::timeout, this, &DeviceSyncAgent::onApplyTimeout);
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
    if (serverUrl().isEmpty()) {
        log(QStringLiteral("同步未启用：Config.ini [Upload] serverUrl 为空，"
                           "参数拉取与状态心跳都不会发出"));
    }
    else {
        log(QStringLiteral("同步已启动：设备 %1，服务器 %2，间隔 %3 秒")
            .arg(DeviceIdentity::id(), serverUrl()).arg(kSyncIntervalMs / 1000));
    }
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

static void addProjectToken(QNetworkRequest& request)
{
    // 设备用上传令牌表明自己属于哪个项目。服务端只接受本项目令牌，
    // 因此指向错误项目地址时心跳和参数请求会直接返回 401。
    const QByteArray token = MseedUploader::Instance()->uploadToken();
    if (!token.isEmpty())
        request.setRawHeader("X-Project-Token", token);
}

void DeviceSyncAgent::fetchConfiguration()
{
    if (m_configRequestActive || m_pendingVersion > 0 || serverUrl().isEmpty()) return;
    m_configRequestActive = true;
    // 不使用 after_version 过滤，始终拉取服务端最新配置。版本只用于内存中
    // 判断“是否已经应用过”，不写入 Config.ini；服务端每次下发都会生成新版本，
    // 因此点击下发后设备下一次轮询就会重新应用。
    QNetworkRequest request(QUrl(endpoint("/config/latest")));
    addProjectToken(request);
    QNetworkReply* reply = m_network->get(request);
    NetTimeout::arm(reply, kRequestTimeoutMs);
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
        // 与内存中最后成功应用的版本相同则跳过，避免每 10 秒重复应用。
        // 服务端每次下发都会递增版本，所以下发后 version != m_appliedVersion，
        // 设备会立即重新应用；设备重启后 m_appliedVersion=0，也会先应用一次最新配置。
        if (version <= 0 || version == m_appliedVersion || parameters.isEmpty()) {
            reply->deleteLater();
            return;
        }
        m_networkOnline = true;
        m_pendingVersion = version;
        m_applyTimer->start(kApplyTimeoutMs);
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
    m_applyTimer->stop();
    if (success)
        m_appliedVersion = version;
    m_pendingVersion = 0;
    postApplyResult(version, success, message);
}

void DeviceSyncAgent::onApplyTimeout()
{
    if (m_pendingVersion <= 0) return;
    const int version = m_pendingVersion;
    // 先复位再记录日志：算法线程稍后即使回调，也会因版本不匹配被忽略。
    m_pendingVersion = 0;
    log(QStringLiteral("参数版本 %1 应用超时：%2 秒内算法线程未回报，"
                       "放弃本次同步并重新拉取配置")
            .arg(version).arg(kApplyTimeoutMs / 1000));
}

void DeviceSyncAgent::postApplyResult(int version, bool success, const QString& message)
{
    // 成功回执只上报当前内存中最后应用的版本，避免重试把服务端的
    // applied_config_version 回退成旧值。失败回执照常上报，便于控制台看到状态。
    if (success && version != m_appliedVersion) return;
    QJsonObject body{{"version", version}, {"success", success}, {"message", message}};
    QNetworkRequest request(QUrl(endpoint("/config/applied")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    addProjectToken(request);
    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    NetTimeout::arm(reply, kRequestTimeoutMs);
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
            {"ip", it->ip}, {"port", static_cast<int>(it->port)},
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
    addProjectToken(request);
    m_heartbeatRequestActive = true;
    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    NetTimeout::arm(reply, kRequestTimeoutMs);
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

void DeviceSyncAgent::log(const QString& message)
{
    emit CallManage::getInstance()->sig_addLog("DeviceSync", message);
}

void DeviceSyncAgent::logError(const QString& operation, QNetworkReply* reply)
{
    log(QStringLiteral("%1失败：%2").arg(operation, NetTimeout::describeFailure(reply)));
}
