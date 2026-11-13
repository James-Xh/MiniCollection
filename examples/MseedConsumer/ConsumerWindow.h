#ifndef CONSUMERWINDOW_H
#define CONSUMERWINDOW_H
#include <QMainWindow>
#include <QMap>
#include <QByteArray>
#include <QNetworkRequest>
#include <memory>
class QLabel; class QLineEdit; class QListWidget; class QNetworkAccessManager;
class QNetworkReply; class QPlainTextEdit; class QPushButton; class QSpinBox;
class QTableWidget; class QTimer; class MseedConsumer; class QJsonObject;
class QCloseEvent; class QDialog; class QSystemTrayIcon; class QAction;
class ConsumerWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ConsumerWindow(QWidget* parent = nullptr);
    ~ConsumerWindow() override;
protected:
    void closeEvent(QCloseEvent* event) override;
private slots:
    void saveSettings(); void startConsumer(); void stopConsumer();
    void appendLog(const QString& message); void refreshDevices();
    void addDevice(); void removeDevice(); void onDeviceSelected();
    void submitParameters();
    void showServerSettings();
    void quitFromTray();
private:
    void loadSettings(); void loadServerSettings(); void saveServerSettings();
    void loadDeviceNames();
    void createDefaultConfig() const;
    void createServerSettingsDialog();
    void createTrayIcon();
    void showMainWindow();
    void startWatchdog(); void stopWatchdog();
    void applyHeartbeatInterval();
    int heartbeatSeconds() const;
    QString absoluteConfigPath(const QString& value) const;
    QString selectedDeviceId() const; QString apiUrl(const QString& path) const;
    QByteArray controlToken() const;
    QNetworkRequest apiRequest(const QString& path) const;
    void loadProjectInfo();
    void loadOutputDirForDevice(const QString& id);
    void setRunning(bool running); void showDevice(const QJsonObject& object);
    void handleNetworkError(const QString& operation, QNetworkReply* reply);
    void appendLogThrottled(const QString& message);
    void initLogFile();
    void cleanupOldLogs();
    void writeLogFile(const QString& line);
    void logBatteryStatus(const QString& deviceId, const QJsonObject& battery, bool online);
    void checkBatteryAlarm(const QString& deviceId, const QJsonObject& battery);
    QString batterySummary(const QJsonObject& battery, bool online) const;
    QString batteryAlarmText(const QJsonObject& battery) const;
    int batteryAlarmLevel(const QJsonObject& battery) const;
    QString m_configPath; std::unique_ptr<MseedConsumer> m_consumer;
    QString m_consumingDeviceId;
    QString m_lastErrorKey; qint64 m_lastErrorMs = 0;   // 相同网络错误 20 秒内不重复刷屏
    QString m_lastConfigStatusKey;                       // 配置状态只在变化时记录一次
    QString m_formDeviceId;                              // 参数表单当前对应的设备
    QString m_projectName;                               // 当前连接的项目名
    QString m_lastDeviceSnapshot;                        // 设备在线状态快照
    bool m_deviceListLogged = false;                     // 首轮成功日志只打一次
    bool m_lastRefreshFailed = false;                    // 失败恢复后补一条成功日志
    bool m_manualRefreshRequested = false;               // 手动点击刷新
    QString m_lastSelectedLogId;                         // 已记录过选择的设备
    QString m_outputDirDeviceId;                         // 保存目录当前对应的设备
    QMap<QString, QString> m_deviceNames;                 // 设备编号 -> 显示名称
    QString m_logDir;                                    // 控制台日志目录
    int m_logKeepDays = 30;
    QString m_lastBatteryLogKey;
    qint64 m_lastBatteryLogMs = 0;
    QString m_batteryAlarmDevice;
    int m_batteryAlarmLevel = 0;
    qint64 m_lastBatteryAlarmMs = 0;
    bool m_batteryAlarmEnabled = true;
    int m_batteryLowPercent = 20;
    int m_batteryCriticalPercent = 10;
    double m_batteryLowVoltage = 11.8;
    double m_batteryCriticalVoltage = 11.2;
    int m_batteryAlarmRepeatMinutes = 30;
    QString m_statusLogDeviceId;                         // 设备状态日志对应设备
    bool m_statusLogOnline = false;
    bool m_statusLogValid = false;
    bool m_projectInfoLoaded = false;
    bool m_projectInfoRequestActive = false;
    bool m_quitRequested = false;                        // 从托盘“退出”时才真正关闭
    bool m_trayHintShown = false;
    QString m_watchdogMarker;                            // 非空表示看门狗正在守护
    QDialog* m_serverDialog = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
    QAction* m_trayShowAction = nullptr;
    QAction* m_trayQuitAction = nullptr;
    QNetworkAccessManager* m_network = nullptr; QTimer* m_refreshTimer = nullptr;
    QLineEdit *m_serverEdit=nullptr,*m_tokenEdit=nullptr,*m_outputEdit=nullptr,*m_stateEdit=nullptr;
    QSpinBox *m_intervalSpin=nullptr,*m_heartbeatSpin=nullptr; QListWidget* m_deviceList=nullptr;
    QLabel *m_deviceStatus=nullptr,*m_deviceIp=nullptr,*m_programVersion=nullptr,*m_configVersion=nullptr;
    QLabel *m_lastSeen=nullptr,*m_stationHint=nullptr;
    QLabel* m_batteryLabel=nullptr;
    QTableWidget* m_stationTable=nullptr; QMap<QString,QLineEdit*> m_parameterEdits;
    QLineEdit* m_uploadUrlEdit=nullptr; QLabel *m_statusLabel=nullptr,*m_cursorLabel=nullptr,*m_lastFileLabel=nullptr;
    QPlainTextEdit* m_logEdit=nullptr; QPushButton *m_saveButton=nullptr,*m_startButton=nullptr,*m_stopButton=nullptr;
};
#endif
