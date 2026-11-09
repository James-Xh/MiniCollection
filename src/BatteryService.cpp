#include "BatteryService.h"

#include "AppConfig.h"
#include "DeviceIdentity.h"
#include "LogManage.h"
#include "ModbusRtuClient.h"
#include "NetworkReplyTimeout.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace {
// 电池控制器动态数据区：0x0100~0x0122，共 35 个寄存器。
const int kDynamicStart = 0x0100;
const int kDynamicCount = 0x0023;
// EEPROM：蓄电池标称容量、蓄电池类型。
const int kNominalCapacityRegister = 0xE002;
const int kBatteryTypeRegister = 0xE004;
const int kHttpTimeoutMs = 15000;
const qint64 kErrorLogIntervalMs = 60000;
const qint64 kUploadedRetentionMs = 7LL * 24 * 3600 * 1000;
}

BatteryService::BatteryService(QObject* parent)
    : QObject(parent)
{
}

BatteryService::~BatteryService()
{
    stop();
}

void BatteryService::start()
{
    if (m_started)
        return;
    m_started = true;
    m_clock.start();

    loadConfig();
    if (!m_enabled) {
        log(QStringLiteral("电池采集未启用（Config.ini [Battery] enabled=0）"));
        return;
    }

    initDatabase();

    if (!m_mock) {
        ensureSerialOpen();
        if (m_modbus && m_modbus->isOpen()) {
            readNominalCapacity();
            m_lastCapacityAttemptMs = m_clock.elapsed();
        }
    }

    m_network = new QNetworkAccessManager(this);

    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &BatteryService::poll);
    m_pollTimer->start(qMax(1, m_pollSec) * 1000);

    m_uploadTimer = new QTimer(this);
    connect(m_uploadTimer, &QTimer::timeout, this, &BatteryService::uploadPending);
    m_uploadTimer->start(qMax(5, m_uploadSec) * 1000);

    log(QStringLiteral("电池采集已启动：%1，从站 %2，轮询 %3 秒，上传 %4 秒%5")
            .arg(m_mock ? QStringLiteral("模拟数据") : m_port)
            .arg(m_slave)
            .arg(qMax(1, m_pollSec))
            .arg(qMax(5, m_uploadSec))
            .arg(m_mock ? QStringLiteral("（mock=1）") : QString()));

    QTimer::singleShot(0, this, &BatteryService::poll);
}

void BatteryService::stop()
{
    if (!m_started)
        return;
    m_started = false;

    if (m_pollTimer) {
        m_pollTimer->stop();
        delete m_pollTimer;
        m_pollTimer = nullptr;
    }
    if (m_uploadTimer) {
        m_uploadTimer->stop();
        delete m_uploadTimer;
        m_uploadTimer = nullptr;
    }
    if (m_modbus) {
        m_modbus->close();
        delete m_modbus;
        m_modbus = nullptr;
    }
    if (m_network) {
        delete m_network;
        m_network = nullptr;
    }

    cleanupDatabase();
}

void BatteryService::loadConfig()
{
    // QSettings 实例必须属于当前线程，不能复用 AppConfig 里的实例。
    QSettings settings(AppConfig::Instance()->configFilePath(), QSettings::IniFormat);

    m_enabled = settings.value("Battery/enabled", 1).toInt() != 0;
    m_mock = settings.value("Battery/mock", 0).toInt() != 0;
    m_port = settings.value("Battery/port", "/dev/ttyS1").toString().trimmed();
    m_baud = settings.value("Battery/baud", 9600).toInt();
    m_parity = settings.value("Battery/parity", "N").toString().trimmed().isEmpty()
                   ? 'N'
                   : settings.value("Battery/parity", "N").toString().trimmed().at(0).toLatin1();
    m_dataBits = settings.value("Battery/dataBits", 8).toInt();
    m_stopBits = settings.value("Battery/stopBits", 1).toInt();
    m_slave = qBound(1, settings.value("Battery/slave", 1).toInt(), 247);
    m_pollSec = qMax(1, settings.value("Battery/pollSec", 10).toInt());
    m_uploadSec = qMax(5, settings.value("Battery/uploadSec", 60).toInt());

    m_nominalCapacityAh = settings.value("Battery/nominalCapacityAh", 0).toDouble();
    m_batteryType = settings.value("Battery/batteryType", 2).toInt();
    m_soc = qBound(0.0, settings.value("Battery/initialSoc", 80).toDouble(), 100.0);
    m_mockSoc = m_soc;
    m_capacityRead = m_nominalCapacityAh > 0;

    m_ocvCurve.clear();
    const QString curve = settings.value(
        "Battery/ocvCurve",
        "11.8:0,12.0:20,12.2:40,12.4:60,12.6:80,12.8:100").toString();
    const QStringList points = curve.split(',', QString::SkipEmptyParts);
    for (const QString& point : points) {
        const QStringList pair = point.split(':');
        if (pair.size() != 2)
            continue;
        bool okV = false;
        bool okS = false;
        const double voltage = pair.at(0).trimmed().toDouble(&okV);
        const double soc = pair.at(1).trimmed().toDouble(&okS);
        if (okV && okS)
            m_ocvCurve.append(qMakePair(voltage, soc));
    }
    std::sort(m_ocvCurve.begin(), m_ocvCurve.end(),
              [](const QPair<double, double>& a, const QPair<double, double>& b) {
                  return a.first < b.first;
              });
}

