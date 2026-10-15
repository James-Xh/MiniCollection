#ifndef CONSUMERWINDOW_H
#define CONSUMERWINDOW_H
#include <QMainWindow>
#include <QMap>
#include <memory>
class QLabel; class QLineEdit; class QListWidget; class QNetworkAccessManager;
class QNetworkReply; class QPlainTextEdit; class QPushButton; class QSpinBox;
class QTableWidget; class QTimer; class MseedConsumer; class QJsonObject;
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
    void setRunning(bool running); void showDevice(const QJsonObject& object);
    void handleNetworkError(const QString& operation, QNetworkReply* reply);
    QString m_configPath; std::unique_ptr<MseedConsumer> m_consumer;
    QString m_consumingDeviceId;
    QNetworkAccessManager* m_network = nullptr; QTimer* m_refreshTimer = nullptr;
    QLineEdit *m_serverEdit=nullptr,*m_tokenEdit=nullptr,*m_outputEdit=nullptr,*m_stateEdit=nullptr;
    QSpinBox* m_intervalSpin=nullptr; QListWidget* m_deviceList=nullptr;
    QLabel *m_deviceStatus=nullptr,*m_deviceIp=nullptr,*m_programVersion=nullptr,*m_configVersion=nullptr;
    QTableWidget* m_stationTable=nullptr; QMap<QString,QLineEdit*> m_parameterEdits;
    QLineEdit* m_uploadUrlEdit=nullptr; QLabel *m_statusLabel=nullptr,*m_cursorLabel=nullptr,*m_lastFileLabel=nullptr;
    QPlainTextEdit* m_logEdit=nullptr; QPushButton *m_saveButton=nullptr,*m_startButton=nullptr,*m_stopButton=nullptr;
};
#endif
