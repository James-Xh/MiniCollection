#ifndef BATTERYSERVICE_H
#define BATTERYSERVICE_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPair>
#include <QSqlDatabase>
#include <QVariantMap>
#include <QVector>

class QNetworkAccessManager;
class QTimer;
class ModbusRtuClient;

// 电池控制器采集服务。
//
// 协议：Modbus RTU，9600 8N1，功能码 03。
// 动态数据从 PDU 0x0100 开始，一次读取 0x0100~0x0122 共 35 个寄存器。
// 该服务运行在独立线程中，串口读写、SQLite 缓存和 HTTP 上传都在本线程完成。
class BatteryService : public QObject
{
    Q_OBJECT
public:
    explicit BatteryService(QObject* parent = nullptr);
    ~BatteryService() override;

public slots:
    void start();
    void stop();

signals:
    // 最新一次采集结果，供设备界面和 DeviceSyncAgent 心跳使用。
    // quality=ok 时包含 percent/voltage/current 等字段；
    // quality=error 时包含 message。
    void batterySnapshot(const QVariantMap& snapshot);

private slots:
    void poll();
    void uploadPending();

private:
    void loadConfig();
    bool ensureSerialOpen();
    bool readNominalCapacity();
    bool readDynamicRegisters(QVector<quint16>* registers, QString* error);
    QVariantMap parseRegisters(const QVector<quint16>& registers);
    QVariantMap buildMockSnapshot();
    void enqueue(const QVariantMap& metrics);
    void initDatabase();
    void cleanupDatabase();
    void emitError(const QString& message);
    void log(const QString& message);
    void logThrottled(const QString& message);
    QString serverUrl() const;
    QByteArray uploadToken() const;

    static int decodeTemperature(quint8 value);
    static quint32 combine32(quint16 high, quint16 low);
    double socFromVoltage(double voltage) const;

    ModbusRtuClient* m_modbus = nullptr;
    QTimer* m_pollTimer = nullptr;
    QTimer* m_uploadTimer = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    QSqlDatabase m_db;
    QElapsedTimer m_clock;

    bool m_started = false;
    bool m_enabled = true;
    bool m_mock = false;
    bool m_uploadBusy = false;
    bool m_dbReady = false;
    bool m_capacityRead = false;
    qint64 m_lastErrorLogMs = 0;
    qint64 m_lastCapacityAttemptMs = 0;
    int m_sequence = 0;

    QString m_port;
    int m_baud = 9600;
    char m_parity = 'N';
    int m_dataBits = 8;
    int m_stopBits = 1;
    int m_slave = 1;
    int m_pollSec = 10;
    int m_uploadSec = 60;

    double m_nominalCapacityAh = 100.0;
    int m_batteryType = 2;
    double m_soc = 80.0;
    double m_mockSoc = 80.0;
    QVector<QPair<double, double>> m_ocvCurve;
};

#endif // BATTERYSERVICE_H