bool BatteryService::ensureSerialOpen()
{
    if (m_mock)
        return true;
    if (m_modbus && m_modbus->isOpen())
        return true;

    if (!m_modbus)
        m_modbus = new ModbusRtuClient;

    QString error;
    if (!m_modbus->open(m_port, m_baud, m_parity, m_dataBits, m_stopBits, &error)) {
        logThrottled(error);
        return false;
    }

    log(QStringLiteral("电池串口已打开：%1，%2 %3%4%5")
            .arg(m_port)
            .arg(m_baud)
            .arg(m_dataBits)
            .arg(m_parity)
            .arg(m_stopBits));
    return true;
}

bool BatteryService::readNominalCapacity()
{
    if (!m_modbus || !m_modbus->isOpen())
        return false;

    QVector<quint16> registers;
    QString error;

    if (m_modbus->readHoldingRegisters(m_slave, kNominalCapacityRegister, 1,
                                       &registers, &error)
        && registers.size() == 1) {
        const int capacity = registers.at(0);
        if (capacity > 0 && capacity <= 100000) {
            m_nominalCapacityAh = capacity;
            m_capacityRead = true;
        }
    }
    else {
        logThrottled(QStringLiteral("读取蓄电池标称容量失败：%1").arg(error));
    }

    registers.clear();
    if (m_modbus->readHoldingRegisters(m_slave, kBatteryTypeRegister, 1,
                                       &registers, &error)
        && registers.size() == 1) {
        m_batteryType = registers.at(0);
    }
    else {
        logThrottled(QStringLiteral("读取蓄电池类型失败：%1").arg(error));
    }

    log(QStringLiteral("电池参数：标称容量 %1Ah，类型 %2")
            .arg(m_nominalCapacityAh, 0, 'f', 0)
            .arg(m_batteryType));
    return true;
}

bool BatteryService::readDynamicRegisters(QVector<quint16>* registers, QString* error)
{
    if (!m_modbus || !m_modbus->isOpen()) {
        if (error)
            *error = QStringLiteral("电池串口未打开");
        return false;
    }
    return m_modbus->readHoldingRegisters(m_slave, kDynamicStart, kDynamicCount,
                                          registers, error);
}

QVariantMap BatteryService::buildMockSnapshot()
{
    // 仅用于没有硬件的开发环境：模拟一个缓慢放电的 12V 电池。
    m_mockSoc = qBound(0.0, m_mockSoc - 0.02, 100.0);
    const double voltage = 11.8 + m_mockSoc * 0.01;

    QVariantMap metrics;
    metrics.insert("percent", m_mockSoc);
    metrics.insert("voltage", voltage);
    metrics.insert("current", 2.5);
    metrics.insert("charge_current", 2.5);
    metrics.insert("load_voltage", voltage - 0.1);
    metrics.insert("load_current", 1.2);
    metrics.insert("load_power", 15);
    metrics.insert("solar_voltage", 18.0);
    metrics.insert("solar_current", 2.5);
    metrics.insert("charge_power", 45);
    metrics.insert("battery_temp", 25);
    metrics.insert("controller_temp", 28);
    const double capacityAh = m_nominalCapacityAh > 0 ? m_nominalCapacityAh : 100.0;
    metrics.insert("remaining_ah", m_mockSoc / 100.0 * capacityAh);
    metrics.insert("nominal_capacity_ah", capacityAh);
    metrics.insert("load_on", true);
    metrics.insert("load_brightness", 100);
    metrics.insert("charge_state", 2);
    metrics.insert("fault_high", 0);
    metrics.insert("fault_low", 0);
    metrics.insert("has_fault", false);
    metrics.insert("battery_type", m_batteryType);
    return metrics;
}

