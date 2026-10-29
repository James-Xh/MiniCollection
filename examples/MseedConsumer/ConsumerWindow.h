#ifndef CONSUMERWINDOW_H
#define CONSUMERWINDOW_H
#include <QMainWindow>
#include <QMap>
#include <QByteArray>
#include <memory>
class QLabel; class QLineEdit; class QListWidget; class QNetworkAccessManager;
class QNetworkReply; class QPlainTextEdit; class QPushButton; class QSpinBox;
class QTableWidget; class QTimer; class MseedConsumer; class QJsonObject;
class QNetworkRequest;
class ConsumerWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ConsumerWindow(QWidget* parent = nullptr);
    ~ConsumerWindow() override;
private slots:
    void saveSettings(); void startConsumer(); void stopConsumer();
    void appendLog(const QString& message); void refreshDevices();
    void addDevice(); void removeDevice(); void onDeviceSelected();
    void submitParameters();
private:
    void loadSettings(); void createDefaultConfig() const;
    QString absoluteConfigPath(const QString& value) const;
    QString selectedDeviceId() const; QString apiUrl(const QString& path) const;
    QByteArray controlToken() const;
    QNetworkRequest apiRequest(const QString& path) const;
    void loadProjectInfo();
    void setRunning(bool running); void showDevice(const QJsonObject& object);
    void handleNetworkError(const QString& operation, QNetworkReply* reply);
    void appendLogThrottled(const QString& message);
    QString m_configPath; std::unique_ptr<MseedConsumer> m_consumer;
    QString m_consumingDeviceId;
    QString m_lastErrorKey; qint64 m_lastErrorMs = 0;   // 相同网络错误 20 秒内不重复刷屏
    QString m_lastConfigStatusKey;                       // 配置状态只在变化时记录一次
    bool m_projectInfoLoaded = false;
    bool m_projectInfoRequestActive = false;
    QNetworkAccessManager* m_network = nullptr; QTimer* m_refreshTimer = nullptr;
    QLineEdit *m_serverEdit=nullptr,*m_tokenEdit=nullptr,*m_projectTokenEdit=nullptr,
        *m_outputEdit=nullptr,*m_stateEdit=nullptr;
    QSpinBox* m_intervalSpin=nullptr; QListWidget* m_deviceList=nullptr;
    QLabel *m_deviceStatus=nullptr,*m_deviceIp=nullptr,*m_programVersion=nullptr,*m_configVersion=nullptr;
    QLabel *m_lastSeen=nullptr,*m_stationHint=nullptr;
    QTableWidget* m_stationTable=nullptr; QMap<QString,QLineEdit*> m_parameterEdits;
    QLineEdit* m_uploadUrlEdit=nullptr; QLabel *m_statusLabel=nullptr,*m_cursorLabel=nullptr,*m_lastFileLabel=nullptr;
    QPlainTextEdit* m_logEdit=nullptr; QPushButton *m_saveButton=nullptr,*m_startButton=nullptr,*m_stopButton=nullptr;
};
#endif
