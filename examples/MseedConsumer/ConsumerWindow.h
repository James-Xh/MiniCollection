#ifndef CONSUMERWINDOW_H
#define CONSUMERWINDOW_H

#include <QMainWindow>
#include <memory>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class MseedConsumer;

class ConsumerWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ConsumerWindow(QWidget* parent = nullptr);
    ~ConsumerWindow() override;
private slots:
    void saveSettings();
    void startConsumer();
    void stopConsumer();
    void appendLog(const QString& message);

private:
    void loadSettings();
    void createDefaultConfig() const;
    QString absoluteConfigPath(const QString& value) const;
    void setRunning(bool running);

    QString m_configPath;
    std::unique_ptr<MseedConsumer> m_consumer;

    QLineEdit* m_serverEdit = nullptr;
    QLineEdit* m_tokenEdit = nullptr;
    QLineEdit* m_outputEdit = nullptr;
    QLineEdit* m_stateEdit = nullptr;
    QSpinBox* m_intervalSpin = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_cursorLabel = nullptr;
    QLabel* m_lastFileLabel = nullptr;
    QPlainTextEdit* m_logEdit = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
};

#endif