QVariantMap BatteryService::parseRegisters(const QVector<quint16>& registers)
{
    QVariantMap metrics;
    if (registers.size() < kDynamicCount) {
        metrics.insert("quality", "error");
        metrics.insert("message", QStringLiteral("寄存器数量不足"));
        return metrics;
    }

    // 0x0100：高 8 位预留，低 8 位 SOC。
    const int rawSoc = registers.at(0x00) & 0xFF;
    const double voltage = registers.at(0x01) * 0.1;
    if (rawSoc >= 0 && rawSoc <= 100) {
        m_soc = rawSoc;
    }
    else {
        const double fallback = socFromVoltage(voltage);
        if (fallback >= 0)
            m_soc = fallback;
    }

    const double chargeCurrent = registers.at(0x02) * 0.01;
    const int controllerTemp = decodeTemperature(quint8(registers.at(0x03) >> 8));
    const int batteryTemp = decodeTemperature(quint8(registers.at(0x03) & 0xFF));
    const double loadVoltage = registers.at(0x04) * 0.1;
    const double loadCurrent = registers.at(0x05) * 0.01;
    const int loadPower = registers.at(0x06);
    const double solarVoltage = registers.at(0x07) * 0.1;
    const double solarCurrent = registers.at(0x08) * 0.01;
    const int chargePower = registers.at(0x09);

    const quint16 loadStatus = registers.at(0x20);
    const quint8 loadHigh = quint8(loadStatus >> 8);
    const bool loadOn = (loadHigh & 0x80) != 0;
    const int loadBrightness = loadHigh & 0x7F;
    const int chargeState = loadStatus & 0xFF;

    const quint16 faultHigh = registers.at(0x21);
    const quint16 faultLow = registers.at(0x22);

    metrics.insert("percent", m_soc);
    metrics.insert("voltage", voltage);
    metrics.insert("current", chargeCurrent);
    metrics.insert("charge_current", chargeCurrent);
    metrics.insert("load_voltage", loadVoltage);
    metrics.insert("load_current", loadCurrent);
    metrics.insert("load_power", loadPower);
    metrics.insert("solar_voltage", solarVoltage);
    metrics.insert("solar_current", solarCurrent);
    metrics.insert("charge_power", chargePower);
    metrics.insert("battery_temp", batteryTemp);
    metrics.insert("controller_temp", controllerTemp);
    const double capacityAh = m_nominalCapacityAh > 0 ? m_nominalCapacityAh : 100.0;
    metrics.insert("remaining_ah", m_soc / 100.0 * capacityAh);
    metrics.insert("nominal_capacity_ah", capacityAh);
    metrics.insert("load_on", loadOn);
    metrics.insert("load_brightness", loadBrightness);
    metrics.insert("charge_state", chargeState);
    metrics.insert("fault_high", faultHigh);
    metrics.insert("fault_low", faultLow);
    metrics.insert("has_fault", faultHigh != 0 || faultLow != 0);
    metrics.insert("battery_type", m_batteryType);

    metrics.insert("day_min_voltage", registers.at(0x0B) * 0.1);
    metrics.insert("day_max_voltage", registers.at(0x0C) * 0.1);
    metrics.insert("day_max_charge_current", registers.at(0x0D) * 0.01);
    metrics.insert("day_max_discharge_current", registers.at(0x0E) * 0.01);
    metrics.insert("day_max_charge_power", registers.at(0x0F));
    metrics.insert("day_max_discharge_power", registers.at(0x10));
    metrics.insert("day_charge_ah", registers.at(0x11));
    metrics.insert("day_discharge_ah", registers.at(0x12));
    metrics.insert("day_generation", registers.at(0x13));
    metrics.insert("day_consumption", registers.at(0x14));
    metrics.insert("total_run_days", registers.at(0x15));
    metrics.insert("overdischarge_count", registers.at(0x16));
    metrics.insert("full_charge_count", registers.at(0x17));
    metrics.insert("total_charge_ah",
                   combine32(registers.at(0x18), registers.at(0x19)));
    metrics.insert("total_discharge_ah",
                   combine32(registers.at(0x1A), registers.at(0x1B)));
    metrics.insert("total_generation",
                   combine32(registers.at(0x1C), registers.at(0x1D)));
    metrics.insert("total_consumption",
                   combine32(registers.at(0x1E), registers.at(0x1F)));

    return metrics;
}

void BatteryService::poll()
{
    if (!m_enabled || !m_started)
        return;

    QVariantMap metrics;
    if (m_mock) {
        metrics = buildMockSnapshot();
    }
    else {
        if (!ensureSerialOpen()) {
            emitError(QStringLiteral("电池串口未打开：%1").arg(m_port));
            return;
        }

        // 启动时串口没打开成功的话，后续首次打开时补读标称容量。
        if (!m_capacityRead
            && m_clock.elapsed() - m_lastCapacityAttemptMs > 60000) {
            readNominalCapacity();
            m_lastCapacityAttemptMs = m_clock.elapsed();
        }

        QVector<quint16> registers;
        QString error;
        if (!readDynamicRegisters(&registers, &error)) {
            emitError(error);
            return;
        }
        metrics = parseRegisters(registers);
        if (metrics.value("quality").toString() == "error") {
            emitError(metrics.value("message").toString());
            return;
        }
    }

    enqueue(metrics);

    QVariantMap snapshot = metrics;
    snapshot.insert("ts", QDateTime::currentMSecsSinceEpoch());
    snapshot.insert("quality", "ok");
    snapshot.insert("message", QString());
    emit batterySnapshot(snapshot);
}

void BatteryService::emitError(const QString& message)
{
    logThrottled(message);
    QVariantMap snapshot;
    snapshot.insert("ts", QDateTime::currentMSecsSinceEpoch());
    snapshot.insert("quality", "error");
    snapshot.insert("message", message);
    emit batterySnapshot(snapshot);
}

void BatteryService::initDatabase()
{
    const QString path = AppConfig::Instance()->getSensorPath() + "battery.db";
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = QSqlDatabase::addDatabase("QSQLITE", "battery_sensors");
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        log(QStringLiteral("打开电池缓存数据库失败：%1（%2）")
                .arg(path, m_db.lastError().text()));
        return;
    }

    QSqlQuery query(m_db);
    if (!query.exec(
            "CREATE TABLE IF NOT EXISTS pending_samples ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " uid TEXT NOT NULL UNIQUE,"
            " ts_ms INTEGER NOT NULL,"
            " sensor_type TEXT NOT NULL,"
            " sensor_id TEXT NOT NULL,"
            " metrics_json TEXT NOT NULL,"
            " quality TEXT NOT NULL,"
            " uploaded INTEGER NOT NULL DEFAULT 0)")) {
        log(QStringLiteral("创建电池缓存表失败：%1").arg(query.lastError().text()));
        return;
    }
    query.exec("CREATE INDEX IF NOT EXISTS idx_pending_uploaded"
               " ON pending_samples(uploaded, ts_ms)");
    m_dbReady = true;
}

void BatteryService::cleanupDatabase()
{
    if (!QSqlDatabase::contains("battery_sensors")) {
        m_dbReady = false;
        return;
    }
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase("battery_sensors");
    m_dbReady = false;
}

void BatteryService::enqueue(const QVariantMap& metrics)
{
    if (!m_dbReady || !m_db.isOpen())
        return;

    const qint64 ts = QDateTime::currentMSecsSinceEpoch();
    const QString uid = QStringLiteral("%1-%2-%3-battery-main")
                            .arg(DeviceIdentity::id())
                            .arg(ts)
                            .arg(++m_sequence);

    const QJsonObject object = QJsonObject::fromVariantMap(metrics);
    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QSqlQuery query(m_db);
    query.prepare(
        "INSERT OR IGNORE INTO pending_samples"
        " (uid, ts_ms, sensor_type, sensor_id, metrics_json, quality, uploaded)"
        " VALUES (?, ?, 'battery', 'main', ?, 'ok', 0)");
    query.addBindValue(uid);
    query.addBindValue(ts);
    query.addBindValue(QString::fromUtf8(json));
    if (!query.exec())
        logThrottled(QStringLiteral("写入电池缓存失败：%1").arg(query.lastError().text()));
}

void BatteryService::uploadPending()
{
    if (!m_started || m_uploadBusy || !m_dbReady || !m_db.isOpen())
        return;

    const QString url = serverUrl();
    if (url.isEmpty())
        return;

    QSqlQuery query(m_db);
    query.prepare(
        "SELECT uid, ts_ms, sensor_type, sensor_id, metrics_json, quality"
        " FROM pending_samples WHERE uploaded = 0"
        " ORDER BY ts_ms ASC LIMIT 100");
    if (!query.exec())
        return;

    QJsonArray samples;
    QStringList uids;
    while (query.next()) {
        const QJsonObject metrics = QJsonDocument::fromJson(
            query.value(4).toString().toUtf8()).object();
        QJsonObject sample;
        sample.insert("uid", query.value(0).toString());
        sample.insert("ts", query.value(1).toLongLong());
        sample.insert("sensor_type", query.value(2).toString());
        sample.insert("sensor_id", query.value(3).toString());
        sample.insert("quality", query.value(5).toString());
        sample.insert("metrics", metrics);
        samples.append(sample);
        uids.append(query.value(0).toString());
    }
    query.finish();
    if (samples.isEmpty())
        return;

    QJsonObject body;
    body.insert("samples", samples);

    QNetworkRequest request(QUrl(url + "/coalmine/devices/"
                                 + DeviceIdentity::id() + "/sensors"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("X-Project-Token", uploadToken());

    m_uploadBusy = true;
    QNetworkReply* reply = m_network->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    NetTimeout::arm(reply, kHttpTimeoutMs);

    connect(reply, &QNetworkReply::finished, this, [this, reply, uids]() {
        m_uploadBusy = false;
        const int httpStatus = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError
            && httpStatus >= 200 && httpStatus < 300) {
            QSqlQuery update(m_db);
            update.prepare("UPDATE pending_samples SET uploaded = 1 WHERE uid = ?");
            for (const QString& uid : uids) {
                update.bindValue(0, uid);
                update.exec();
            }
            QSqlQuery cleanup(m_db);
            cleanup.prepare("DELETE FROM pending_samples"
                            " WHERE uploaded = 1 AND ts_ms < ?");
            cleanup.addBindValue(QDateTime::currentMSecsSinceEpoch()
                                 - kUploadedRetentionMs);
            cleanup.exec();
        }
        else {
            logThrottled(QStringLiteral("上传电池数据失败：%1")
                             .arg(NetTimeout::describeFailure(reply)));
        }
        reply->deleteLater();
    });
}

QString BatteryService::serverUrl() const
{
    QSettings settings(AppConfig::Instance()->configFilePath(), QSettings::IniFormat);
    QString url = settings.value("Upload/serverUrl").toString().trimmed();
    while (url.endsWith('/'))
        url.chop(1);
    return url;
}

QByteArray BatteryService::uploadToken() const
{
    QSettings settings(AppConfig::Instance()->configFilePath(), QSettings::IniFormat);
    return settings.value("Upload/token").toString().trimmed().toUtf8();
}

int BatteryService::decodeTemperature(quint8 value)
{
    const bool negative = (value & 0x80) != 0;
    const int magnitude = value & 0x7F;
    return negative ? -magnitude : magnitude;
}

quint32 BatteryService::combine32(quint16 high, quint16 low)
{
    return (quint32(high) << 16) | quint32(low);
}

double BatteryService::socFromVoltage(double voltage) const
{
    if (m_ocvCurve.isEmpty())
        return -1.0;
    if (voltage <= m_ocvCurve.first().first)
        return m_ocvCurve.first().second;
    if (voltage >= m_ocvCurve.last().first)
        return m_ocvCurve.last().second;

    for (int i = 1; i < m_ocvCurve.size(); ++i) {
        const QPair<double, double>& low = m_ocvCurve.at(i - 1);
        const QPair<double, double>& high = m_ocvCurve.at(i);
        if (voltage <= high.first) {
            const double span = high.first - low.first;
            if (span <= 0.0)
                return high.second;
            const double ratio = (voltage - low.first) / span;
            return low.second + (high.second - low.second) * ratio;
        }
    }
    return m_ocvCurve.last().second;
}

void BatteryService::log(const QString& message)
{
    LOGMgr->addLog("Battery", message);
}

void BatteryService::logThrottled(const QString& message)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastErrorLogMs < kErrorLogIntervalMs)
        return;
    m_lastErrorLogMs = now;
    log(message);
}
